#pragma once
//
// Job decomposition and the task queue — step 1.14.
//
// In-memory only. SQLite durability lands in 2.19; adaptive sizing in 2.12.
// Everything here is deliberately the simplest thing that lets one worker
// complete a real job, because G1 is about the PROTOCOL shape being right, not
// about the scheduler being good.

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstddef>
#include <span>
#include <vector>

#include "p2pgpu/coordinator/affinity.hpp"
#include "p2pgpu/coordinator/composite.hpp"
#include "p2pgpu/coordinator/task_state.hpp"
#include "p2pgpu/protocol/error.hpp"
#include "p2pgpu/protocol/ids.hpp"

namespace p2pgpu::coordinator {

using protocol::JobId;
using protocol::TaskId;
using protocol::WorkerId;

struct Task {
    TaskId id;
    JobId job;
    TaskState state = TaskState::Queued;

    /// The kernel's chunkable range (K1). `work_units` is hardcoded at this
    /// step — the sizer computes it per worker from 2.12.
    std::uint64_t start_unit = 0;
    std::uint64_t unit_count = 0;

    /// Nil unless state == Leased. Not an optional because a nil ID already
    /// means "absent" and two ways to say the same thing invites disagreement.
    WorkerId holder;

    /// Set on grant; the sweep in 2.7 uses it. Coordinator clock, absolute —
    /// worker clocks are never trusted (PROTOCOL.md §5).
    std::uint64_t lease_expires_at_ms = 0;

    /// Set when this task is a speculative replica of another (2.17, D-0045).
    /// Nil for an original. Two tasks over one range is what is ACTUALLY true —
    /// two workers are computing the same thing — and modelling it as one task
    /// with two holders would make "who holds this lease" ambiguous, which is
    /// the question invariants 5 and 6 exist to answer.
    TaskId replica_of;

    /// One worker's answer, kept until the group resolves (D-0054).
    ///
    /// For `Exact` only `checksum` is populated — bitwise equality and checksum
    /// equality are the same question, and the checksum is already computed for
    /// invariant 9, so integer replication costs 8 bytes rather than up to
    /// 8 MiB. `payload` is filled only for classes where "close enough" cannot
    /// be answered from a hash.
    struct Submission {
        WorkerId worker;
        std::uint64_t checksum = 0;
        std::vector<std::byte> payload;
    };

    /// Answers received so far. Cleared when the task reaches a terminal state
    /// — holding them afterwards is pure memory cost with nothing left to
    /// decide (R11: the protocol permits an 8 MiB payload).
    std::vector<Submission> submissions;

    /// Workers that have already computed this task or a sibling replica.
    /// Invariant 6 forbids granting a replica to any of them: a worker agreeing
    /// with itself is not evidence.
    std::vector<WorkerId> prior_workers;
};

/// Everything a render job needs that a keyspace job does not (5.15/5.16).
///
/// Lives on the Job because it IS job configuration — the camera and the tile
/// grid are chosen once when the render is submitted and every task of it reads
/// the same values. Putting it here also keeps `BuildParams` a pure function of
/// (spec, job, task), which is what makes a task's params reproducible from the
/// job record alone after a restart.
struct RenderConfig {
    TileGrid grid;
    /// Samples per tile for the whole render. Equals `Job::units_per_group`,
    /// and the duplication is deliberate: the carve rule must not have to know
    /// what a "tile" is (R1), while the params builder must.
    std::uint64_t samples_per_tile = 0;

    float cam_origin[3]{};
    float cam_lower_left[3]{};
    float cam_horizontal[3]{};
    float cam_vertical[3]{};
    std::uint32_t max_bounces = 8;
    std::uint32_t rr_start_bounce = 3;

    /// Section sizes of the BVH asset, so the kernel can bounds-check its own
    /// traversal. Belt to `LoadBvh`'s braces — WGSL clamps an out-of-range
    /// index rather than faulting, so a validator regression would otherwise
    /// render a silently wrong image (D-0069).
    std::uint32_t node_count = 0;
    std::uint32_t prim_count = 0;
    std::uint32_t material_count = 0;
};

struct Job {
    JobId id;
    std::string kernel_id;
    std::uint64_t seed = 0;

    /// The keyspace, and how much of it has been handed out. Tasks are CARVED
    /// ON DEMAND from `next_unit` rather than pre-split (D-0043), because a
    /// task's size depends on which worker is asking — and a fleet with a 20x
    /// speed spread handed identical tasks either starves the fast machines or
    /// hands the slow ones work they cannot finish inside a lease.
    std::uint64_t total_units = 0;
    std::uint64_t next_unit = 0;

    /// A task may never cross a multiple of this. 0 disables the constraint.
    ///
    /// The path tracer linearizes a 2D job — `unit = tile_index *
    /// units_per_group + sample_offset` — so one task must belong to exactly
    /// ONE tile. A task spanning two tiles would carry a single set of tile
    /// coordinates and render the wrong half of its range into the wrong place,
    /// and the result would still be a well-formed accumulator of the right
    /// size. Nothing downstream could detect it (the D-0040 shape).
    ///
    /// Enforced at the CARVE, not by asking the sizer to cooperate: the sizer
    /// answers "how much work suits this worker" and has no business knowing
    /// about tiles (R1, and the D-0043 rule order).
    std::uint64_t units_per_group = 0;

    /// The bulk input every task of this job reads, if any (2.16).
    ///
    /// Always absent today: nothing creates assets until Phase 6. It is a real
    /// field rather than a placeholder so the affinity path in `Grant` is the
    /// same code before and after assets exist (D-0047).
    std::optional<AssetId> input_ref;

    /// Present only for render jobs (5.15). Absent means a plain keyspace job.
    std::optional<RenderConfig> render;

    /// Tasks carved so far. Grows as the job runs; empty at creation.
    std::vector<TaskId> tasks;

    [[nodiscard]] bool keyspace_exhausted() const noexcept {
        return next_unit >= total_units;
    }
    [[nodiscard]] std::uint64_t remaining_units() const noexcept {
        return keyspace_exhausted() ? 0 : total_units - next_unit;
    }
};

/// Owns jobs, tasks, and the queue. Single-threaded: it lives on the
/// uWebSockets event loop and nothing may block that loop (CONVENTIONS.md §4).
class JobManager {
public:
    /// Create a job over `total_units` of keyspace. NO tasks are created here —
    /// they are carved on demand by `Grant` (D-0043), because a task's size
    /// depends on which worker asks for it.
    [[nodiscard]] JobId CreateJob(std::string kernel_id, std::uint64_t total_units,
                                  std::uint64_t seed,
                                  std::uint64_t units_per_group = 0);

    /// Hand `worker` a task of `units` work, or nullopt if there is nothing to
    /// give.
    ///
    /// Order is deliberate: **requeued work first, then fresh keyspace.** Work
    /// that has already failed once is the work most at risk of being forgotten
    /// at the tail of a job, and 2.17's speculation exists because the last few
    /// tasks dominate completion time.
    ///
    /// A requeued task keeps its ORIGINAL size — it was carved for whoever had
    /// it before. Re-sizing it would leave a hole in the keyspace or overlap an
    /// existing task, and either is a wrong answer nothing would catch.
    ///
    /// Enforces invariant 6: never granted to a worker that already computed it
    /// or a sibling.
    /// `cached` is the requesting worker's held bulk inputs (2.16). Defaulted
    /// empty because nothing produces assets yet — see D-0047 for why the
    /// preference is implemented rather than stubbed.
    [[nodiscard]] std::optional<Task> Grant(WorkerId worker, std::uint64_t now_ms,
                                            std::uint32_t lease_ms,
                                            std::uint64_t units,
                                            std::span<const AssetId> cached = {});

    /// Invariant 5 on its own, WITHOUT any state change.
    ///
    /// Split out from Submit so a caller can authorize first and only then
    /// decide whether the submission is admissible at all. Ingestion needs that
    /// order: authorization must gate expensive work (a hostile worker must not
    /// be able to make us hash 8 MiB), but a payload that fails its checksum
    /// must leave the task exactly as it was.
    [[nodiscard]] protocol::Status CheckLease(WorkerId worker, TaskId task) const;

    /// Move a task to Validating. Re-checks invariant 5 — callers that already
    /// called CheckLease pay a pointer comparison twice, which is the right
    /// price for this never being bypassable.
    [[nodiscard]] protocol::Status Submit(WorkerId worker, TaskId task);

    /// Extend a lease — step 2.6, driven by `Progress{request_renew}`.
    ///
    /// The DEADLINE moves; the state does not (D-0011). Modelling renewal as a
    /// state change would make it indistinguishable from a fresh grant, and the
    /// two mean opposite things to the sweep below.
    ///
    /// Enforces invariant 5: only the holder may renew. Without that check any
    /// connected peer could keep another worker's lease alive indefinitely,
    /// which is a denial-of-service on the queue rather than a protocol nicety.
    // ── PARTIAL RESULTS (5.13, D-0074) ───────────────────────────────────

    /// The most recent partial upload for a task: a FULL snapshot of the
    /// accumulator so far, never a delta.
    ///
    /// Held here rather than on `Task` because `DirtyTasks()` copies tasks by
    /// value for the durability flush, and a 64 KiB payload per tile would make
    /// every sweep copy the whole working set.
    struct PartialResult {
        WorkerId worker;
        std::uint32_t sequence = 0;
        std::uint64_t units_done = 0;
        std::vector<std::byte> payload;
        std::uint64_t received_at_ms = 0;
    };

    /// Record a partial. Renews the lease and does NOT advance the state
    /// machine — the task stays `Leased` (D-0074).
    ///
    /// Returns an error if the lease is not held, or if `sequence` is not
    /// STRICTLY greater than the one already stored. Out-of-order delivery is
    /// normal after a reconnect, and letting a stale snapshot replace a newer
    /// one rolls a tile backwards — an image that un-converges.
    [[nodiscard]] protocol::Status RecordPartial(WorkerId worker, TaskId task,
                                                 std::uint32_t sequence,
                                                 std::uint64_t units_done,
                                                 std::span<const std::byte> payload,
                                                 std::uint64_t now_ms,
                                                 std::uint32_t lease_ms);

    /// The latest partial for a task, or nullptr. 5.15 composites from these.
    [[nodiscard]] const PartialResult* LatestPartial(TaskId task) const noexcept;

    [[nodiscard]] std::size_t partial_count() const noexcept { return partials_.size(); }

    [[nodiscard]] protocol::Status RenewLease(WorkerId worker, TaskId task,
                                              std::uint64_t now_ms,
                                              std::uint32_t lease_ms);

    /// Return every task whose lease has expired to the queue — step 2.7.
    ///
    /// ONE sweep over all tasks, called from ONE timer on the event loop, never
    /// a timer per task (CONVENTIONS.md §4). At 10k tasks the difference is a
    /// linear scan every few seconds versus 10k live timers.
    ///
    /// **NO REPUTATION PENALTY** (R8). A worker whose lease expired is absent,
    /// and absence is not malice — the machines we most want to reach are the
    /// ones most likely to sleep, close a tab, or lose wifi mid-task.
    ///
    /// Returns the expired ids so the caller can log and count them; 2.9's
    /// metrics need expiry and voluntary release to be tellable apart.
    /// Returns the expired ids AND who held them. The holder matters: an
    /// expiry is EVIDENCE about that worker's speed — the task took at least a
    /// full lease — and 2.13's correction factor would otherwise only ever
    /// learn from tasks that completed, which is precisely the set that
    /// excludes "we sized this far too large" (D-0044).
    struct Expiry {
        TaskId task;
        WorkerId holder;
        std::uint64_t unit_count = 0;
    };
    [[nodiscard]] std::vector<Expiry> SweepExpiredLeases(std::uint64_t now_ms);

    /// Release every lease held by `worker` — step 2.8/2.9. Disconnect, missed
    /// heartbeat, `Goodbye`, and user-stop all route here.
    ///
    /// Returns how many were released. Also no penalty, for the same reason.
    std::size_t ReleaseAllHeldBy(WorkerId worker);

    /// Terminal outcome after validation.
    [[nodiscard]] protocol::Status Finish(TaskId task, bool accepted);

    /// Keep one worker's answer until the group resolves (3.4/D-0054).
    ///
    /// `payload` is empty for `Exact` kernels — the checksum IS the comparison
    /// there, and retaining up to 8 MiB per submission would be a memory lever
    /// an attacker controls (R11).
    void RecordSubmission(TaskId task, WorkerId worker, std::uint64_t checksum,
                          std::vector<std::byte> payload);

    /// Send a task back out for another opinion (3.4).
    ///
    /// `Validating --Disagreement--> NeedsReplica --IssueReplica--> Queued`, so
    /// the state machine witnesses the reason as well as the outcome. Invariant
    /// 6 then keeps it away from anyone who already computed it — not
    /// re-implemented here, because `Grant` already enforces it.
    [[nodiscard]] protocol::Status RequestReplica(TaskId task);

    /// Abandon the group's answers and re-run the range (3.5, no-majority case).
    ///
    /// The submissions are DROPPED: keeping them lets the same deadlocked split
    /// re-form with one more vote and stall the task forever.
    [[nodiscard]] protocol::Status RestartValidation(TaskId task);

    /// Issue a task over a SPECIFIC range whose answer is already known (3.9).
    ///
    /// Deliberately identical to a normal grant in every observable way — same
    /// message, same shape, nothing on the wire says "you are being tested". If
    /// a worker could tell, it would compute these honestly and cheat on
    /// everything else, producing confident evidence of honesty from exactly
    /// the workers this exists to catch (D-0055).
    ///
    /// It does NOT advance the job cursor: this range has already been
    /// computed and accepted, so counting it again would make the job report
    /// progress it has not made.
    [[nodiscard]] std::optional<Task> IssueKnownRange(WorkerId worker,
                                                      std::uint64_t now_ms,
                                                      std::uint32_t lease_ms,
                                                      std::uint64_t start_unit,
                                                      std::uint64_t unit_count);

    /// Return a task to the queue without penalty (R8 — absence is not malice).
    [[nodiscard]] protocol::Status Requeue(TaskId task, TaskEvent why);

    [[nodiscard]] const Task* Find(TaskId id) const noexcept;
    [[nodiscard]] const Job* FindJob(JobId id) const noexcept;

    /// Mutable access to a job's own metadata (2.16 sets `input_ref`).
    ///
    /// Deliberately NOT a way to edit task state: everything about a task's
    /// lifecycle goes through Grant/Submit/Requeue so the state machine stays
    /// the only thing that moves a task (D-0011).
    [[nodiscard]] Job* MutableJob(JobId id) noexcept;
    [[nodiscard]] std::vector<TaskId> HeldBy(WorkerId worker) const;
    /// Tasks waiting to be re-granted. NOT the same as "work left" — fresh
    /// keyspace is not in the queue until it has been carved and come back.
    [[nodiscard]] std::size_t queued() const noexcept { return queue_.size(); }
    [[nodiscard]] std::uint64_t remaining_units() const noexcept;

    /// The job a grant would come from next, or nullptr if there is no work.
    ///
    /// Exists because sizing needs the KERNEL before the task: a task's size
    /// depends on the kernel's R5 floor (D-0029), and the kernel depends on
    /// which job is granted from. Peeking resolves the ordering without making
    /// Grant take a sizing callback.
    [[nodiscard]] const Job* PeekNextJob() const noexcept;
    [[nodiscard]] std::size_t total_tasks() const noexcept { return tasks_.size(); }

    /// Task counts indexed by `TaskState` (2.21). Derived on demand rather than
    /// kept as counters: a tally maintained beside the state it describes is a
    /// tally that eventually disagrees with it.
    [[nodiscard]] std::vector<std::size_t> CountByState() const;

    /// Sum of `total_units` over every job. `remaining_units()` is the
    /// uncarved counterpart.
    [[nodiscard]] std::uint64_t total_units() const noexcept;

    /// Completion detection. Deliberately counts TERMINAL states rather than
    /// "queue is empty" — an empty queue with tasks still leased is not a
    /// finished job, and conflating them is how a job reports success while
    /// work is outstanding (step 2.18 makes this subtler with speculation).
    [[nodiscard]] bool JobComplete(JobId job) const;

    /// Every task in every job is terminal. Same definition as JobComplete,
    /// applied across all jobs — used only by the dev-only
    /// --exit-when-complete harness (step 1.26).
    [[nodiscard]] bool AllComplete() const;

    // ── Speculation (2.17) ───────────────────────────────────────────────

    /// Issue a speculative replica of an outstanding task to `worker`, if the
    /// job is at least `threshold` complete by units.
    ///
    /// Only fires near the end, because that is the only time it pays: a
    /// straggler holding one of the last tasks can double a job's wall time,
    /// while the same duplication early on is pure waste.
    ///
    /// Never to a worker that already holds or has computed the task
    /// (invariant 6) — a worker racing itself is not a race.
    [[nodiscard]] std::optional<Task> IssueSpeculative(WorkerId worker,
                                                       std::uint64_t now_ms,
                                                       std::uint32_t lease_ms,
                                                       double threshold = 0.95);

    /// Cancel every live sibling of `task` — the group is done.
    ///
    /// Returns (task, holder) pairs so the caller can send `Revoke`. Cancelled,
    /// NOT rejected: losing a race carries no penalty (D-0045).
    struct Revocation {
        TaskId task;
        WorkerId holder;
        std::uint64_t wasted_units = 0;
    };
    [[nodiscard]] std::vector<Revocation> CancelSiblingsOf(TaskId task);

    /// Units duplicated by speculation and then discarded. E5 reports this as
    /// the COST side — a speculation policy whose cost is not measured is one
    /// nobody can argue with.
    [[nodiscard]] std::uint64_t wasted_units() const noexcept { return wasted_units_; }

    /// Fraction of the job's keyspace that has reached a terminal accepted
    /// state. Drives the speculation threshold.
    [[nodiscard]] double CompletionFraction(JobId job) const;

    // ── 2.19 — durability (D-0048) ───────────────────────────────────────
    //
    // JobManager contains NO SQL and does not link SQLite. It reports which
    // rows changed; `Store` decides how to write them. That is the same
    // separation that made `Session` testable without a socket (1.15), and it
    // is why every existing test still runs without a database.

    /// Jobs and tasks changed since the last `ClearDirty`.
    ///
    /// Returned by value as full copies rather than pointers: the sweep hands
    /// these to `Store::Flush`, and a pointer into `tasks_` would dangle the
    /// moment anything rehashes the map mid-flush.
    [[nodiscard]] std::vector<Job> DirtyJobs() const;
    [[nodiscard]] std::vector<Task> DirtyTasks() const;

    /// Called only AFTER a successful flush. If the write failed the rows stay
    /// dirty and go out on the next sweep — dropping them would silently make
    /// the file diverge from memory, which is the one failure this layer must
    /// not have.
    void ClearDirty();

    [[nodiscard]] bool has_dirty() const noexcept {
        return !dirty_jobs_.empty() || !dirty_tasks_.empty();
    }

    /// Replace all state with what was recovered from disk (2.20).
    ///
    /// Applies the recovery POLICY: a task that was `Leased` or `Validating`
    /// when we died is requeued, because its worker was talking to a process
    /// that no longer exists (R8). Terminal tasks stay terminal.
    ///
    /// Everything adopted is marked clean, not dirty: it came FROM the file, so
    /// writing it straight back would be a full rewrite on the first sweep
    /// after every restart.
    void AdoptRecovered(const std::vector<Job>& jobs, const std::vector<Task>& tasks,
                        std::uint64_t next_id);

private:
    [[nodiscard]] protocol::Status Apply(Task& task, TaskEvent ev);

    /// Latest partial per task (5.13). Erased when the task goes terminal, so
    /// a long-running job does not accumulate the payload of every tile it has
    /// already finished.
    std::unordered_map<TaskId, PartialResult> partials_;

    /// Mark a row for the next flush. Called from every mutating path; the
    /// alternative — remembering to call it at each site — is the kind of
    /// bookkeeping that is correct until someone adds a path.
    void MarkTaskDirty(TaskId id);
    void MarkJobDirty(JobId id);

    std::unordered_set<TaskId> dirty_tasks_;
    std::unordered_set<JobId> dirty_jobs_;

    std::unordered_map<TaskId, Task> tasks_;
    std::unordered_map<JobId, Job> jobs_;
    std::vector<TaskId> queue_;
    std::uint64_t next_id_ = 1;
    std::uint64_t wasted_units_ = 0;
};

}  // namespace p2pgpu::coordinator

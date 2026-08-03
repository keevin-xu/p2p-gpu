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
#include <vector>

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

    /// Workers that have already computed this task or a sibling replica.
    /// Invariant 6 forbids granting a replica to any of them: a worker agreeing
    /// with itself is not evidence.
    std::vector<WorkerId> prior_workers;
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
                                  std::uint64_t seed);

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
    [[nodiscard]] std::optional<Task> Grant(WorkerId worker, std::uint64_t now_ms,
                                            std::uint32_t lease_ms,
                                            std::uint64_t units);

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
    [[nodiscard]] std::vector<TaskId> SweepExpiredLeases(std::uint64_t now_ms);

    /// Release every lease held by `worker` — step 2.8/2.9. Disconnect, missed
    /// heartbeat, `Goodbye`, and user-stop all route here.
    ///
    /// Returns how many were released. Also no penalty, for the same reason.
    std::size_t ReleaseAllHeldBy(WorkerId worker);

    /// Terminal outcome after validation.
    [[nodiscard]] protocol::Status Finish(TaskId task, bool accepted);

    /// Return a task to the queue without penalty (R8 — absence is not malice).
    [[nodiscard]] protocol::Status Requeue(TaskId task, TaskEvent why);

    [[nodiscard]] const Task* Find(TaskId id) const noexcept;
    [[nodiscard]] const Job* FindJob(JobId id) const noexcept;
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

    /// Completion detection. Deliberately counts TERMINAL states rather than
    /// "queue is empty" — an empty queue with tasks still leased is not a
    /// finished job, and conflating them is how a job reports success while
    /// work is outstanding (step 2.18 makes this subtler with speculation).
    [[nodiscard]] bool JobComplete(JobId job) const;

    /// Every task in every job is terminal. Same definition as JobComplete,
    /// applied across all jobs — used only by the dev-only
    /// --exit-when-complete harness (step 1.26).
    [[nodiscard]] bool AllComplete() const;

private:
    [[nodiscard]] protocol::Status Apply(Task& task, TaskEvent ev);

    std::unordered_map<TaskId, Task> tasks_;
    std::unordered_map<JobId, Job> jobs_;
    std::vector<TaskId> queue_;
    std::uint64_t next_id_ = 1;
};

}  // namespace p2pgpu::coordinator

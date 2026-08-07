// Job decomposition and the task queue — step 1.14. See job.hpp.

#include "p2pgpu/coordinator/job.hpp"

#include <limits>

#include <algorithm>

#include "p2pgpu/protocol/invariants.hpp"

namespace p2pgpu::coordinator {
namespace {
using protocol::ErrorCode;
using protocol::MakeError;
}  // namespace

JobId JobManager::CreateJob(std::string kernel_id, std::uint64_t total_units,
                            std::uint64_t seed, std::uint64_t units_per_group) {
    const JobId job_id{0, next_id_++};
    Job job;
    job.id = job_id;
    job.kernel_id = std::move(kernel_id);
    job.seed = seed;
    job.total_units = total_units;
    job.next_unit = 0;
    job.units_per_group = units_per_group;
    // NO tasks yet. They are carved by Grant, sized for whoever asks (D-0043).
    jobs_.emplace(job_id, std::move(job));
    MarkJobDirty(job_id);
    return job_id;
}

std::vector<std::size_t> JobManager::CountByState() const {
    // Sized from the enum's last member, so adding a state widens this
    // automatically rather than silently dropping it off the dashboard.
    std::vector<std::size_t> out(static_cast<std::size_t>(TaskState::Cancelled) + 1, 0);
    for (const auto& [id, task] : tasks_) {
        const auto idx = static_cast<std::size_t>(task.state);
        if (idx < out.size()) {
            ++out[idx];
        }
    }
    return out;
}

std::uint64_t JobManager::total_units() const noexcept {
    std::uint64_t total = 0;
    for (const auto& [id, job] : jobs_) {
        total += job.total_units;
    }
    return total;
}

std::uint64_t JobManager::remaining_units() const noexcept {
    std::uint64_t total = 0;
    for (const auto& [id, job] : jobs_) {
        total += job.remaining_units();
    }
    return total;
}

protocol::Status JobManager::Apply(Task& task, TaskEvent ev) {
    const auto next = Advance(task.state, ev);
    if (!next) {
        return next.error();
    }
    task.state = *next;
    // THE choke point for durability (2.19). Every state transition in the
    // system goes through here, so marking dirty here covers Grant, Submit,
    // Requeue, Finish, Renew and Cancel by construction rather than by
    // remembering to call it at six sites.
    MarkTaskDirty(task.id);
    return {};
}

void JobManager::MarkTaskDirty(TaskId id) { dirty_tasks_.insert(id); }
void JobManager::MarkJobDirty(JobId id) { dirty_jobs_.insert(id); }

std::vector<Job> JobManager::DirtyJobs() const {
    std::vector<Job> out;
    out.reserve(dirty_jobs_.size());
    for (const JobId id : dirty_jobs_) {
        if (const auto it = jobs_.find(id); it != jobs_.end()) {
            out.push_back(it->second);
        }
    }
    return out;
}

std::vector<Task> JobManager::DirtyTasks() const {
    std::vector<Task> out;
    out.reserve(dirty_tasks_.size());
    for (const TaskId id : dirty_tasks_) {
        if (const auto it = tasks_.find(id); it != tasks_.end()) {
            out.push_back(it->second);
        }
    }
    return out;
}

void JobManager::ClearDirty() {
    dirty_jobs_.clear();
    dirty_tasks_.clear();
}

void JobManager::AdoptRecovered(const std::vector<Job>& jobs,
                                const std::vector<Task>& tasks,
                                std::uint64_t next_id) {
    jobs_.clear();
    tasks_.clear();
    queue_.clear();
    ClearDirty();
    next_id_ = next_id;

    for (const Job& job : jobs) {
        Job copy = job;
        copy.tasks.clear();  // rebuilt below, so the two cannot disagree
        jobs_.emplace(job.id, std::move(copy));
    }

    for (const Task& task : tasks) {
        Task copy = task;

        // THE RECOVERY POLICY, in one visible place (2.20). A task that was
        // Leased or Validating when we died belongs to a worker that was
        // talking to a process which no longer exists. R8 says that is the
        // normal case, so it is requeued rather than treated as an error.
        //
        // `prior_workers` survives, so invariant 6 still holds across a
        // restart: the worker that already computed this task does not get
        // handed its replica after we come back up.
        if (copy.state == TaskState::Leased || copy.state == TaskState::Validating) {
            copy.state = TaskState::Queued;
            queue_.push_back(copy.id);
        }
        // Always cleared, whatever the state. A holder that outlived the
        // process is a lie about who is working, and a lease deadline measured
        // against a clock we no longer share is worse than none.
        copy.holder = WorkerId{};
        copy.lease_expires_at_ms = 0;

        if (const auto it = jobs_.find(copy.job); it != jobs_.end()) {
            it->second.tasks.push_back(copy.id);
        }
        tasks_.emplace(copy.id, std::move(copy));
    }
}

std::optional<Task> JobManager::Grant(WorkerId worker, std::uint64_t now_ms,
                                      std::uint32_t lease_ms, std::uint64_t units,
                                      std::span<const AssetId> cached) {
    // 2.16 — CACHE AFFINITY. Which queued task needs a bulk input this worker
    // already holds? Today no job has an `input_ref` and no worker has a cache,
    // so `needs` is all-nullopt, `PreferCached` returns nullopt, and the loop
    // below runs in plain queue order. Inert by DATA, not by code (D-0047).
    std::vector<std::optional<AssetId>> needs;
    needs.reserve(queue_.size());
    for (const TaskId id : queue_) {
        const auto found = tasks_.find(id);
        const Job* job = found != tasks_.end() ? FindJob(found->second.job) : nullptr;
        needs.push_back(job != nullptr ? job->input_ref : std::nullopt);
    }
    const std::optional<std::size_t> preferred = PreferCached(needs, cached);

    // REQUEUED WORK FIRST. Work that has already failed once is the work most
    // at risk of being forgotten at the tail of a job. A requeued task keeps
    // its original size — re-sizing it would leave a hole in the keyspace or
    // overlap a live task, and neither would be caught by anything.
    //
    // Affinity picks WITHIN this order, never replaces it: the preferred entry
    // is tried first, then everything else in the order it was queued. A task
    // that has already been requeued once does not become less urgent because
    // some other worker happens to hold its input.
    std::vector<std::size_t> order;
    order.reserve(queue_.size());
    if (preferred) {
        order.push_back(*preferred);
    }
    for (std::size_t i = 0; i < queue_.size(); ++i) {
        if (!preferred || i != *preferred) {
            order.push_back(i);
        }
    }

    for (const std::size_t idx : order) {
        // Re-resolved every iteration: `queue_.erase` below invalidates
        // iterators, and the only exit after an erase is `return`.
        auto it = queue_.begin() + static_cast<std::ptrdiff_t>(idx);
        auto found = tasks_.find(*it);
        if (found == tasks_.end()) {
            continue;
        }
        Task& task = found->second;

        // INVARIANT 6, checked before granting rather than after. A worker that
        // already computed this task (or a sibling replica) agreeing with
        // itself is not evidence, and a liar would validate its own lie.
        if (!protocol::CheckReplicaAssignment(task.prior_workers, worker)) {
            continue;  // try the next queued task
        }

        if (!Apply(task, TaskEvent::Grant)) {
            continue;  // not actually grantable; leave it alone
        }
        task.holder = worker;
        task.lease_expires_at_ms = now_ms + lease_ms;
        queue_.erase(it);
        return task;
    }

    // Nothing requeued. Carve fresh keyspace, sized for this worker.
    if (units == 0) {
        return std::nullopt;
    }
    for (auto& [job_id, job] : jobs_) {
        if (job.keyspace_exhausted()) {
            continue;
        }
        Task t;
        t.id = TaskId{0, next_id_++};
        t.job = job_id;

        // ── SAMPLE-BUDGET SCHEDULING (5.17, D-0078) ──────────────────────
        //
        // A render job picks the LEAST-GRANTED tile rather than advancing a
        // cursor, so the image converges evenly instead of in index order.
        // `next_unit` stops being a position and becomes the SUM of what has
        // been granted, which is the only property anything downstream ever
        // used it for — `keyspace_exhausted`, `remaining_units`, `JobComplete`
        // and the durability flush all keep working unchanged.
        std::uint64_t tile_index = 0;
        const bool by_tile =
            job.render.has_value() && job.render->samples_per_tile > 0 &&
            !job.tile_granted.empty();
        if (by_tile) {
            std::uint64_t fewest = std::numeric_limits<std::uint64_t>::max();
            bool found = false;
            for (std::size_t i = 0; i < job.tile_granted.size(); ++i) {
                if (job.tile_granted[i] >= job.render->samples_per_tile) {
                    continue;   // this tile's budget is fully carved
                }
                // Strictly less, so ties keep the LOWEST index. The choice has
                // to be deterministic or an experiment cannot be replayed
                // (2.3's rule), and "converges evenly" is a claim someone will
                // want to reproduce.
                if (job.tile_granted[i] < fewest) {
                    fewest = job.tile_granted[i];
                    tile_index = i;
                    found = true;
                }
            }
            if (!found) {
                continue;   // every tile fully carved; try the next job
            }
            t.start_unit = tile_index * job.render->samples_per_tile +
                           job.tile_granted[tile_index];
        } else {
            t.start_unit = job.next_unit;
        }
        // Never past the end. The final task of a job takes whatever remains,
        // which may be less than the R5 floor — correct, because the
        // alternative is leaving a remainder unsearched (D-0043).
        t.unit_count = std::min(units, job.remaining_units());
        // Never across a group boundary (a TILE, for the path tracer). Applied
        // AFTER the size and the remaining-units cap, so it can only ever
        // shrink a task — it is a constraint on where a task may end, not a
        // sizing input, and D-0043's rule order stays intact.
        if (job.units_per_group != 0) {
            const std::uint64_t offset_in_group = t.start_unit % job.units_per_group;
            const std::uint64_t to_boundary = job.units_per_group - offset_in_group;
            t.unit_count = std::min(t.unit_count, to_boundary);
        }
        if (t.unit_count == 0) {
            continue;   // nothing left in the chosen tile; try the next job
        }
        t.state = TaskState::Leased;
        t.holder = worker;
        t.lease_expires_at_ms = now_ms + lease_ms;

        if (by_tile) {
            job.tile_granted[tile_index] += t.unit_count;
        }
        // The SUM either way. For a render job this is no longer a position,
        // and nothing downstream treats it as one.
        job.next_unit += t.unit_count;
        job.tasks.push_back(t.id);
        // The CURSOR moved, which is the only job field that changes after
        // creation — and the field that decides what a restart re-carves.
        MarkJobDirty(job_id);
        MarkTaskDirty(t.id);
        const Task copy = t;
        tasks_.emplace(t.id, std::move(t));
        return copy;
    }
    return std::nullopt;
}

protocol::Status JobManager::CheckLease(WorkerId worker, TaskId task_id) const {
    const auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        // Deliberately the same error as "you don't hold it": a worker probing
        // for which task IDs exist learns nothing from the reply.
        return MakeError(ErrorCode::LeaseNotHeld, "unknown task");
    }
    const Task& task = it->second;

    // INVARIANT 5 — the authorization check. Without it any worker can submit
    // results for another worker's task, defeating replication, reputation, and
    // speculation at once.
    if (task.state != TaskState::Leased || task.holder != worker) {
        return MakeError(ErrorCode::LeaseNotHeld,
                         "worker does not hold the lease on this task");
    }
    // Strongly typed since 1.6: `held` is a span of TaskId, so a WorkerId list
    // cannot be passed here by mistake.
    const TaskId held[] = {task.id};
    return protocol::CheckLeaseHeld(held, task_id);
}

protocol::Status JobManager::Submit(WorkerId worker, TaskId task_id) {
    if (const auto s = CheckLease(worker, task_id); !s) {
        return s;
    }
    Task& task = tasks_.find(task_id)->second;  // CheckLease proved it exists

    if (const auto s = Apply(task, TaskEvent::Submit); !s) {
        return s;
    }
    // Record the worker BEFORE validation resolves: if this result is later
    // disputed, the replica must go to somebody else (invariant 6), and that
    // holds regardless of whether the submission is eventually accepted.
    task.prior_workers.push_back(worker);
    task.holder = WorkerId{};
    return {};
}

protocol::Status JobManager::RenewLease(WorkerId worker, TaskId task_id,
                                        std::uint64_t now_ms, std::uint32_t lease_ms) {
    // Invariant 5 first. A peer that does not hold the lease must not be able
    // to extend it — otherwise anyone can pin a task out of the queue forever.
    if (const auto s = CheckLease(worker, task_id); !s) {
        return s;
    }
    Task& task = tasks_.find(task_id)->second;

    // Only the DEADLINE moves. Apply(Renew) is still called so the state
    // machine gets to reject a renewal from a state that should not see one —
    // it maps Leased->Leased and everything else to an error (D-0011).
    if (const auto s = Apply(task, TaskEvent::Renew); !s) {
        return s;
    }
    task.lease_expires_at_ms = now_ms + lease_ms;
    return {};
}

// ── Partial results (5.13, D-0074) ───────────────────────────────────────

protocol::Status JobManager::RecordPartial(WorkerId worker, TaskId task_id,
                                           std::uint32_t sequence,
                                           std::uint64_t units_done,
                                           std::span<const std::byte> payload,
                                           std::uint64_t now_ms,
                                           std::uint32_t lease_ms) {
    // Invariant 5 first, exactly as RenewLease does: a peer that does not hold
    // the lease must not be able to write a snapshot into this task, and must
    // not be able to extend the lease by doing so.
    if (const auto s = CheckLease(worker, task_id); !s) {
        return s;
    }

    auto it = partials_.find(task_id);
    if (it != partials_.end() && sequence <= it->second.sequence) {
        // STRICTLY greater, and equality counts as stale. A retransmission
        // after a reconnect carries the same sequence, and letting it through
        // would be harmless today but would silently permit a genuine reorder
        // tomorrow — the tile rolls backwards and the image un-converges.
        return protocol::MakeError(protocol::ErrorCode::Internal,
                                   "partial result is not newer than the one held");
    }

    // A partial is PROOF OF LIFE and renews the lease — 5.13's "lease renewal
    // tied to upload cadence". The 2.6/D-0046 rule from the other side: never
    // require a separate heartbeat from a worker that is visibly delivering.
    if (const auto s = RenewLease(worker, task_id, now_ms, lease_ms); !s) {
        return s;
    }

    PartialResult& slot = partials_[task_id];
    slot.worker = worker;
    slot.sequence = sequence;
    slot.units_done = units_done;
    slot.payload.assign(payload.begin(), payload.end());
    slot.received_at_ms = now_ms;
    // NOTE: the task's STATE is deliberately untouched. Submit would move it to
    // Validating, from which Release is not a legal edge — D-0031's wedge, where
    // a task strands with no lease and no worker and the job never completes.
    return {};
}

const JobManager::PartialResult* JobManager::LatestPartial(TaskId task) const noexcept {
    const auto it = partials_.find(task);
    return it == partials_.end() ? nullptr : &it->second;
}

std::vector<JobManager::Expiry> JobManager::SweepExpiredLeases(std::uint64_t now_ms) {
    std::vector<Expiry> expired;
    for (auto& [id, task] : tasks_) {
        // Validity is the HALF-OPEN interval [grant, expires): valid strictly
        // before the deadline, expired at it. That makes a 1000 ms lease last
        // exactly 1000 ms — and the sizer's "never exceed lease duration" clamp
        // (2.12) has to read it the same way, or a task can be sized to outlive
        // the lease it was granted under.
        if (task.state != TaskState::Leased || now_ms < task.lease_expires_at_ms) {
            continue;
        }
        // LeaseExpired, not Release. Same target state, deliberately distinct
        // events: neither penalises reputation (R8), but 2.7 and 2.9 must tell
        // them apart in metrics — "the worker gave it back" and "the worker
        // vanished" are different facts about the fleet.
        if (const auto s = Apply(task, TaskEvent::LeaseExpired); !s) {
            continue;   // not expirable from this state; leave it alone
        }
        expired.push_back(Expiry{id, task.holder, task.unit_count});
        task.holder = WorkerId{};
        task.lease_expires_at_ms = 0;
        queue_.push_back(id);
    }
    return expired;
}

std::size_t JobManager::ReleaseAllHeldBy(WorkerId worker) {
    std::size_t released = 0;
    for (const TaskId id : HeldBy(worker)) {
        if (Requeue(id, TaskEvent::Release)) {
            ++released;
        }
    }
    return released;
}

protocol::Status JobManager::Finish(TaskId task_id, bool accepted) {
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        return MakeError(ErrorCode::Internal, "unknown task");
    }
    const auto s = Apply(it->second, accepted ? TaskEvent::Accept : TaskEvent::Reject);
    if (s) {
        // Terminal: the answers have nothing left to decide, and the protocol
        // permits an 8 MiB payload each (R11/D-0054). Freed here rather than
        // "eventually" because "eventually" is how a coordinator with a long
        // uptime runs out of memory.
        it->second.submissions.clear();
        it->second.submissions.shrink_to_fit();
        // Same reasoning for the partial snapshot (5.13): a finished tile's
        // accumulator is 64 KiB that nothing will read again. A render of
        // thousands of tiles would otherwise hold every one of them for the
        // life of the process.
        partials_.erase(task_id);
    }
    return s;
}

void JobManager::RecordSubmission(TaskId task_id, WorkerId worker,
                                  std::uint64_t checksum,
                                  std::vector<std::byte> payload) {
    const auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        return;
    }
    it->second.submissions.push_back(
        Task::Submission{worker, checksum, std::move(payload)});
    MarkTaskDirty(task_id);
}

protocol::Status JobManager::RequestReplica(TaskId task_id) {
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        return MakeError(ErrorCode::Internal, "unknown task");
    }
    Task& task = it->second;
    // Both transitions, so the state machine witnesses the reason as well as
    // the outcome. Collapsing them into one edge would lose "why is this task
    // queued again" at exactly the point 3.3 wants to log it.
    if (const auto s = Apply(task, TaskEvent::Disagreement); !s) {
        return s;
    }
    if (const auto s = Apply(task, TaskEvent::IssueReplica); !s) {
        return s;
    }
    task.holder = WorkerId{};
    task.lease_expires_at_ms = 0;
    queue_.push_back(task.id);
    return {};
}

std::optional<Task> JobManager::IssueKnownRange(WorkerId worker, std::uint64_t now_ms,
                                                std::uint32_t lease_ms,
                                                std::uint64_t start_unit,
                                                std::uint64_t unit_count) {
    if (jobs_.empty() || unit_count == 0) {
        return std::nullopt;
    }
    // Attach to whichever job owns this keyspace. With one job this is exact;
    // with several the caller supplies a range that came from a completed task,
    // so the first job containing it is the right one.
    for (auto& [job_id, job] : jobs_) {
        if (start_unit + unit_count > job.total_units) {
            continue;
        }
        Task t;
        t.id = TaskId{0, next_id_++};
        t.job = job_id;
        t.start_unit = start_unit;
        t.unit_count = unit_count;
        t.state = TaskState::Leased;
        t.holder = worker;
        t.lease_expires_at_ms = now_ms + lease_ms;

        // NOT added to `job.tasks`, and the cursor is NOT advanced. This range
        // is already accounted for by the task that originally computed it —
        // counting it twice would make JobComplete wait on a task that carries
        // no new work, and inflate every task count in the metrics.
        const Task copy = t;
        tasks_.emplace(t.id, std::move(t));
        MarkTaskDirty(copy.id);
        return copy;
    }
    return std::nullopt;
}

protocol::Status JobManager::RestartValidation(TaskId task_id) {
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        return MakeError(ErrorCode::Internal, "unknown task");
    }
    Task& task = it->second;
    if (const auto s = Apply(task, TaskEvent::Disagreement); !s) {
        return s;
    }
    if (const auto s = Apply(task, TaskEvent::IssueReplica); !s) {
        return s;
    }
    // DROP the answers. Keeping them lets the same deadlocked split re-form
    // with one more vote and stall the task indefinitely; the point of an
    // inconclusive verdict is that this group produced no evidence.
    //
    // `prior_workers` is deliberately NOT cleared — invariant 6 still bars
    // everyone who has already seen this range, which is what "fresh workers"
    // means.
    task.submissions.clear();
    task.holder = WorkerId{};
    task.lease_expires_at_ms = 0;
    queue_.push_back(task.id);
    return {};
}

protocol::Status JobManager::Requeue(TaskId task_id, TaskEvent why) {
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        return MakeError(ErrorCode::Internal, "unknown task");
    }
    Task& task = it->second;
    if (const auto s = Apply(task, why); !s) {
        return s;
    }
    task.holder = WorkerId{};
    task.lease_expires_at_ms = 0;
    queue_.push_back(task.id);
    return {};
}

const Task* JobManager::Find(TaskId id) const noexcept {
    const auto it = tasks_.find(id);
    return it == tasks_.end() ? nullptr : &it->second;
}

bool JobManager::AllComplete() const {
    if (jobs_.empty()) {
        return false;
    }
    return std::ranges::all_of(jobs_, [this](const auto& kv) {
        return JobComplete(kv.first);
    });
}

const Job* JobManager::PeekNextJob() const noexcept {
    // Requeued work first, mirroring Grant — otherwise the size would be
    // computed against one job's kernel and the task carved from another's.
    for (const TaskId id : queue_) {
        const auto it = tasks_.find(id);
        if (it != tasks_.end()) {
            return FindJob(it->second.job);
        }
    }
    for (const auto& [job_id, job] : jobs_) {
        if (!job.keyspace_exhausted()) {
            return &job;
        }
    }
    return nullptr;
}

Job* JobManager::MutableJob(JobId id) noexcept {
    const auto it = jobs_.find(id);
    return it == jobs_.end() ? nullptr : &it->second;
}

const Job* JobManager::FindJob(JobId id) const noexcept {
    const auto it = jobs_.find(id);
    return it == jobs_.end() ? nullptr : &it->second;
}

std::vector<TaskId> JobManager::HeldBy(WorkerId worker) const {
    std::vector<TaskId> out;
    for (const auto& [id, task] : tasks_) {
        if (task.state == TaskState::Leased && task.holder == worker) {
            out.push_back(id);
        }
    }
    return out;
}

double JobManager::CompletionFraction(JobId job_id) const {
    const auto it = jobs_.find(job_id);
    if (it == jobs_.end() || it->second.total_units == 0) {
        return 0.0;
    }
    std::uint64_t done = 0;
    for (const TaskId id : it->second.tasks) {
        const Task* t = Find(id);
        // ACCEPTED only. A cancelled or rejected task's range still needs
        // doing, and counting it would make a job look nearly finished while
        // keyspace remains unsearched.
        if (t != nullptr && t->state == TaskState::Accepted && t->replica_of == TaskId{}) {
            done += t->unit_count;
        }
    }
    return static_cast<double>(done) / static_cast<double>(it->second.total_units);
}

std::optional<Task> JobManager::IssueSpeculative(WorkerId worker, std::uint64_t now_ms,
                                                 std::uint32_t lease_ms,
                                                 double threshold) {
    for (auto& [job_id, job] : jobs_) {
        // Only near the end. Early duplication is pure waste; it pays only when
        // a straggler holding one of the last tasks would otherwise dominate
        // the job's wall time.
        if (!job.keyspace_exhausted() || CompletionFraction(job_id) < threshold) {
            continue;
        }
        for (const TaskId id : job.tasks) {
            auto found = tasks_.find(id);
            if (found == tasks_.end()) {
                continue;
            }
            Task& original = found->second;
            if (original.state != TaskState::Leased) {
                continue;
            }
            // Only ORIGINALS may be raced. A replica is itself a Leased task,
            // so without this it becomes a candidate too — and replicas of
            // replicas multiply: three workers on one range, then four, each
            // costing a full duplicate of the work.
            if (original.replica_of != TaskId{}) {
                continue;
            }
            // Never race a worker against itself — invariant 6. A worker
            // agreeing with itself is not evidence, and here it would not even
            // be faster.
            if (original.holder == worker ||
                !protocol::CheckReplicaAssignment(original.prior_workers, worker)) {
                continue;
            }
            // Already has a live replica; one race is enough.
            const bool has_live_replica = std::ranges::any_of(
                job.tasks, [&](TaskId t) {
                    const Task* c = Find(t);
                    return c != nullptr && c->replica_of == id && !IsTerminal(c->state);
                });
            if (has_live_replica) {
                continue;
            }

            Task replica;
            replica.id = TaskId{0, next_id_++};
            replica.job = job_id;
            replica.replica_of = id;
            replica.start_unit = original.start_unit;
            replica.unit_count = original.unit_count;
            replica.state = TaskState::Leased;
            replica.holder = worker;
            replica.lease_expires_at_ms = now_ms + lease_ms;

            job.tasks.push_back(replica.id);
            const Task copy = replica;
            const TaskId replica_id = replica.id;
            tasks_.emplace(replica.id, std::move(replica));
            // Constructed directly rather than via Apply (it is born Leased),
            // so it needs an explicit mark — the one task-creation path Apply
            // does not cover.
            MarkTaskDirty(replica_id);
            return copy;
        }
    }
    return std::nullopt;
}

std::vector<JobManager::Revocation> JobManager::CancelSiblingsOf(TaskId task_id) {
    std::vector<Revocation> out;
    const Task* winner = Find(task_id);
    if (winner == nullptr) {
        return out;
    }
    // The whole family: the original and every replica of it. Which one won
    // does not matter — the range is done.
    const TaskId root = winner->replica_of == TaskId{} ? task_id : winner->replica_of;

    const Job* job = FindJob(winner->job);
    if (job == nullptr) {
        return out;
    }
    for (const TaskId id : job->tasks) {
        if (id == task_id) {
            continue;
        }
        auto it = tasks_.find(id);
        if (it == tasks_.end()) {
            continue;
        }
        Task& sibling = it->second;
        const bool same_family = (id == root) || (sibling.replica_of == root);
        if (!same_family || IsTerminal(sibling.state)) {
            continue;
        }
        if (!Apply(sibling, TaskEvent::Cancel)) {
            continue;
        }
        wasted_units_ += sibling.unit_count;
        out.push_back(Revocation{id, sibling.holder, sibling.unit_count});
        sibling.holder = WorkerId{};
        sibling.lease_expires_at_ms = 0;
        // NOT requeued. The range is done — putting it back would hand
        // already-finished work to somebody else.
        std::erase(queue_, id);
    }
    return out;
}

bool JobManager::JobComplete(JobId job_id) const {
    const auto it = jobs_.find(job_id);
    if (it == jobs_.end()) {
        return false;
    }
    // Counts TERMINAL states, not an empty queue. A queue can be empty while
    // tasks are still leased — reporting that as complete would claim success
    // with work outstanding.
    // TWO conditions, not one (D-0043). Either alone is wrong: an empty queue
    // with the cursor mid-keyspace is a job that has barely started, and an
    // exhausted cursor with tasks in flight is one that is nearly done.
    if (!it->second.keyspace_exhausted()) {
        return false;
    }
    return std::ranges::all_of(it->second.tasks, [this](TaskId t) {
        const Task* task = Find(t);
        return task != nullptr && IsTerminal(task->state);
    });
}

}  // namespace p2pgpu::coordinator

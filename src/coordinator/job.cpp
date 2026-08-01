// Job decomposition and the task queue — step 1.14. See job.hpp.

#include "p2pgpu/coordinator/job.hpp"

#include <algorithm>

#include "p2pgpu/protocol/invariants.hpp"

namespace p2pgpu::coordinator {
namespace {
using protocol::ErrorCode;
using protocol::MakeError;
}  // namespace

JobId JobManager::CreateJob(std::string kernel_id, std::uint64_t total_units,
                            std::uint32_t task_count, std::uint64_t seed) {
    const JobId job_id{0, next_id_++};
    Job job;
    job.id = job_id;
    job.kernel_id = std::move(kernel_id);
    job.seed = seed;

    const std::uint32_t n = std::max(task_count, 1U);
    const std::uint64_t per_task = std::max<std::uint64_t>(total_units / n, 1);

    for (std::uint32_t i = 0; i < n; ++i) {
        Task t;
        t.id = TaskId{0, next_id_++};
        t.job = job_id;
        t.start_unit = static_cast<std::uint64_t>(i) * per_task;
        // The last task absorbs the remainder, so the union of all tasks is
        // exactly the keyspace. Rounding each one independently would leave a
        // gap, and the missing candidates would never be searched by anyone.
        t.unit_count = (i + 1 == n) ? (total_units - t.start_unit) : per_task;

        job.tasks.push_back(t.id);
        queue_.push_back(t.id);
        tasks_.emplace(t.id, std::move(t));
    }

    jobs_.emplace(job_id, std::move(job));
    return job_id;
}

protocol::Status JobManager::Apply(Task& task, TaskEvent ev) {
    const auto next = Advance(task.state, ev);
    if (!next) {
        return next.error();
    }
    task.state = *next;
    return {};
}

std::optional<Task> JobManager::Grant(WorkerId worker, std::uint64_t now_ms,
                                      std::uint32_t lease_ms) {
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
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

protocol::Status JobManager::Finish(TaskId task_id, bool accepted) {
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        return MakeError(ErrorCode::Internal, "unknown task");
    }
    return Apply(it->second, accepted ? TaskEvent::Accept : TaskEvent::Reject);
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

std::vector<TaskId> JobManager::HeldBy(WorkerId worker) const {
    std::vector<TaskId> out;
    for (const auto& [id, task] : tasks_) {
        if (task.state == TaskState::Leased && task.holder == worker) {
            out.push_back(id);
        }
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
    return std::ranges::all_of(it->second.tasks, [this](TaskId t) {
        const Task* task = Find(t);
        return task != nullptr && IsTerminal(task->state);
    });
}

}  // namespace p2pgpu::coordinator

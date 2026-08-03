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
    std::vector<TaskId> tasks;
};

/// Owns jobs, tasks, and the queue. Single-threaded: it lives on the
/// uWebSockets event loop and nothing may block that loop (CONVENTIONS.md §4).
class JobManager {
public:
    /// Split a keyspace into `task_count` equal tasks.
    ///
    /// Fixed sizing on purpose at this step. Adaptive sizing needs a benchmark
    /// score per worker (2.11) which does not exist yet, and inventing one here
    /// would be a number with no measurement behind it.
    [[nodiscard]] JobId CreateJob(std::string kernel_id, std::uint64_t total_units,
                                  std::uint32_t task_count, std::uint64_t seed);

    /// Hand the next queued task to `worker`, or nullopt if the queue is empty.
    ///
    /// Enforces invariant 6: a task is never granted to a worker that already
    /// computed it or a sibling.
    [[nodiscard]] std::optional<Task> Grant(WorkerId worker, std::uint64_t now_ms,
                                            std::uint32_t lease_ms);

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

    /// Terminal outcome after validation.
    [[nodiscard]] protocol::Status Finish(TaskId task, bool accepted);

    /// Return a task to the queue without penalty (R8 — absence is not malice).
    [[nodiscard]] protocol::Status Requeue(TaskId task, TaskEvent why);

    [[nodiscard]] const Task* Find(TaskId id) const noexcept;
    [[nodiscard]] const Job* FindJob(JobId id) const noexcept;
    [[nodiscard]] std::vector<TaskId> HeldBy(WorkerId worker) const;
    [[nodiscard]] std::size_t queued() const noexcept { return queue_.size(); }
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

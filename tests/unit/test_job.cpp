// Step 1.14 — job decomposition, the task queue, and lease bookkeeping.
//
// The interesting assertions here are the ones about what does NOT happen:
// a task granted twice, a submit from a worker that does not hold the lease, a
// job reporting complete while work is outstanding. Those are the failures that
// silently corrupt results rather than crashing, so they get explicit tests.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <numeric>

#include "p2pgpu/coordinator/job.hpp"

using namespace p2pgpu::coordinator;
using p2pgpu::protocol::WorkerId;

namespace {
constexpr std::uint64_t kNow = 1'000'000;
constexpr std::uint32_t kLease = 30'000;
const WorkerId kAlice{1, 0};
const WorkerId kBob{2, 0};
}  // namespace

TEST_CASE("CreateJob partitions the keyspace exactly", "[job]") {
    JobManager jobs;

    // 1000 units into 3 tasks does not divide evenly. The partition must still
    // cover the range exactly once — a lost or duplicated unit is a wrong
    // answer that no amount of replication would catch, since every replica
    // would search the same wrong range.
    const JobId job = jobs.CreateJob("brute_search_v1", 1000, 3, /*seed=*/42);
    REQUIRE(jobs.total_tasks() == 3);
    REQUIRE(jobs.queued() == 3);

    std::vector<Task> tasks;
    for (int i = 0; i < 3; ++i) {
        const auto t = jobs.Grant(WorkerId{static_cast<std::uint64_t>(10 + i), 0}, kNow, kLease);
        REQUIRE(t.has_value());
        REQUIRE(t->job == job);
        tasks.push_back(*t);
    }

    std::ranges::sort(tasks, {}, &Task::start_unit);
    CHECK(tasks[0].start_unit == 0);
    for (std::size_t i = 1; i < tasks.size(); ++i) {
        // Contiguous, no gap and no overlap.
        CHECK(tasks[i].start_unit == tasks[i - 1].start_unit + tasks[i - 1].unit_count);
    }
    const std::uint64_t total = std::accumulate(
        tasks.begin(), tasks.end(), std::uint64_t{0},
        [](std::uint64_t acc, const Task& t) { return acc + t.unit_count; });
    CHECK(total == 1000);
    // The remainder lands on the LAST task rather than being dropped.
    CHECK(tasks.back().unit_count == 334);
}

TEST_CASE("Grant is exclusive and drains the queue", "[job]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 100, 1, 7);

    const auto first = jobs.Grant(kAlice, kNow, kLease);
    REQUIRE(first.has_value());
    CHECK(jobs.queued() == 0);
    CHECK(jobs.Find(first->id)->state == TaskState::Leased);
    CHECK(jobs.Find(first->id)->holder == kAlice);
    CHECK(jobs.Find(first->id)->lease_expires_at_ms == kNow + kLease);

    // A leased task is not available to anyone else. Handing the same task to
    // two workers here would look like successful speculation, which is a Phase
    // 3 feature with a completely different bookkeeping path.
    CHECK_FALSE(jobs.Grant(kBob, kNow, kLease).has_value());
}

TEST_CASE("Submit requires the lease (invariant 5)", "[job]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 100, 1, 7);
    const auto task = jobs.Grant(kAlice, kNow, kLease);
    REQUIRE(task.has_value());

    // Bob never held it. This is the authorization check, and it must fail
    // before any state change — the task is still Alice's afterwards.
    CHECK_FALSE(jobs.Submit(kBob, task->id));
    CHECK(jobs.Find(task->id)->state == TaskState::Leased);
    CHECK(jobs.Find(task->id)->holder == kAlice);

    REQUIRE(jobs.Submit(kAlice, task->id));
    CHECK(jobs.Find(task->id)->state == TaskState::Validating);

    // A second submit for the same task is no longer legal: the task is not
    // Leased any more, so there is no lease to check against.
    CHECK_FALSE(jobs.Submit(kAlice, task->id));
}

TEST_CASE("Requeue after a lease does not bar the worker from retrying", "[job]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 100, 1, 7);
    const auto task = jobs.Grant(kAlice, kNow, kLease);
    REQUIRE(task.has_value());

    REQUIRE(jobs.Requeue(task->id, TaskEvent::Release));
    CHECK(jobs.queued() == 1);
    CHECK(jobs.Find(task->id)->state == TaskState::Queued);
    CHECK(jobs.Find(task->id)->holder == WorkerId{});
    CHECK(jobs.Find(task->id)->prior_workers.empty());

    // Alice can have it back. Invariant 6 bars a worker that already COMPUTED a
    // task, and `prior_workers` is written on Submit for exactly that reason —
    // a lease that ended without a result produced no opinion to agree with.
    // Barring her here would also wedge a single-worker fleet: every dropped
    // lease would permanently shrink the set of workers eligible for that task.
    CHECK(jobs.Grant(kAlice, kNow, kLease).has_value());
}

TEST_CASE("Invariant 6 bars a worker that already submitted", "[job]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 100, 1, 7);
    const auto task = jobs.Grant(kAlice, kNow, kLease);
    REQUIRE(task.has_value());
    REQUIRE(jobs.Submit(kAlice, task->id));

    // Disputed result: back to the queue for a second opinion.
    REQUIRE(jobs.Requeue(task->id, TaskEvent::Disagreement));
    REQUIRE(jobs.Requeue(task->id, TaskEvent::IssueReplica));
    CHECK(jobs.Find(task->id)->state == TaskState::Queued);

    // Now Alice must NOT get it back — a worker agreeing with itself is not
    // evidence, and a liar would validate its own lie.
    CHECK_FALSE(jobs.Grant(kAlice, kNow, kLease).has_value());
    CHECK(jobs.Grant(kBob, kNow, kLease).has_value());
}

TEST_CASE("HeldBy lists exactly the leases a worker holds", "[job]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 100, 4, 7);

    const auto a1 = jobs.Grant(kAlice, kNow, kLease);
    const auto a2 = jobs.Grant(kAlice, kNow, kLease);
    const auto b1 = jobs.Grant(kBob, kNow, kLease);
    REQUIRE(a1);
    REQUIRE(a2);
    REQUIRE(b1);

    auto held = jobs.HeldBy(kAlice);
    std::ranges::sort(held, {}, [](p2pgpu::protocol::TaskId t) { return t.lo(); });
    REQUIRE(held.size() == 2);
    CHECK(std::ranges::find(held, a1->id) != held.end());
    CHECK(std::ranges::find(held, a2->id) != held.end());
    CHECK(std::ranges::find(held, b1->id) == held.end());

    // Submitting moves the task out of Leased, so it is no longer "held" —
    // otherwise a disconnect during validation would requeue work that is
    // already being judged.
    REQUIRE(jobs.Submit(kAlice, a1->id));
    CHECK(jobs.HeldBy(kAlice).size() == 1);
}

TEST_CASE("JobComplete counts terminal states, not an empty queue", "[job]") {
    JobManager jobs;
    const JobId job = jobs.CreateJob("k", 100, 2, 7);

    const auto t1 = jobs.Grant(kAlice, kNow, kLease);
    const auto t2 = jobs.Grant(kBob, kNow, kLease);
    REQUIRE(t1);
    REQUIRE(t2);

    // THE POINT OF THIS TEST: the queue is empty here, and the job is not done.
    // Conflating the two is how a job reports success with work outstanding.
    CHECK(jobs.queued() == 0);
    CHECK_FALSE(jobs.JobComplete(job));

    REQUIRE(jobs.Submit(kAlice, t1->id));
    REQUIRE(jobs.Finish(t1->id, /*accepted=*/true));
    CHECK_FALSE(jobs.JobComplete(job));

    REQUIRE(jobs.Submit(kBob, t2->id));
    // Rejected is terminal too: a job whose tasks all failed is finished, not
    // hung. Distinguishing "complete" from "successful" is Phase 3's problem.
    REQUIRE(jobs.Finish(t2->id, /*accepted=*/false));
    CHECK(jobs.Find(t2->id)->state == TaskState::Rejected);
    CHECK(jobs.JobComplete(job));
}

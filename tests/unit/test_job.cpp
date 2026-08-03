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
/// Tasks are carved on demand now (D-0043), so a grant states its size.
constexpr std::uint64_t kUnits = 100;
constexpr std::uint32_t kLease = 30'000;
const WorkerId kAlice{1, 0};
const WorkerId kBob{2, 0};
}  // namespace

TEST_CASE("carving covers the keyspace exactly once", "[job]") {
    JobManager jobs;

    // No tasks exist at creation any more — they are carved on demand, sized
    // for whoever asks (D-0043). Nothing to enumerate until somebody works.
    const JobId job = jobs.CreateJob("brute_search_v1", 1000, /*seed=*/42);
    CHECK(jobs.total_tasks() == 0);
    CHECK(jobs.queued() == 0);
    CHECK(jobs.remaining_units() == 1000);

    // Deliberately UNEQUAL sizes, which is the whole point of on-demand
    // carving: a fast worker takes more. 400 + 400 leaves 200.
    std::vector<Task> tasks;
    for (int i = 0; i < 3; ++i) {
        const auto t = jobs.Grant(WorkerId{static_cast<std::uint64_t>(10 + i), 0},
                                  kNow, kLease, 400);
        REQUIRE(t.has_value());
        REQUIRE(t->job == job);
        tasks.push_back(*t);
    }

    std::ranges::sort(tasks, {}, &Task::start_unit);
    CHECK(tasks[0].start_unit == 0);
    for (std::size_t i = 1; i < tasks.size(); ++i) {
        // Contiguous: no gap and no overlap. A lost or duplicated unit is a
        // wrong answer no amount of replication would catch, because every
        // replica would search the same wrong range.
        CHECK(tasks[i].start_unit == tasks[i - 1].start_unit + tasks[i - 1].unit_count);
    }
    const std::uint64_t total = std::accumulate(
        tasks.begin(), tasks.end(), std::uint64_t{0},
        [](std::uint64_t acc, const Task& t) { return acc + t.unit_count; });
    CHECK(total == 1000);

    // The LAST task takes what remains — 200, not the 400 it asked for. It may
    // legally be smaller than a kernel's R5 floor; the alternative is leaving a
    // remainder unsearched (D-0043).
    CHECK(tasks.back().unit_count == 200);

    // Keyspace exhausted: no more fresh work, however large the request.
    CHECK(jobs.remaining_units() == 0);
    CHECK_FALSE(jobs.Grant(kAlice, kNow, kLease, 1000).has_value());
}

TEST_CASE("a requeued task keeps its ORIGINAL size", "[job]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 1000, 7);
    const auto first = jobs.Grant(kAlice, kNow, kLease, 250);
    REQUIRE(first.has_value());
    REQUIRE(first->unit_count == 250);
    REQUIRE(jobs.Requeue(first->id, TaskEvent::Release));

    // Bob asks for a much bigger task and gets the requeued one at ITS size.
    // Re-sizing a carved task would leave a hole in the keyspace or overlap a
    // live task, and neither would be caught by anything downstream.
    const auto again = jobs.Grant(kBob, kNow, kLease, 900);
    REQUIRE(again.has_value());
    CHECK(again->id == first->id);
    CHECK(again->unit_count == 250);
    CHECK(again->start_unit == first->start_unit);
}

TEST_CASE("requeued work is granted before fresh keyspace", "[job]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 1000, 7);
    const auto stale = jobs.Grant(kAlice, kNow, kLease, 100);
    REQUIRE(stale.has_value());
    REQUIRE(jobs.Requeue(stale->id, TaskEvent::Release));

    // Work that has already failed once is the work most at risk of being
    // forgotten at the tail of a job — and the tail is what dominates
    // completion time, which is why 2.17's speculation exists at all.
    const auto next = jobs.Grant(kBob, kNow, kLease, 100);
    REQUIRE(next.has_value());
    CHECK(next->id == stale->id);
}

TEST_CASE("a leased task is never handed to a second worker", "[job]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 10000, 7);

    const auto first = jobs.Grant(kAlice, kNow, kLease, kUnits);
    REQUIRE(first.has_value());
    CHECK(jobs.queued() == 0);
    CHECK(jobs.Find(first->id)->state == TaskState::Leased);
    CHECK(jobs.Find(first->id)->holder == kAlice);
    CHECK(jobs.Find(first->id)->lease_expires_at_ms == kNow + kLease);

    // Bob gets FRESH keyspace, never Alice's task. Handing the same task to two
    // workers would look like successful speculation, which is a Phase 3
    // feature with completely different bookkeeping.
    const auto second = jobs.Grant(kBob, kNow, kLease, kUnits);
    REQUIRE(second.has_value());
    CHECK(second->id != first->id);
    CHECK(second->start_unit == first->start_unit + first->unit_count);
    CHECK(jobs.Find(first->id)->holder == kAlice);   // untouched
}

TEST_CASE("Submit requires the lease (invariant 5)", "[job]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 10000, 7);
    const auto task = jobs.Grant(kAlice, kNow, kLease, kUnits);
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
    (void)jobs.CreateJob("k", 10000, 7);
    const auto task = jobs.Grant(kAlice, kNow, kLease, kUnits);
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
    CHECK(jobs.Grant(kAlice, kNow, kLease, kUnits).has_value());
}

TEST_CASE("Invariant 6 bars a worker that already submitted", "[job]") {
    JobManager jobs;
    // Exactly one task's worth, so "Alice gets nothing" cannot be satisfied by
    // handing her fresh keyspace instead.
    (void)jobs.CreateJob("k", kUnits, 7);
    const auto task = jobs.Grant(kAlice, kNow, kLease, kUnits);
    REQUIRE(task.has_value());
    REQUIRE(jobs.Submit(kAlice, task->id));

    // Disputed result: back to the queue for a second opinion.
    REQUIRE(jobs.Requeue(task->id, TaskEvent::Disagreement));
    REQUIRE(jobs.Requeue(task->id, TaskEvent::IssueReplica));
    CHECK(jobs.Find(task->id)->state == TaskState::Queued);

    // Now Alice must NOT get it back — a worker agreeing with itself is not
    // evidence, and a liar would validate its own lie.
    CHECK_FALSE(jobs.Grant(kAlice, kNow, kLease, kUnits).has_value());
    CHECK(jobs.Grant(kBob, kNow, kLease, kUnits).has_value());
}

TEST_CASE("HeldBy lists exactly the leases a worker holds", "[job]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 10000, 7);

    const auto a1 = jobs.Grant(kAlice, kNow, kLease, kUnits);
    const auto a2 = jobs.Grant(kAlice, kNow, kLease, kUnits);
    const auto b1 = jobs.Grant(kBob, kNow, kLease, kUnits);
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
    // Exactly two tasks' worth: the keyspace must be exhausted for completion
    // to be possible at all (D-0043 makes that a second condition).
    const JobId job = jobs.CreateJob("k", 2 * kUnits, 7);

    const auto t1 = jobs.Grant(kAlice, kNow, kLease, kUnits);
    const auto t2 = jobs.Grant(kBob, kNow, kLease, kUnits);
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

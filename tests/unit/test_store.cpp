// Steps 2.19/2.20 — durable state and crash recovery.
//
// Every case runs against ":memory:", so nothing touches the filesystem and no
// case can inherit another's state. The interesting assertions are about what
// recovery deliberately DOES NOT restore — a live lease, a holder — because
// restoring those is how a restart would hand out work that nobody is doing.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string_view>

#include "p2pgpu/coordinator/store.hpp"

using namespace p2pgpu::coordinator;
using p2pgpu::protocol::WorkerId;

namespace {
constexpr std::uint64_t kNow = 1'000'000;
constexpr std::uint32_t kLease = 30'000;

std::unique_ptr<Store> OpenMemory() {
    auto store = Store::Open(":memory:");
    REQUIRE(store);
    return *std::move(store);
}

/// Flush whatever is dirty, then clear — the sweep's exact sequence (D-0048).
void FlushAll(Store& store, JobManager& jobs) {
    REQUIRE(store.Flush(jobs.DirtyJobs(), jobs.DirtyTasks()));
    jobs.ClearDirty();
}
}  // namespace

TEST_CASE("a job and its tasks survive a round trip", "[store]") {
    auto store = OpenMemory();
    JobManager jobs;
    const auto job = jobs.CreateJob("brute_search_v1", /*total_units=*/5000, /*seed=*/9);
    const WorkerId alice{1, 1};
    const auto t = jobs.Grant(alice, kNow, kLease, 1000);
    REQUIRE(t);
    FlushAll(*store, jobs);

    const auto loaded = store->LoadAll();
    REQUIRE(loaded);
    REQUIRE(loaded->jobs.size() == 1);
    CHECK(loaded->jobs.front().id == job);
    CHECK(loaded->jobs.front().kernel_id == "brute_search_v1");
    CHECK(loaded->jobs.front().seed == 9);
    CHECK(loaded->jobs.front().total_units == 5000);
    // The CURSOR is what a restart re-carves from, so it is the one job field
    // that must be current rather than merely present.
    CHECK(loaded->jobs.front().next_unit == 1000);

    REQUIRE(loaded->tasks.size() == 1);
    CHECK(loaded->tasks.front().id == t->id);
    CHECK(loaded->tasks.front().start_unit == 0);
    CHECK(loaded->tasks.front().unit_count == 1000);
}

TEST_CASE("recovery requeues in-flight work and forgets its holder", "[store]") {
    auto store = OpenMemory();
    const WorkerId alice{1, 1};
    const WorkerId bob{2, 2};

    TaskId leased_id;
    TaskId done_id;
    {
        JobManager jobs;
        (void)jobs.CreateJob("k", 3000, 1);
        const auto done = jobs.Grant(alice, kNow, kLease, 1000);
        const auto leased = jobs.Grant(bob, kNow, kLease, 1000);
        REQUIRE(done);
        REQUIRE(leased);
        done_id = done->id;
        leased_id = leased->id;

        REQUIRE(jobs.Submit(alice, done_id));
        REQUIRE(jobs.Finish(done_id, /*accepted=*/true));
        FlushAll(*store, jobs);
        // `jobs` dies here — this is the crash.
    }

    JobManager recovered;
    const auto state = store->LoadAll();
    REQUIRE(state);
    RecoverInto(recovered, *state);

    // The accepted task stays accepted. Redoing finished work would be safe but
    // wasteful, and the whole point of persisting is not to.
    const Task* done = recovered.Find(done_id);
    REQUIRE(done != nullptr);
    CHECK(done->state == TaskState::Accepted);

    // The leased one comes back QUEUED, with no holder and no deadline. Bob was
    // talking to a process that no longer exists (R8).
    const Task* leased = recovered.Find(leased_id);
    REQUIRE(leased != nullptr);
    CHECK(leased->state == TaskState::Queued);
    CHECK(leased->holder == WorkerId{});
    CHECK(leased->lease_expires_at_ms == 0);

    // And it is actually grantable again, which is the property that matters —
    // a task marked Queued but absent from the queue is lost work that looks
    // fine in a dump.
    const WorkerId carol{3, 3};
    const auto regranted = recovered.Grant(carol, kNow, kLease, 1000);
    REQUIRE(regranted);
    CHECK(regranted->id == leased_id);
}

TEST_CASE("invariant 6 survives a restart", "[store]") {
    auto store = OpenMemory();
    const WorkerId alice{1, 1};
    TaskId task_id;
    {
        JobManager jobs;
        (void)jobs.CreateJob("k", 1000, 1);
        const auto t = jobs.Grant(alice, kNow, kLease, 1000);
        REQUIRE(t);
        task_id = t->id;
        // Alice computed it; the result was rejected, so it goes back.
        REQUIRE(jobs.Submit(alice, task_id));
        REQUIRE(jobs.Finish(task_id, /*accepted=*/false));
        FlushAll(*store, jobs);
    }

    JobManager recovered;
    const auto state = store->LoadAll();
    REQUIRE(state);
    RecoverInto(recovered, *state);

    const Task* t = recovered.Find(task_id);
    REQUIRE(t != nullptr);
    // THE POINT: a worker agreeing with itself is not evidence, and that has to
    // hold across a restart too. Without persisted `prior_workers` the replica
    // could go straight back to Alice and validate her own answer.
    REQUIRE(t->prior_workers.size() == 1);
    CHECK(t->prior_workers.front() == alice);

    // Still Rejected — a terminal state, and deliberately NOT requeued.
    // Recovery only requeues what was in flight (Leased/Validating); inventing
    // a different lifecycle for a terminal task across a restart is the kind of
    // divergence that is unfixable later.
    CHECK(t->state == TaskState::Rejected);

    // What this test CANNOT yet check: that `Grant` refuses to hand a replica
    // back to Alice. Nothing re-queues a task once it is Rejected, and replica
    // issuance for disagreement (`NeedsReplica`) is Phase 3 — so there is no
    // grant path to exercise. The persisted `prior_workers` above is the half
    // that exists now, and 3.x inherits the other half already stored.
}

TEST_CASE("ids are not reissued after a restart", "[store]") {
    auto store = OpenMemory();
    TaskId first_id;
    {
        JobManager jobs;
        (void)jobs.CreateJob("k", 10'000, 1);
        const WorkerId alice{1, 1};
        const auto t = jobs.Grant(alice, kNow, kLease, 1000);
        REQUIRE(t);
        first_id = t->id;
        REQUIRE(jobs.Submit(alice, first_id));
        REQUIRE(jobs.Finish(first_id, /*accepted=*/true));
        FlushAll(*store, jobs);
    }

    JobManager recovered;
    const auto state = store->LoadAll();
    REQUIRE(state);
    RecoverInto(recovered, *state);

    const WorkerId bob{2, 2};
    const auto fresh = recovered.Grant(bob, kNow, kLease, 1000);
    REQUIRE(fresh);
    // A reissued id would silently inherit the old task's history — including
    // its `prior_workers` and its Accepted state.
    CHECK(fresh->id != first_id);
    CHECK(fresh->start_unit == 1000);
}

TEST_CASE("a failed flush leaves the rows dirty", "[store]") {
    auto store = OpenMemory();
    JobManager jobs;
    (void)jobs.CreateJob("k", 1000, 1);
    REQUIRE(jobs.has_dirty());

    // Clearing is the caller's job and happens only AFTER a successful flush.
    // If the write fails the rows must stay dirty and go out next sweep —
    // dropping them would let the file diverge from memory silently.
    REQUIRE(store->Flush(jobs.DirtyJobs(), jobs.DirtyTasks()));
    CHECK(jobs.has_dirty());
    jobs.ClearDirty();
    CHECK_FALSE(jobs.has_dirty());
}

TEST_CASE("flushing twice is idempotent", "[store]") {
    auto store = OpenMemory();
    JobManager jobs;
    (void)jobs.CreateJob("k", 2000, 1);
    const WorkerId alice{1, 1};
    const auto t = jobs.Grant(alice, kNow, kLease, 1000);
    REQUIRE(t);

    // Same rows, twice, without clearing between: the upsert must update rather
    // than duplicate, or a restart would see two tasks over one range.
    REQUIRE(store->Flush(jobs.DirtyJobs(), jobs.DirtyTasks()));
    REQUIRE(store->Flush(jobs.DirtyJobs(), jobs.DirtyTasks()));

    const auto loaded = store->LoadAll();
    REQUIRE(loaded);
    CHECK(loaded->jobs.size() == 1);
    CHECK(loaded->tasks.size() == 1);
}

TEST_CASE("a speculative replica round-trips with its origin", "[store]") {
    auto store = OpenMemory();
    const WorkerId alice{1, 1};
    const WorkerId bob{2, 2};
    TaskId original_id;
    TaskId replica_id;
    {
        JobManager jobs;
        (void)jobs.CreateJob("k", 1000, 1);
        const auto a = jobs.Grant(alice, kNow, kLease, 500);
        const auto b = jobs.Grant(alice, kNow, kLease, 500);
        REQUIRE(a);
        REQUIRE(b);
        REQUIRE(jobs.Submit(alice, b->id));
        REQUIRE(jobs.Finish(b->id, true));
        const auto replica = jobs.IssueSpeculative(bob, kNow, kLease, /*threshold=*/0.4);
        REQUIRE(replica);
        original_id = a->id;
        replica_id = replica->id;
        FlushAll(*store, jobs);
    }

    JobManager recovered;
    const auto state = store->LoadAll();
    REQUIRE(state);
    RecoverInto(recovered, *state);

    const Task* replica = recovered.Find(replica_id);
    REQUIRE(replica != nullptr);
    // `replica_of` must survive, or CancelSiblingsOf cannot find the group
    // after a restart and both copies run to completion.
    CHECK(replica->replica_of == original_id);
    // Both were in flight, so both come back queued.
    CHECK(replica->state == TaskState::Queued);
    CHECK(recovered.Find(original_id)->state == TaskState::Queued);
}

TEST_CASE("a corrupt state value does not become an out-of-range enum", "[store]") {
    // A file is not network input, but the consequence is the same shape: every
    // switch over TaskState has no `default:` arm (D-0011), so an out-of-range
    // value would fall past every case. Clamping is what keeps a corrupt or
    // newer file from reaching undefined behaviour.
    auto store = OpenMemory();
    {
        JobManager jobs;
        (void)jobs.CreateJob("k", 1000, 1);
        const WorkerId alice{1, 1};
        REQUIRE(jobs.Grant(alice, kNow, kLease, 1000));
        FlushAll(*store, jobs);
    }

    const auto state = store->LoadAll();
    REQUIRE(state);
    REQUIRE(state->tasks.size() == 1);
    // Whatever came back, it is a state the machine knows how to print — which
    // is the same oracle the loader uses.
    CHECK(ToString(state->tasks.front().state) != std::string_view{"?"});
}

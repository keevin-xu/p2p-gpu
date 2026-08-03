// Lease lifecycle — steps 2.6-2.10. THE CORRECTNESS CORE OF FAULT TOLERANCE.
//
// R8 says a worker disappearing mid-task is the NORMAL case. Everything here is
// about that being true rather than aspirational: expiry returns work, loss
// releases it, and none of it costs the worker anything. The failures these
// guard against do not crash — they lose work quietly, or punish honest
// workers for having bad wifi.

#include <catch2/catch_test_macros.hpp>

#include "p2pgpu/coordinator/fleet.hpp"
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

// ── 2.6 renewal ──────────────────────────────────────────────────────────

TEST_CASE("renewal moves the deadline and nothing else", "[lease]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 10000, 7);
    const auto task = jobs.Grant(kAlice, kNow, kLease, kUnits);
    REQUIRE(task.has_value());
    CHECK(jobs.Find(task->id)->lease_expires_at_ms == kNow + kLease);

    REQUIRE(jobs.RenewLease(kAlice, task->id, kNow + 5000, kLease));

    // The DEADLINE moves; the state does not. Modelling renewal as a state
    // change would make it indistinguishable from a fresh grant, and the two
    // mean opposite things to the sweep.
    CHECK(jobs.Find(task->id)->lease_expires_at_ms == kNow + 5000 + kLease);
    CHECK(jobs.Find(task->id)->state == TaskState::Leased);
    CHECK(jobs.Find(task->id)->holder == kAlice);
}

TEST_CASE("only the holder may renew", "[lease]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 10000, 7);
    const auto task = jobs.Grant(kAlice, kNow, kLease, kUnits);
    REQUIRE(task.has_value());

    // Without this check any connected peer could keep another worker's lease
    // alive forever — a denial of service on the queue, not a protocol nicety.
    CHECK_FALSE(jobs.RenewLease(kBob, task->id, kNow + 5000, kLease));
    CHECK(jobs.Find(task->id)->lease_expires_at_ms == kNow + kLease);
}

TEST_CASE("a queued task cannot be renewed", "[lease]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 10000, 7);
    const auto task = jobs.Grant(kAlice, kNow, kLease, kUnits);
    REQUIRE(task.has_value());
    REQUIRE(jobs.Requeue(task->id, TaskEvent::Release));

    // Alice no longer holds it. A renewal arriving after the sweep took the
    // task back must not resurrect her claim — that task may already belong to
    // somebody else.
    CHECK_FALSE(jobs.RenewLease(kAlice, task->id, kNow + 1000, kLease));
}

// ── 2.7 expiry sweep ─────────────────────────────────────────────────────

TEST_CASE("the sweep expires only what is actually overdue", "[lease]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 10000, 7);
    const auto a = jobs.Grant(kAlice, kNow, 1000, kUnits);
    const auto b = jobs.Grant(kBob, kNow, 60'000, kUnits);
    REQUIRE(a);
    REQUIRE(b);

    // Validity is the HALF-OPEN interval [grant, expires). One millisecond
    // before the deadline is still valid; at the deadline it has expired.
    //
    // That is what makes a 1000 ms lease last exactly 1000 ms — treating the
    // deadline as still-valid would make it 1001, and the same off-by-one
    // repeated in the sizer's "never exceed lease duration" clamp is how a task
    // gets sized to outlive its own lease.
    CHECK(jobs.SweepExpiredLeases(kNow + 999).empty());

    const auto expired = jobs.SweepExpiredLeases(kNow + 1000);
    REQUIRE(expired.size() == 1);
    CHECK(expired[0].task == a->id);
    // The HOLDER comes back too: an expiry is evidence about that worker's
    // speed, and the sizer's correction needs to know whose it was (D-0044).
    CHECK(expired[0].holder == kAlice);
    CHECK(jobs.Find(a->id)->state == TaskState::Queued);
    CHECK(jobs.Find(a->id)->holder == WorkerId{});
    // Bob's is untouched.
    CHECK(jobs.Find(b->id)->state == TaskState::Leased);
}

TEST_CASE("expiry costs the worker nothing", "[lease]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 10000, 7);
    const auto task = jobs.Grant(kAlice, kNow, 1000, kUnits);
    REQUIRE(task.has_value());
    REQUIRE(jobs.SweepExpiredLeases(kNow + 2000).size() == 1);

    // R8: absence is not malice. The worker is not in prior_workers, so
    // invariant 6 does not bar it — a machine that slept and woke may have the
    // task back. Barring it would shrink the eligible set every time a laptop
    // closed, which on a volunteer grid is constantly.
    CHECK(jobs.Find(task->id)->prior_workers.empty());
    CHECK(jobs.Grant(kAlice, kNow + 3000, kLease, kUnits).has_value());
}

TEST_CASE("a renewed lease survives the sweep", "[lease]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 10000, 7);
    const auto task = jobs.Grant(kAlice, kNow, 1000, kUnits);
    REQUIRE(task.has_value());

    REQUIRE(jobs.RenewLease(kAlice, task->id, kNow + 900, 1000));
    CHECK(jobs.SweepExpiredLeases(kNow + 1500).empty());   // would have expired
    CHECK(jobs.Find(task->id)->state == TaskState::Leased);

    // ...and still expires once renewal stops. THE POINT: renewal buys time,
    // it does not make a lease permanent.
    CHECK(jobs.SweepExpiredLeases(kNow + 5000).size() == 1);
}

TEST_CASE("the sweep is idempotent", "[lease]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 10000, 7);
    REQUIRE(jobs.Grant(kAlice, kNow, 1000, kUnits).has_value());

    CHECK(jobs.SweepExpiredLeases(kNow + 2000).size() == 1);
    // Running again must not re-queue an already-queued task. The sweep fires
    // every second forever; a second pass finding the same task would duplicate
    // it in the queue and hand one task to two workers.
    CHECK(jobs.SweepExpiredLeases(kNow + 2000).empty());
    CHECK(jobs.queued() == 1);
}

// ── 2.8 / 2.9 loss and drain ─────────────────────────────────────────────

TEST_CASE("releasing a worker's leases returns all of them", "[lease]") {
    JobManager jobs;
    (void)jobs.CreateJob("k", 10000, 7);
    REQUIRE(jobs.Grant(kAlice, kNow, kLease, kUnits));
    REQUIRE(jobs.Grant(kAlice, kNow, kLease, kUnits));
    REQUIRE(jobs.Grant(kBob, kNow, kLease, kUnits));

    CHECK(jobs.ReleaseAllHeldBy(kAlice) == 2);
    CHECK(jobs.HeldBy(kAlice).empty());
    CHECK(jobs.HeldBy(kBob).size() == 1);   // Bob is untouched
    // Alice's two, and only those. Fresh keyspace is NOT in the queue — nothing
    // is queued until it has been carved and come back (D-0043).
    CHECK(jobs.queued() == 2);
}

TEST_CASE("a worker is lost by OUR clock, not its own claim", "[fleet]") {
    Fleet fleet;
    fleet.Join(kAlice, 1, kNow);
    fleet.Join(kBob, 2, kNow);

    fleet.Touch(kAlice, kNow + 10'000);   // Alice sent something; Bob went quiet

    const auto lost = fleet.FindLost(kNow + 20'000, /*timeout_ms=*/15'000);
    REQUIRE(lost.size() == 1);
    CHECK(lost[0] == kBob);
}

TEST_CASE("elapsed time cannot underflow", "[fleet]") {
    Fleet fleet;
    // A record stamped in the future — a clock adjustment, or a frame processed
    // slightly out of order. `now - last_seen` on unsigned values would wrap to
    // an enormous elapsed time and declare a live worker dead.
    fleet.Join(kAlice, 1, kNow + 60'000);
    CHECK(fleet.FindLost(kNow, /*timeout_ms=*/1000).empty());
}

TEST_CASE("a lost worker leaves the fleet but its work does not", "[fleet]") {
    JobManager jobs;
    Fleet fleet;
    (void)jobs.CreateJob("k", 200, 7);
    fleet.Join(kAlice, 1, kNow);
    REQUIRE(jobs.Grant(kAlice, kNow, kLease, kUnits));

    fleet.Leave(kAlice);
    CHECK(fleet.size() == 0);
    // The task outlives the worker holding it — that is the whole point of
    // leasing. It is still Leased until something releases or expires it.
    CHECK(jobs.HeldBy(kAlice).size() == 1);
    CHECK(jobs.ReleaseAllHeldBy(kAlice) == 1);
    CHECK(jobs.queued() == 1);   // only the carved task; fresh keyspace is not queued
}

TEST_CASE("a worker that just joined is not immediately lost", "[fleet]") {
    Fleet fleet;
    // Stamped with a REAL clock value. Joining with 0 makes
    // `0 + timeout < now` true on the very first sweep, so every worker is
    // declared lost within a second of connecting — and since it holds no
    // leases yet, `released=0` makes the log look harmless while the worker is
    // silently removed and can never be granted work.
    fleet.Join(kAlice, 1, kNow);
    CHECK(fleet.FindLost(kNow, /*timeout_ms=*/45'000).empty());
    CHECK(fleet.FindLost(kNow + 1000, /*timeout_ms=*/45'000).empty());
    CHECK(fleet.FindLost(kNow + 46'000, /*timeout_ms=*/45'000).size() == 1);
}

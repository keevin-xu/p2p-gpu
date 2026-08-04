// Step 2.21 — metrics.
//
// The property worth testing is that metrics are DERIVED rather than counted:
// a snapshot must agree with the scheduler no matter what path got it there.
// A separately maintained counter would pass a test written against the same
// increment that maintains it, so these cases drive the JobManager through real
// operations and then ask the snapshot what it sees.

#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "p2pgpu/coordinator/metrics.hpp"

using namespace p2pgpu::coordinator;
using p2pgpu::protocol::WorkerId;

namespace {
constexpr std::uint64_t kNow = 1'000'000;
constexpr std::uint32_t kLease = 30'000;
}  // namespace

TEST_CASE("a snapshot agrees with the scheduler it describes", "[metrics]") {
    JobManager jobs;
    Fleet fleet;
    (void)jobs.CreateJob("k", /*total_units=*/3000, /*seed=*/1);

    const WorkerId alice{1, 1};
    fleet.Join(alice, /*conn_id=*/1, kNow);
    const auto t1 = jobs.Grant(alice, kNow, kLease, 1000);
    const auto t2 = jobs.Grant(alice, kNow, kLease, 1000);
    REQUIRE(t1);
    REQUIRE(t2);
    REQUIRE(jobs.Submit(alice, t1->id));
    REQUIRE(jobs.Finish(t1->id, /*accepted=*/true));

    const Snapshot snap = Collect(jobs, fleet, /*rejected_frames=*/7, kNow);

    CHECK(snap.workers == 1);
    CHECK(snap.total_tasks == 2);
    CHECK(snap.units_total == 3000);
    CHECK(snap.units_remaining == 1000);
    CHECK(snap.rejected_frames == 7);

    // Counts are indexed by TaskState, so these read as "one accepted, one
    // still leased" without the snapshot needing to know which is which.
    CHECK(snap.tasks_by_state.at(static_cast<std::size_t>(TaskState::Accepted)) == 1);
    CHECK(snap.tasks_by_state.at(static_cast<std::size_t>(TaskState::Leased)) == 1);
    CHECK(snap.tasks_by_state.at(static_cast<std::size_t>(TaskState::Queued)) == 0);
}

TEST_CASE("state counts cover every state, including any newly added one",
          "[metrics]") {
    JobManager jobs;
    Fleet fleet;
    const Snapshot snap = Collect(jobs, fleet, 0, kNow);

    // Sized from the enum rather than a literal, so a new TaskState widens this
    // automatically. The check that it is WIDE ENOUGH is the point: a hardcoded
    // 6 here would drop `Cancelled` off the dashboard silently.
    REQUIRE(snap.tasks_by_state.size() ==
            static_cast<std::size_t>(TaskState::Cancelled) + 1);
    for (std::size_t i = 0; i < snap.tasks_by_state.size(); ++i) {
        CHECK(ToString(static_cast<TaskState>(i)) != std::string_view{"?"});
    }
}

TEST_CASE("a worker that has completed nothing reports 0, never NaN", "[metrics]") {
    JobManager jobs;
    Fleet fleet;
    const WorkerId alice{1, 1};
    fleet.Join(alice, 1, kNow);

    const Snapshot snap = Collect(jobs, fleet, 0, kNow);
    REQUIRE(snap.fleet.size() == 1);
    // 0/0 would be NaN, which is not JSON and would break the dashboard's parse
    // for the WHOLE document rather than just this field.
    CHECK(snap.fleet.front().observed_units_per_sec == 0.0);

    const std::string json = ToJson(snap);
    CHECK(json.find("nan") == std::string::npos);
    CHECK(json.find("inf") == std::string::npos);
}

TEST_CASE("wasted units are reported from the scheduler, not recounted",
          "[metrics]") {
    JobManager jobs;
    Fleet fleet;
    (void)jobs.CreateJob("k", 1000, 1);
    const WorkerId alice{1, 1};
    const WorkerId bob{2, 2};
    fleet.Join(alice, 1, kNow);
    fleet.Join(bob, 2, kNow);

    const auto a = jobs.Grant(alice, kNow, kLease, 500);
    const auto b = jobs.Grant(alice, kNow, kLease, 500);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(jobs.Submit(alice, b->id));
    REQUIRE(jobs.Finish(b->id, true));
    const auto replica = jobs.IssueSpeculative(bob, kNow, kLease, /*threshold=*/0.4);
    REQUIRE(replica);

    REQUIRE(jobs.Submit(alice, a->id));
    REQUIRE(jobs.Finish(a->id, true));
    const auto cancelled = jobs.CancelSiblingsOf(a->id);
    REQUIRE(cancelled.size() == 1);

    const Snapshot snap = Collect(jobs, fleet, 0, kNow);
    CHECK(snap.wasted_units == jobs.wasted_units());
    CHECK(snap.wasted_units > 0);
    CHECK(snap.tasks_by_state.at(static_cast<std::size_t>(TaskState::Cancelled)) == 1);
}

TEST_CASE("the JSON carries state names alongside the counts", "[metrics]") {
    JobManager jobs;
    Fleet fleet;
    const std::string json = ToJson(Collect(jobs, fleet, 0, kNow));

    // The dashboard labels its columns from this, so a hardcoded list on the
    // page cannot drift out of sync with the enum the scheduler switches on.
    CHECK(json.find("\"state_names\"") != std::string::npos);
    CHECK(json.find("\"Cancelled\"") != std::string::npos);
    CHECK(json.find("\"NeedsReplica\"") != std::string::npos);
    CHECK(json.find("\"tasks_by_state\"") != std::string::npos);
}

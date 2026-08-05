// Step 3.9 — spot-checking.
//
// The property that makes this worth having: it convicts a liar from ONE
// result, because the coordinator already knows the answer. That is what can
// move the measured 1.84x replication overhead (D-0057) toward 1.0.
//
// The property that makes it safe: a spot-check is indistinguishable from real
// work. There is no field for a worker to read, and these tests assert the
// selection rules rather than any marker, because a marker is the one thing
// that would break it.

#include <catch2/catch_test_macros.hpp>

#include "p2pgpu/coordinator/spot_check.hpp"

using namespace p2pgpu::coordinator;
using p2pgpu::protocol::TaskId;

TEST_CASE("an empty pool never injects", "[spot_check]") {
    const SpotCheckPool pool;
    // Nothing known means nothing to test against. Injecting a range whose
    // answer we do not hold would be a task we then cannot judge.
    CHECK_FALSE(pool.Maybe(0.5, 0.0, 0).has_value());
}

TEST_CASE("unproven workers are tested more than established ones",
          "[spot_check]") {
    SpotCheckPool pool;
    pool.Remember(0, 1000, 0xAAAA);
    const auto& cfg = pool.config();

    // A roll between the two rates fires for the unproven worker and not the
    // trusted one — that ordering IS the policy.
    const double roll = (cfg.rate_trusted + cfg.rate_unproven) / 2.0;
    CHECK(pool.Maybe(0.5, roll, 0).has_value());
    CHECK_FALSE(pool.Maybe(0.99, roll, 0).has_value());
}

TEST_CASE("established workers are still tested sometimes", "[spot_check]") {
    SpotCheckPool pool;
    pool.Remember(0, 1000, 0xAAAA);
    // A ZERO rate here would make becoming trusted a permanent licence to start
    // lying, which is exactly the adversary a reputation system invites.
    CHECK(pool.config().rate_trusted > 0.0);
    CHECK(pool.Maybe(0.99, pool.config().rate_trusted / 2.0, 0).has_value());
}

TEST_CASE("a spot-check carries the expected answer and can be resolved",
          "[spot_check]") {
    SpotCheckPool pool;
    pool.Remember(500, 1000, 0xBEEF);
    const auto pick = pool.Maybe(0.5, 0.0, 0);
    REQUIRE(pick.has_value());
    CHECK(pick->start_unit == 500);
    CHECK(pick->expected_checksum == 0xBEEF);

    const TaskId t{0, 42};
    pool.MarkIssued(t, pick->expected_checksum);
    const auto expected = pool.ExpectedFor(t);
    REQUIRE(expected.has_value());
    CHECK(*expected == 0xBEEF);

    // A task nobody marked is not a spot-check, and must not be treated as one
    // — judging an ordinary task against an answer we never held would
    // manufacture convictions.
    CHECK_FALSE(pool.ExpectedFor(TaskId{0, 43}).has_value());
}

TEST_CASE("resolved spot-checks are forgotten", "[spot_check]") {
    SpotCheckPool pool;
    pool.Remember(0, 1000, 1);
    pool.MarkIssued(TaskId{0, 1}, 1);
    CHECK(pool.outstanding() == 1);
    pool.Forget(TaskId{0, 1});
    // Unbounded growth over a long uptime is a slow leak, not a crash, which is
    // the kind that survives testing.
    CHECK(pool.outstanding() == 0);
}

TEST_CASE("the same range is never remembered twice", "[spot_check]") {
    SpotCheckPool pool;
    for (int i = 0; i < 10; ++i) {
        pool.Remember(0, 1000, 0xAAAA);
    }
    // Duplicates would bias every injection toward one range, and a worker that
    // saw the same range repeatedly would learn it is being tested.
    CHECK(pool.size() == 1);
}

TEST_CASE("a zero-length range is not remembered", "[spot_check]") {
    SpotCheckPool pool;
    pool.Remember(0, 0, 0xAAAA);
    CHECK(pool.size() == 0);
}

TEST_CASE("the pool is bounded", "[spot_check]") {
    SpotCheckPool pool;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        pool.Remember(i * 1000, 1000, i);
    }
    // A coordinator with a long uptime would otherwise accumulate one entry per
    // validated task forever, and the pool's value does not grow with size.
    CHECK(pool.size() <= 256);
    // The OLDEST were dropped, so the set stays fresh rather than fossilising
    // into a fixed list a worker could come to recognise.
    CHECK(pool.Maybe(0.5, 0.0, 0)->start_unit > 0);
}

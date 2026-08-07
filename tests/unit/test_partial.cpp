// Partial results — step 5.13, D-0074.
//
// The properties that matter are all about what a partial must NOT do: it must
// not advance the state machine (D-0031's wedge), must not let a stale snapshot
// win, and must not be accepted from a worker without the lease.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "p2pgpu/coordinator/job.hpp"

using namespace p2pgpu::coordinator;
using p2pgpu::protocol::TaskId;
using p2pgpu::protocol::WorkerId;

namespace {

std::vector<std::byte> Bytes(std::size_t n, unsigned char fill) {
    return std::vector<std::byte>(n, static_cast<std::byte>(fill));
}

struct Fixture {
    JobManager jobs;
    WorkerId worker{1, 1};
    WorkerId other{2, 2};
    TaskId task;

    Fixture() {
        (void)jobs.CreateJob("pathtrace_tile_v1", 4096, 4096);
        const auto granted = jobs.Grant(worker, 1000, 5000, 4096, {});
        REQUIRE(granted.has_value());
        task = granted->id;
    }
};

}  // namespace

TEST_CASE("a partial does NOT advance the task state machine", "[partial]") {
    // D-0031's wedge, from the other direction. If a partial reached `Submit`
    // the task would move to Validating, from which `Release` is not a legal
    // edge — no lease for the sweep to expire, no worker to resubmit, and the
    // job never completes.
    Fixture fx;
    REQUIRE(fx.jobs.Find(fx.task)->state == TaskState::Leased);

    REQUIRE(fx.jobs.RecordPartial(fx.worker, fx.task, 1, 1024, Bytes(64, 0xAA),
                                  1100, 5000));

    CHECK(fx.jobs.Find(fx.task)->state == TaskState::Leased);
    CHECK(fx.jobs.Find(fx.task)->holder == fx.worker);
}

TEST_CASE("a partial renews the lease", "[partial]") {
    // 5.13's "lease renewal tied to upload cadence". The 2.6/D-0046 rule from
    // the other side: never demand a separate heartbeat from a worker that is
    // visibly delivering results.
    Fixture fx;
    const std::uint64_t before = fx.jobs.Find(fx.task)->lease_expires_at_ms;

    REQUIRE(fx.jobs.RecordPartial(fx.worker, fx.task, 1, 1024, Bytes(64, 0x01),
                                  3000, 5000));
    CHECK(fx.jobs.Find(fx.task)->lease_expires_at_ms > before);
    CHECK(fx.jobs.Find(fx.task)->lease_expires_at_ms == 3000 + 5000);
}

TEST_CASE("a newer partial supersedes; a stale or repeated one is dropped",
          "[partial]") {
    Fixture fx;
    REQUIRE(fx.jobs.RecordPartial(fx.worker, fx.task, 5, 2048, Bytes(64, 0x05),
                                  1100, 5000));
    REQUIRE(fx.jobs.LatestPartial(fx.task) != nullptr);
    CHECK(fx.jobs.LatestPartial(fx.task)->sequence == 5);
    CHECK(fx.jobs.LatestPartial(fx.task)->units_done == 2048);

    // Strictly greater wins.
    REQUIRE(fx.jobs.RecordPartial(fx.worker, fx.task, 6, 3072, Bytes(64, 0x06),
                                  1200, 5000));
    CHECK(fx.jobs.LatestPartial(fx.task)->sequence == 6);
    CHECK(fx.jobs.LatestPartial(fx.task)->units_done == 3072);

    // A LOWER sequence is a reordered or replayed snapshot. Accepting it would
    // roll the tile backwards — an image that un-converges, which is miserable
    // to diagnose because nothing errors.
    CHECK_FALSE(fx.jobs.RecordPartial(fx.worker, fx.task, 4, 1024, Bytes(64, 0x04),
                                      1300, 5000));
    CHECK(fx.jobs.LatestPartial(fx.task)->sequence == 6);
    CHECK(fx.jobs.LatestPartial(fx.task)->units_done == 3072);

    // EQUAL counts as stale too: a retransmission after a reconnect carries the
    // same sequence, and permitting it would be harmless today while quietly
    // allowing a genuine reorder tomorrow.
    CHECK_FALSE(fx.jobs.RecordPartial(fx.worker, fx.task, 6, 9999, Bytes(64, 0x07),
                                      1400, 5000));
    CHECK(fx.jobs.LatestPartial(fx.task)->units_done == 3072);
}

TEST_CASE("a partial from a worker without the lease is refused", "[partial]") {
    // Invariant 5. Without this, anyone could write a snapshot into someone
    // else's task — and, because a partial renews the lease, pin it out of the
    // queue indefinitely.
    Fixture fx;
    CHECK_FALSE(fx.jobs.RecordPartial(fx.other, fx.task, 1, 1024, Bytes(64, 0x01),
                                      1100, 5000));
    CHECK(fx.jobs.LatestPartial(fx.task) == nullptr);
}

TEST_CASE("the payload is stored verbatim and replaced wholesale", "[partial]") {
    // A partial is a FULL snapshot, never a delta: merging deltas would need
    // the coordinator to know the payload's arithmetic, which is kernel-specific
    // knowledge R1 keeps out of it.
    Fixture fx;
    REQUIRE(fx.jobs.RecordPartial(fx.worker, fx.task, 1, 1024, Bytes(64, 0xAA),
                                  1100, 5000));
    CHECK(fx.jobs.LatestPartial(fx.task)->payload == Bytes(64, 0xAA));

    REQUIRE(fx.jobs.RecordPartial(fx.worker, fx.task, 2, 2048, Bytes(32, 0xBB),
                                  1200, 5000));
    const auto* latest = fx.jobs.LatestPartial(fx.task);
    CHECK(latest->payload == Bytes(32, 0xBB));
    CHECK(latest->payload.size() == 32);   // replaced, not appended
}

TEST_CASE("a finished task drops its partial snapshot", "[partial]") {
    // A completed tile's accumulator is dead weight — 64 KiB apiece, and a
    // render is thousands of tiles. "Freed eventually" is how a coordinator
    // with a long uptime runs out of memory (the D-0054 reasoning for
    // submissions, applied here).
    Fixture fx;
    REQUIRE(fx.jobs.RecordPartial(fx.worker, fx.task, 1, 1024, Bytes(4096, 0xAA),
                                  1100, 5000));
    CHECK(fx.jobs.partial_count() == 1);

    REQUIRE(fx.jobs.Submit(fx.worker, fx.task));
    REQUIRE(fx.jobs.Finish(fx.task, true));
    CHECK(fx.jobs.partial_count() == 0);
    CHECK(fx.jobs.LatestPartial(fx.task) == nullptr);
}

TEST_CASE("a task with partials can still complete normally", "[partial]") {
    // The whole point: partials are progress, not a terminal state. The task
    // must still reach Accepted through the ordinary path.
    Fixture fx;
    REQUIRE(fx.jobs.RecordPartial(fx.worker, fx.task, 1, 1024, Bytes(64, 0x01),
                                  1100, 5000));
    REQUIRE(fx.jobs.RecordPartial(fx.worker, fx.task, 2, 2048, Bytes(64, 0x02),
                                  1200, 5000));
    REQUIRE(fx.jobs.Submit(fx.worker, fx.task));
    REQUIRE(fx.jobs.Finish(fx.task, true));
    CHECK(fx.jobs.Find(fx.task)->state == TaskState::Accepted);
}

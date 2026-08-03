// Adaptive sizing — steps 2.12-2.14.
//
// The rules are ORDERED, and the order is the design. These tests pin the
// order, not just the arithmetic — a sizer that applies the R5 floor before the
// lease clamp produces different (and wrong) answers for a slow worker.

#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "p2pgpu/coordinator/sizer.hpp"

using namespace p2pgpu::coordinator;

namespace {
SizingInputs Base() {
    SizingInputs in;
    in.score = 1000.0;          // 1000 units/sec
    in.correction = 1.0;
    in.throttle = 1.0;
    in.target_ms = 2000;        // want ~2 s of work => 2000 units
    in.lease_ms = 30'000;
    in.r5_min_units = 0;
    in.remaining_units = 1'000'000;
    return in;
}
}  // namespace

TEST_CASE("size tracks measured throughput", "[sizer]") {
    auto in = Base();
    CHECK(ComputeTaskSize(in) == 2000);

    // Twice as fast, twice the work — the point of measuring at all.
    in.score = 2000.0;
    CHECK(ComputeTaskSize(in) == 4000);
}

TEST_CASE("a worker with no score gets NOTHING", "[sizer]") {
    auto in = Base();
    in.score = 0.0;
    CHECK(ComputeTaskSize(in) == 0);

    // Not a default, not a guess. A fabricated score would feed the correction
    // factor, and 2.13 would then be correcting toward a number nobody
    // measured.
    in.score = -5.0;
    CHECK(ComputeTaskSize(in) == 0);
    in.score = std::numeric_limits<double>::quiet_NaN();
    CHECK(ComputeTaskSize(in) == 0);
    in.score = std::numeric_limits<double>::infinity();
    CHECK(ComputeTaskSize(in) == 0);
}

TEST_CASE("the correction factor shrinks grants for optimistic benchmarks", "[sizer]") {
    auto in = Base();
    in.correction = 2.0;   // tasks take twice as long as predicted
    CHECK(ComputeTaskSize(in) == 1000);

    in.correction = 0.5;   // faster in practice than at join time
    CHECK(ComputeTaskSize(in) == 4000);
}

TEST_CASE("throttle scales the grant and zero means none", "[sizer]") {
    auto in = Base();
    in.throttle = 0.5;
    CHECK(ComputeTaskSize(in) == 1000);

    // R7: the user's setting is authoritative. Zero is a pause, NOT an error
    // and NOT a disconnect — the coordinator does not get an opinion.
    in.throttle = 0.0;
    CHECK(ComputeTaskSize(in) == 0);
}

TEST_CASE("a task is never sized to outlive its lease", "[sizer]") {
    auto in = Base();
    in.lease_ms = 1000;    // shorter than target_ms
    // Half the lease, by the headroom rule: a task sized to exactly fill its
    // lease expires just as it finishes, because validity is [grant, expires).
    CHECK(ComputeTaskSize(in) == 500);
}

TEST_CASE("the R5 floor WINS over the lease clamp", "[sizer]") {
    auto in = Base();
    in.score = 1.0;              // hopelessly slow
    in.lease_ms = 1000;
    in.r5_min_units = 400'000;

    // The lease clamp would give 0-ish; the floor says 400k. The floor wins
    // (D-0029): below it the kernel is transfer-bound and adding nodes cannot
    // help, so a sub-R5 task is worse than granting nothing — it costs a round
    // trip and a lease to produce a result that cannot pay for itself.
    //
    // A worker too slow to finish this inside a lease will simply keep losing
    // it to expiry, which is the honest outcome rather than a pretend one.
    CHECK(ComputeTaskSize(in) == 400'000);
}

TEST_CASE("the last task takes what remains, even below the R5 floor", "[sizer]") {
    auto in = Base();
    in.r5_min_units = 400'000;
    in.remaining_units = 1234;

    // Correct, and deliberately so: the alternative is leaving a remainder
    // unsearched. R5 is a SIZING rule, not an invariant about every task that
    // exists (D-0043) — worth stating because "every task clears R5" is exactly
    // the sort of thing that gets asserted later.
    CHECK(ComputeTaskSize(in) == 1234);
}

TEST_CASE("no keyspace, no task", "[sizer]") {
    auto in = Base();
    in.remaining_units = 0;
    CHECK(ComputeTaskSize(in) == 0);
}

// ── 2.13 EWMA correction ─────────────────────────────────────────────────

TEST_CASE("the correction converges toward observed reality", "[sizer]") {
    double c = 1.0;
    // A worker consistently 2x slower than predicted.
    for (int i = 0; i < 40; ++i) {
        c = UpdateCorrection(c, 1000.0, 2000.0);
    }
    CHECK(c > 1.9);
    CHECK(c < 2.1);
}

TEST_CASE("one anomalous task does not exile a worker", "[sizer]") {
    double c = 1.0;
    // A 60-second stall on a 2-second task. Unbounded, the ratio is 30 and the
    // worker would be granted single-digit units for a long time afterwards.
    c = UpdateCorrection(c, 2000.0, 60'000.0);
    CHECK(c <= 10.0);

    // ...and it recovers as normal tasks come in. A hiccup is not a verdict
    // (R8's spirit).
    for (int i = 0; i < 30; ++i) {
        c = UpdateCorrection(c, 1000.0, 1000.0);
    }
    CHECK(c < 1.2);
}

TEST_CASE("degenerate samples are ignored, not absorbed", "[sizer]") {
    const double before = 1.5;
    // A zero, a negative, or a NaN would persist through every future grant for
    // this worker. Ignoring one sample is a much smaller failure.
    CHECK(UpdateCorrection(before, 0.0, 1000.0) == before);
    CHECK(UpdateCorrection(before, 1000.0, 0.0) == before);
    CHECK(UpdateCorrection(before, std::numeric_limits<double>::quiet_NaN(), 1.0) == before);
    CHECK(UpdateCorrection(before, 1.0, std::numeric_limits<double>::infinity()) == before);
}

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

// ── D-0050 — the first grant is a probe ──────────────────────────────────
//
// E5 measured that every task over 10 s was a worker's FIRST task, predicted
// 2000 ms against ~41000 ms actual. These pin the fix and, more importantly,
// pin that it does not disturb the rule ORDER it sits in front of.

TEST_CASE("a worker's first task is divided by the probe divisor", "[sizer]") {
    SizingInputs in;
    in.score = 1.0e6;
    in.target_ms = 2000;
    in.lease_ms = 30000;
    in.remaining_units = 1'000'000'000;

    in.cold_start = false;
    const std::uint64_t warm = ComputeTaskSize(in);
    in.cold_start = true;
    const std::uint64_t probe = ComputeTaskSize(in);

    REQUIRE(warm > 0);
    CHECK(probe == warm / kProbeDivisor);
}

TEST_CASE("the probe bounds a wildly inflated score", "[sizer]") {
    // A worker claiming 20x its real speed. The probe does not detect the lie —
    // nothing here can — it bounds what believing it costs, which is the point.
    SizingInputs honest;
    honest.score = 1.0e6;
    honest.target_ms = 2000;
    honest.lease_ms = 30000;
    honest.remaining_units = 1'000'000'000;

    SizingInputs liar = honest;
    liar.score = 20.0e6;
    liar.cold_start = true;

    // Real time the liar's first task takes, at its ACTUAL 1e6 units/sec.
    const double actual_s = static_cast<double>(ComputeTaskSize(liar)) / 1.0e6;
    liar.cold_start = false;
    const double unbounded_s = static_cast<double>(ComputeTaskSize(liar)) / 1.0e6;

    CHECK(unbounded_s > 30.0);   // ~40 s — the E5 tail
    CHECK(actual_s < 6.0);       // ~5 s
}

TEST_CASE("the R5 floor still wins over the probe", "[sizer]") {
    // Rule ORDER is the design (D-0043). The probe divides the throughput term
    // and everything after it still applies — a probe must never become a new
    // way to grant sub-R5 work.
    SizingInputs in;
    in.score = 1.0e6;
    in.target_ms = 2000;
    in.lease_ms = 30000;
    in.r5_min_units = 4'000'000;   // deliberately above the probe size
    in.remaining_units = 1'000'000'000;
    in.cold_start = true;

    CHECK(ComputeTaskSize(in) == in.r5_min_units);
}

TEST_CASE("the probe never exceeds what is left", "[sizer]") {
    SizingInputs in;
    in.score = 1.0e9;
    in.target_ms = 2000;
    in.lease_ms = 30000;
    in.remaining_units = 500;
    in.cold_start = true;

    CHECK(ComputeTaskSize(in) == 500);
}

// ── D-0063 — a task must fit its own params block ────────────────────────
//
// Found in the 4.17 three-way run, not by any test: a browser reporting
// 2.12e12 ops/s was handed the whole 5e9-unit keyspace, and
// `BruteSearchParams::unit_count` is a u32, so it reached the worker as
// 705,032,704 — 5e9 mod 2^32. Every prior task was small only because every
// prior worker was slow.

TEST_CASE("a task never exceeds what the params block can express", "[sizer]") {
    SizingInputs in;
    // A worker fast enough to be given everything, against a keyspace larger
    // than u32 — the exact shape 4.17 produced.
    in.score = 2.0e12;
    in.target_ms = 2000;
    in.lease_ms = 30000;
    in.remaining_units = 5'000'000'000ULL;

    const std::uint64_t units = ComputeTaskSize(in);
    CHECK(units <= kMaxUnitsPerTask);
    // And specifically NOT the truncated value, which is what made the bug so
    // hard to see: 705,032,704 looks like a perfectly ordinary task size.
    CHECK(units != (5'000'000'000ULL % (1ULL << 32)));
}

TEST_CASE("the cap does not shrink a task that already fits", "[sizer]") {
    SizingInputs in;
    in.score = 1.0e6;
    in.target_ms = 2000;
    in.lease_ms = 30000;
    in.remaining_units = 1'000'000'000ULL;
    // A normal task is nowhere near the ceiling; the clamp must be inert here
    // rather than quietly reshaping ordinary sizing.
    CHECK(ComputeTaskSize(in) < kMaxUnitsPerTask);
}

TEST_CASE("the last task of a job is still whatever remains", "[sizer]") {
    SizingInputs in;
    in.score = 2.0e12;
    in.target_ms = 2000;
    in.lease_ms = 30000;
    in.remaining_units = 500;
    // The u32 cap sits before the remaining-units cap, so a tiny remainder is
    // unaffected (D-0043's rule order).
    CHECK(ComputeTaskSize(in) == 500);
}

// ── D-0064 — the correction must be able to express real hardware ────────

TEST_CASE("the correction can express a browser on a discrete GPU", "[sizer]") {
    // 4.17: a browser benchmarked at 2.01e12 ops/s and could not finish a task.
    // It needs ~45x (D-0026's browser submission cost) compounded with the
    // 11.5x per-chunk penalty 0.16 measured on that card. At the old 10x
    // ceiling the estimator could not represent it, so every task expired and
    // the worker never completed one to learn from.
    double c = 1.0;
    for (int i = 0; i < 40; ++i) {
        c = UpdateCorrection(c, /*predicted_ms=*/100.0, /*actual_ms=*/5000.0);
    }
    CHECK(c > 10.0);          // the old ceiling
    CHECK(c <= kMaxCorrection);
    // Converging toward the true ratio (50x), not parked at a limit.
    CHECK(c > 40.0);
}

TEST_CASE("the correction still refuses absurd values", "[sizer]") {
    // The clamp exists to reject nonsense, and raising it must not remove that.
    double c = 1.0;
    for (int i = 0; i < 200; ++i) {
        c = UpdateCorrection(c, 1.0, 1.0e9);
    }
    CHECK(c <= kMaxCorrection);
}

TEST_CASE("an over-granted worker recovers to a sane task size", "[sizer]") {
    // The consequence that matters: after correcting, the task must actually
    // shrink enough to finish inside a lease.
    SizingInputs in;
    in.score = 2.51e10;        // units/s the browser CLAIMED
    in.target_ms = 2000;
    in.lease_ms = 20000;
    in.remaining_units = 5'000'000'000ULL;
    in.correction = 50.0;      // what it actually needs

    const std::uint64_t units = ComputeTaskSize(in);
    const double real_rate = 2.51e10 / 50.0;      // its true throughput
    const double seconds = static_cast<double>(units) / real_rate;
    CHECK(seconds < 20.0);     // fits the lease, which it did not before
}

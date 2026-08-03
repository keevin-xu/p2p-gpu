#pragma once
//
// Injectable misbehaviour — step 2.3. THE EXPERIMENTAL INSTRUMENTS.
//
// These are not test fixtures. E3 (fault tolerance), E4 (Byzantine), and E5
// (stragglers) are *measurements taken with these*, so their semantics need to
// be as clean as a lab instrument's: orthogonal, independently toggleable, and
// reproducible from a seed.
//
// ── ORTHOGONAL MEANS ORTHOGONAL ──────────────────────────────────────────
// Every behaviour below does exactly one thing, and enabling two must produce
// the union of their effects with no interaction. A `slow` worker that also
// happens to renew its lease late is TWO behaviours pretending to be one, and
// an experiment that turns on `slow` then cannot say which caused the result.
//
// ── SEEDED MEANS REPRODUCIBLE ────────────────────────────────────────────
// Every probabilistic decision draws from a per-worker RNG seeded from
// (run_seed, worker_index). Same seed, same fleet, same failures, same order.
// A chaos harness whose failures cannot be replayed produces anecdotes.

#include <cstdint>
#include <random>
#include <string>
#include <string_view>

namespace p2pgpu::mock {

/// What a virtual worker does wrong. All default to "behaves correctly".
struct Behaviors {
    /// Multiplies simulated task duration. 1.0 is nominal; 10.0 is a straggler.
    /// Deliberately a multiplier on the SIMULATED duration, not real CPU work —
    /// 200 slow workers must not need 200 busy cores (D-0042).
    double slow_factor = 1.0;

    /// Probability of disconnecting mid-task, per task. The R8 case: a worker
    /// vanishing is normal, and the coordinator must requeue without penalty.
    double dies_mid_task = 0.0;

    /// Probability of returning a well-formed result with WRONG numbers.
    /// Distinct from `lies_probabilistically` only in intent: this is corruption
    /// (a broken worker), that is deception (a malicious one). The coordinator
    /// cannot tell them apart from one sample, which is exactly the problem
    /// Phase 3 exists to solve — so the harness keeps them separable even
    /// though the wire does not.
    double returns_garbage = 0.0;

    /// Probability, per task, of deliberately reporting a plausible-but-false
    /// result. THE BYZANTINE INSTRUMENT (E4).
    double lies_probabilistically = 0.0;

    /// Added to every message this worker sends. Simulates a distant peer.
    std::uint32_t high_latency_ms = 0;

    /// Never sends Progress{request_renew}. The lease expires under the worker
    /// while it is still working — tests that expiry returns the task without
    /// penalising a worker that is merely slow (R8).
    bool never_renews_lease = false;

    /// Submits every result twice. Invariant 4 and step 2.10 must make the
    /// second a silent no-op rather than an error, which matters once
    /// speculation makes duplicates routine rather than pathological.
    bool duplicate_submit = false;

    /// Disconnects and reconnects repeatedly. Distinct from `dies_mid_task`:
    /// that worker is gone, this one keeps coming back with a new identity,
    /// which is what makes reputation hard.
    double flaps = 0.0;

    /// Sends deliberately malformed frames — step 2.5. THE HOSTILE PROFILE.
    /// Live-fire counterpart to the Phase 1 fuzzer: fuzzing proves the parser
    /// is safe in isolation, this proves the SERVER stays up under sustained
    /// abuse from a real socket.
    bool malformed_frames = false;

    [[nodiscard]] bool honest() const noexcept {
        return returns_garbage == 0.0 && lies_probabilistically == 0.0 &&
               !malformed_frames;
    }
};

/// Per-worker RNG. Seeded from (run_seed, worker_index) so a fleet replays
/// exactly: same workers misbehave, in the same order, on the same tasks.
class Dice {
public:
    Dice(std::uint64_t run_seed, std::uint32_t worker_index)
        : rng_(run_seed ^ (0x9E3779B97F4A7C15ULL * (worker_index + 1))) {}

    /// True with probability p. p <= 0 never fires and p >= 1 always does,
    /// without consuming randomness — so toggling an unused behaviour cannot
    /// shift every later draw and change an unrelated experiment.
    [[nodiscard]] bool chance(double p) {
        if (p <= 0.0) { return false; }
        if (p >= 1.0) { return true; }
        return std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < p;
    }

    /// Log-normal throughput multiplier. Real fleets are not uniform: a few
    /// fast machines, a long tail of slow ones. Default sigma gives roughly a
    /// 20x spread between the fastest and slowest of a hundred (step 2.2).
    [[nodiscard]] double lognormal(double sigma) {
        return std::lognormal_distribution<double>(0.0, sigma)(rng_);
    }

    [[nodiscard]] std::uint64_t next() { return rng_(); }

private:
    std::mt19937_64 rng_;
};

}  // namespace p2pgpu::mock

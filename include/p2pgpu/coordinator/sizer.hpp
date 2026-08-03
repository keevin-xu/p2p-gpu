#pragma once
//
// Adaptive task sizing — steps 2.12-2.14.
//
// ── THE WORKER ASKS WHETHER; THE COORDINATOR DECIDES HOW MUCH (R1) ───────
// A worker sends `LeaseRequest{max_tasks}` and nothing else. Everything about
// how much work it gets is computed here, from a benchmark score the worker
// reported once and a correction factor derived from what it has actually
// achieved since. A worker that could choose its own task size could take a
// tiny one and look fast, which is a reputation exploit dressed as a
// scheduling knob.
//
// ── WHY A CORRECTION FACTOR AT ALL ───────────────────────────────────────
// The join-time benchmark measures a synthetic kernel on an idle machine. Real
// tasks contend with whatever else the user is doing, and browsers pay ~45x more
// per submission than native (D-0026). The score is a starting point; the
// correction is what makes the second grant better than the first (2.13).

#include <cstdint>

namespace p2pgpu::coordinator {

/// Everything sizing depends on. Grouped so the calculation is a pure function
/// of its inputs and can be tested without a fleet, a job, or a clock.
struct SizingInputs {
    /// Normalized work-units/sec from the join-time benchmark (2.11).
    double score = 0.0;

    /// EWMA of (actual / predicted) duration for this worker (2.13). 1.0 means
    /// the benchmark has been accurate so far; >1 means tasks take longer than
    /// predicted, so grant less.
    double correction = 1.0;

    /// User-set throttle, 0.0-1.0 (R7). Scales the effective score, applied
    /// WITHOUT argument — the user's setting is authoritative and the
    /// coordinator does not get an opinion about it.
    double throttle = 1.0;

    /// How long we want a task to take. 1-3 s per ARCHITECTURE.md §7.
    std::uint32_t target_ms = 2000;

    /// The lease this task would be granted under. A task must FINISH inside
    /// its lease or it expires mid-work and the work is discarded.
    std::uint32_t lease_ms = 30000;

    /// The kernel's R5 floor (D-0029). Below this the kernel is transfer-bound
    /// and adding nodes cannot help, so the sizer must not clamp under it even
    /// for a very slow worker.
    std::uint64_t r5_min_units = 0;

    /// Units left in the job's keyspace. The last task takes whatever remains.
    std::uint64_t remaining_units = 0;
};

/// Compute a task size, in work units.
///
/// Rules, in the order they apply — the order is the design:
///
///   1. `target_ms x score x correction x throttle`
///   2. **clamp to the lease** — with headroom, using the same half-open
///      reading as the expiry sweep. A task sized to exactly fill its lease
///      expires just as it finishes.
///   3. **floor at `r5_min_units`**, which WINS over the lease clamp. A worker
///      too slow to finish that inside a lease cannot usefully contribute to
///      this kernel, and handing it a pointless sub-R5 task is worse than
///      granting nothing (D-0029, D-0043).
///   4. **cap at `remaining_units`** — so the final task of a job may legally be
///      smaller than the R5 floor. That is correct; the alternative is leaving a
///      remainder unsearched. R5 is a SIZING rule, not an invariant about every
///      task that exists.
///
/// Returns 0 when nothing should be granted — no keyspace left, or a worker
/// with no usable score.
[[nodiscard]] std::uint64_t ComputeTaskSize(const SizingInputs& in) noexcept;

/// Update a worker's correction factor from one completed task (2.13).
///
/// EWMA over the ratio of actual to predicted duration. Returns the new factor.
///
/// `alpha` is how fast it forgets. Low values converge slowly but survive a
/// single anomalous task; high values chase noise. 0.3 is a starting point, and
/// 2.26 plots convergence — if it does not converge, that is a finding worth a
/// DECISIONS entry rather than a quiet tweak.
[[nodiscard]] double UpdateCorrection(double current, double predicted_ms,
                                      double actual_ms, double alpha = 0.3) noexcept;

}  // namespace p2pgpu::coordinator

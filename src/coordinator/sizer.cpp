// Adaptive task sizing — steps 2.12-2.14. See sizer.hpp.

#include "p2pgpu/coordinator/sizer.hpp"

#include <algorithm>
#include <cmath>

namespace p2pgpu::coordinator {
namespace {

/// Fraction of the lease a task may be sized to fill.
///
/// A task sized to exactly fill its lease expires just as it finishes — the
/// sweep uses the half-open interval [grant, expires), so "finishes at the
/// deadline" is "expired". This leaves room for the submission round trip, one
/// renewal interval, and being wrong about the worker's speed.
constexpr double kLeaseHeadroom = 0.5;

}  // namespace

std::uint64_t ComputeTaskSize(const SizingInputs& in) noexcept {
    if (in.remaining_units == 0) {
        return 0;
    }
    // No usable score: the worker has not benchmarked, or reported nonsense.
    // Refuse rather than guess — a fabricated score would propagate into every
    // future grant through the correction factor.
    if (!(in.score > 0.0) || !std::isfinite(in.score)) {
        return 0;
    }

    const double correction = (in.correction > 0.0 && std::isfinite(in.correction))
                                  ? in.correction
                                  : 1.0;
    const double throttle = std::clamp(in.throttle, 0.0, 1.0);
    if (throttle == 0.0) {
        return 0;   // paused by the user (R7); not an error, just no work
    }

    // 0. THE PROBE (D-0050). Before a worker's first completed task the only
    //    thing known about it is a score it reported about itself, and E5
    //    measured what believing that costs: every task over 10 s was a
    //    worker's FIRST task, predicted 2000 ms against ~41000 ms actual.
    //    Divided here rather than clamped later so the lease clamp, the R5
    //    floor and the remaining cap all still apply in their fixed order.
    const double probe = in.cold_start ? static_cast<double>(kProbeDivisor) : 1.0;

    // 1. What this worker should manage in target_ms.
    const double seconds = static_cast<double>(in.target_ms) / 1000.0;
    double units = seconds * in.score * throttle / (correction * probe);

    // 2. Clamp to what fits in the lease, with headroom.
    const double lease_seconds =
        (static_cast<double>(in.lease_ms) / 1000.0) * kLeaseHeadroom;
    const double lease_cap = lease_seconds * in.score * throttle / (correction * probe);
    units = std::min(units, lease_cap);

    if (!(units >= 1.0) || !std::isfinite(units)) {
        units = 1.0;
    }
    auto result = static_cast<std::uint64_t>(units);

    // 3. R5 floor WINS over the lease clamp (D-0029). A worker too slow to
    //    finish this inside a lease cannot usefully contribute to this kernel,
    //    and a pointless sub-R5 task is worse than granting nothing — it costs
    //    a round trip and a lease to produce a transfer-bound result.
    result = std::max(result, in.r5_min_units);

    //    Also never more than the params block can describe (D-0063). Placed
    //    before the remaining-units cap so the final task of a job is still
    //    whatever remains, and after the R5 floor so a floor above the ceiling
    //    is impossible rather than merely unlikely.
    result = std::min(result, kMaxUnitsPerTask);

    // 5. ...but never more than is left. The final task of a job may therefore
    //    be smaller than the R5 floor, which is correct: the alternative is
    //    leaving a remainder unsearched. R5 is a sizing rule, not an invariant
    //    about every task that exists (D-0043).
    return std::min(result, in.remaining_units);
}

double UpdateCorrection(double current, double predicted_ms, double actual_ms,
                        double alpha) noexcept {
    // Guard the degenerate inputs rather than letting them poison the average.
    // A single zero or NaN here would persist through every future grant for
    // this worker, which is a much worse failure than ignoring one sample.
    if (!(predicted_ms > 0.0) || !(actual_ms > 0.0) ||
        !std::isfinite(predicted_ms) || !std::isfinite(actual_ms)) {
        return current;
    }
    const double ratio = actual_ms / predicted_ms;

    // Bounded. A worker that stalls for a minute on a 2 s task would otherwise
    // push the correction so high it is granted single-digit units forever —
    // one bad sample must not permanently exile a machine (R8's spirit: a
    // hiccup is not a verdict).
    // Ceiling 1000, not 10 (D-0064). A browser on a discrete GPU measured
    // needing MORE than 10x — the benchmark overstates it ~45x (D-0026) and the
    // 1650 Super's per-chunk cost is 11.5x the Mac's (0.16) — so at 10x the
    // estimator could not express reality, every task expired, and the worker
    // never completed one to learn from.
    const double clamped = std::clamp(ratio, kMinCorrection, kMaxCorrection);
    const double a = std::clamp(alpha, 0.0, 1.0);
    const double next = (1.0 - a) * current + a * clamped;
    return std::clamp(next, kMinCorrection, kMaxCorrection);
}

}  // namespace p2pgpu::coordinator

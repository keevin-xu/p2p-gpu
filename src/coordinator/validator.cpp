// coordinator/validator — steps 3.1-3.3. See validator.hpp.
// The coordinator is the ONLY component that makes decisions (rule R1).
// No unwrap-equivalent: never crash on worker input (docs/CONVENTIONS.md §1).

#include "p2pgpu/coordinator/validator.hpp"
#include <vector>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>

namespace p2pgpu::coordinator {
namespace {

/// Distance in units in the last place between two float32s.
///
/// For IEEE-754, the bit patterns of same-signed finites are ordered exactly as
/// the values are, so subtracting them counts representable steps. `bit_cast`,
/// never a pointer cast (R11, and the spelling D-0027 settled on).
///
/// Mapped through a signed-magnitude ordering rather than handling only equal
/// signs: +0.0 and -0.0 are 0 ULP apart and must compare equal, which a
/// same-sign-only version gets wrong on any buffer that legitimately contains
/// signed zeros.
std::uint32_t UlpDistance(float a, float b) {
    if (a == b) {
        return 0;   // also catches +0.0 vs -0.0
    }
    if (!std::isfinite(a) || !std::isfinite(b)) {
        // NaN or inf against anything else is not "some number of steps away".
        // Saturate rather than invent a distance.
        return std::numeric_limits<std::uint32_t>::max();
    }

    const auto to_ordered = [](float v) -> std::int64_t {
        const auto bits = std::bit_cast<std::int32_t>(v);
        return bits >= 0 ? static_cast<std::int64_t>(bits)
                         : static_cast<std::int64_t>(0x80000000LL) -
                               static_cast<std::int64_t>(bits);
    };

    const std::int64_t diff = to_ordered(a) - to_ordered(b);
    const std::uint64_t mag = static_cast<std::uint64_t>(diff < 0 ? -diff : diff);
    return mag > std::numeric_limits<std::uint32_t>::max()
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(mag);
}

/// Read one float32 out of a byte span without a cast (R11).
float FloatAt(std::span<const std::byte> bytes, std::size_t index) {
    std::array<std::byte, sizeof(float)> raw{};
    for (std::size_t k = 0; k < sizeof(float); ++k) {
        raw[k] = bytes[index * sizeof(float) + k];
    }
    return std::bit_cast<float>(raw);
}

Comparison CompareExact(std::span<const std::byte> a, std::span<const std::byte> b) {
    Comparison c;
    c.elements = a.size();
    c.first_differing_index = a.size();

    // Span indexing, not memcmp (R11) — both buffers came off the wire. Also
    // counts ALL differing bytes rather than stopping at the first, because
    // "one byte off" and "entirely different" are different evidence for 3.7.
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            ++c.differing_elements;
            c.first_differing_index = std::min(c.first_differing_index, i);
        }
    }
    // Bitwise or nothing — and 0.16 validated that is sound: `smoke_hash` was
    // 1000/1000 identical across Apple Metal and NVIDIA D3D12, so `Exact` is a
    // measured property rather than an assumption.
    c.verdict = c.differing_elements == 0 ? Verdict::Match : Verdict::Mismatch;
    return c;
}

Comparison CompareTolerant(const KernelSpec& spec, std::span<const std::byte> a,
                           std::span<const std::byte> b) {
    Comparison c;

    // float32 is ASSUMED — no kernel declares an output element type yet
    // (D-0053). Reject rather than read a partial element: silently ignoring
    // trailing bytes an attacker chose is how a payload gets to hide in the
    // remainder.
    if (a.size() % sizeof(float) != 0) {
        c.verdict = Verdict::Unsupported;
        c.elements = a.size();
        c.detail = "Tolerant payload is not a multiple of 4 bytes (assumes f32)";
        return c;
    }

    const std::size_t n = a.size() / sizeof(float);
    c.elements = n;
    c.first_differing_index = n;

    for (std::size_t i = 0; i < n; ++i) {
        const float va = FloatAt(a, i);
        const float vb = FloatAt(b, i);

        // NaN fails every comparison including its own, so an unguarded
        // `abs_diff > allowed` reads NaN as "within tolerance" — silently
        // accepting the one output most likely to mean the kernel broke.
        const bool a_nan = std::isnan(va);
        const bool b_nan = std::isnan(vb);
        if (a_nan && b_nan) {
            continue;   // both NaN: agreeing, in the only sense available
        }
        if (a_nan != b_nan) {
            ++c.differing_elements;
            c.first_differing_index = std::min(c.first_differing_index, i);
            c.max_ulp_diff = std::numeric_limits<std::uint32_t>::max();
            c.max_abs_diff = std::numeric_limits<double>::infinity();
            continue;
        }

        const double abs_diff =
            std::fabs(static_cast<double>(va) - static_cast<double>(vb));
        const double denom = std::fabs(static_cast<double>(vb));
        const double rel_diff = denom > 0.0 ? abs_diff / denom : 0.0;

        c.max_abs_diff = std::max(c.max_abs_diff, abs_diff);
        c.max_rel_diff = std::max(c.max_rel_diff, rel_diff);
        c.max_ulp_diff = std::max(c.max_ulp_diff, UlpDistance(va, vb));

        // The 3.1 form: |a-b| <= abs_eps + rel_eps*|b|. `abs_eps` carries
        // values near zero, where relative error is meaningless — a correct
        // result can be 1e-30 against 0.0.
        const double allowed = static_cast<double>(spec.abs_eps) +
                               static_cast<double>(spec.rel_eps) * denom;
        if (abs_diff > allowed) {
            ++c.differing_elements;
            c.first_differing_index = std::min(c.first_differing_index, i);
        }
    }

    c.verdict = c.differing_elements == 0 ? Verdict::Match : Verdict::Mismatch;
    return c;
}

}  // namespace

// ── Statistical (5.14, D-0075) ───────────────────────────────────────────
//
// The payload contract for this class is an array of `{r,g,b,samples}` f32
// quadruples — the accumulator format from D-0074, a running SUM plus the count
// that produced it. Sample counts must be ON the payload: the estimator's
// precision depends on them, and a comparator without them is guessing.

namespace {

/// Per-channel outlier cutoff, in robust standard deviations.
constexpr double kSigmaCutoff = 6.0;
/// Tile fails if more than this fraction of channels are outliers. Non-zero
/// because a handful of genuine tail samples in a Monte Carlo image is normal —
/// fireflies are not fraud.
constexpr double kMaxOutlierFraction = 0.005;
/// Aggregate cutoff, deliberately stricter in sigma terms: averaging over
/// thousands of pixels shrinks the noise floor, so a global bias that survives
/// it is not noise.
constexpr double kAggregateCutoff = 8.0;
/// Floor on the robust scale, relative to the typical pixel magnitude.
///
/// WITHOUT THIS THE COMPARATOR REJECTS PERFECT AGREEMENT. Identical inputs give
/// a median |d| of ~0, so the scale is ~0 and every pixel exceeds `6 * 0`.
/// D-0073 measured honest identical-range disagreement at ~1 ULP, far under
/// this floor, and any real disagreement is far over it.
constexpr double kScaleFloorRel = 1e-3;
/// 1/Phi^-1(3/4): makes the MAD a consistent estimator of sigma for a normal.
constexpr double kMadToSigma = 1.4826;

double Median(std::vector<double>& v) {
    if (v.empty()) {
        return 0.0;
    }
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    return v[mid];
}

Comparison CompareStatistical(std::span<const std::byte> a,
                              std::span<const std::byte> b) {
    Comparison c;
    constexpr std::size_t kStride = 16;   // {r,g,b,samples} f32

    if (a.size() % kStride != 0) {
        c.verdict = Verdict::Unsupported;
        c.detail = "statistical payload is not a whole number of RGBA f32 pixels";
        return c;
    }
    const std::size_t pixels = a.size() / kStride;
    c.elements = pixels * 3;   // channels compared; samples is not a measurement

    const auto pixel_at = [](std::span<const std::byte> buf, std::size_t i) {
        std::array<float, 4> out{};
        for (std::size_t k = 0; k < 4; ++k) {
            std::array<std::byte, 4> raw{};
            for (std::size_t j = 0; j < 4; ++j) {
                raw[j] = buf[i * kStride + k * 4 + j];
            }
            out[k] = std::bit_cast<float>(raw);
        }
        return out;
    };

    std::vector<double> d;
    d.reserve(pixels * 3);
    std::vector<double> magnitude;
    magnitude.reserve(pixels * 3);
    double sum_a = 0.0;
    double sum_b = 0.0;
    double sum_pooled_sq = 0.0;

    for (std::size_t i = 0; i < pixels; ++i) {
        const auto pa = pixel_at(a, i);
        const auto pb = pixel_at(b, i);
        const double na = pa[3];
        const double nb = pb[3];
        if (!(na > 0.0) || !(nb > 0.0) || !std::isfinite(na) || !std::isfinite(nb)) {
            // No sample count means no estimate of precision, so there is no
            // honest comparison to make. Unsupported, NOT Match — a comparator
            // that passes what it cannot judge is the stub D-0047 refused.
            c.verdict = Verdict::Unsupported;
            c.detail = "a pixel reports a non-positive or non-finite sample count";
            return c;
        }
        // Variance of the difference of two independent means.
        const double pooled = std::sqrt(1.0 / na + 1.0 / nb);
        sum_pooled_sq += pooled * pooled;

        for (std::size_t k = 0; k < 3; ++k) {
            if (!std::isfinite(pa[k]) || !std::isfinite(pb[k])) {
                c.verdict = Verdict::Mismatch;
                c.differing_elements = 1;
                c.first_differing_index = i * 3 + k;
                c.detail = "non-finite radiance";
                return c;
            }
            const double ma = pa[k] / na;
            const double mb = pb[k] / nb;
            d.push_back((ma - mb) / pooled);
            magnitude.push_back(std::abs(mb));
            sum_a += ma;
            sum_b += mb;
        }
    }
    if (d.empty()) {
        c.verdict = Verdict::Match;
        return c;
    }

    std::vector<double> abs_d;
    abs_d.reserve(d.size());
    for (const double v : d) {
        abs_d.push_back(std::abs(v));
    }
    std::vector<double> mag_copy = magnitude;
    const double typical = Median(mag_copy);
    // MAD about zero: the difference of two unbiased estimators is centred on
    // zero by construction, so subtracting a sample median would only absorb a
    // genuine global bias into the scale — precisely what the aggregate test
    // below exists to catch.
    const double scale =
        std::max(kMadToSigma * Median(abs_d), kScaleFloorRel * std::max(typical, 1e-9));

    std::size_t outliers = 0;
    double worst = 0.0;
    std::size_t worst_index = d.size();
    for (std::size_t i = 0; i < d.size(); ++i) {
        const double z = std::abs(d[i]) / scale;
        if (z > worst) {
            worst = z;
            worst_index = i;
        }
        if (z > kSigmaCutoff) {
            ++outliers;
        }
    }
    c.differing_elements = outliers;
    c.first_differing_index = worst_index;
    c.max_abs_diff = worst;   // in robust sigmas, which is the scale-free number

    // Aggregate: a MAD test is BLIND to a uniform error, because scaling every
    // pixel scales the estimator too. Averaging over thousands of pixels shrinks
    // the noise floor, so a global bias that survives it is not noise.
    const double mean_diff = (sum_a - sum_b) / static_cast<double>(d.size());
    const double aggregate_se =
        scale * std::sqrt(sum_pooled_sq / static_cast<double>(d.size())) /
        std::sqrt(static_cast<double>(d.size()));
    const double z_aggregate =
        aggregate_se > 0.0 ? std::abs(mean_diff) / aggregate_se : 0.0;
    c.max_rel_diff = z_aggregate;

    const double outlier_fraction =
        static_cast<double>(outliers) / static_cast<double>(d.size());
    if (outlier_fraction > kMaxOutlierFraction) {
        c.verdict = Verdict::Mismatch;
        c.detail = "too many per-pixel outliers";
        return c;
    }
    if (z_aggregate > kAggregateCutoff) {
        c.verdict = Verdict::Mismatch;
        c.detail = "tile means disagree beyond sampling error";
        return c;
    }
    c.verdict = Verdict::Match;
    return c;
}

}  // namespace

Comparison Compare(const KernelSpec& spec, std::span<const std::byte> a,
                   std::span<const std::byte> b) {
    Comparison c;

    // Length first, for every class. Two results of different sizes are not
    // "close" under any determinism class, and every loop below indexes both
    // buffers — one check here is what makes that safe, rather than the same
    // bounds question asked three times (R11).
    if (a.size() != b.size()) {
        c.verdict = Verdict::Mismatch;
        c.elements = std::max(a.size(), b.size());
        c.differing_elements = c.elements;
        c.detail = "payload lengths differ: " + std::to_string(a.size()) + " vs " +
                   std::to_string(b.size());
        return c;
    }
    if (a.empty()) {
        // Nothing to disagree about. Whether a zero-length output is legal is
        // the registry's business, not the comparator's.
        c.verdict = Verdict::Match;
        return c;
    }

    // No `default:` arm — adding a determinism class must break this build
    // (ARCHITECTURE.md §5).
    switch (spec.determinism) {
        case Determinism::Exact:
            return CompareExact(a, b);
        case Determinism::Tolerant:
            return CompareTolerant(spec, a, b);
        case Determinism::Statistical:
            return CompareStatistical(a, b);
    }

    c.verdict = Verdict::Unsupported;
    c.detail = "unknown determinism class";
    return c;
}

std::string Describe(const Comparison& c) {
    std::array<char, 256> buf{};
    const int n = std::snprintf(
        buf.data(), buf.size(),
        "verdict=%s elements=%zu differing=%zu first_idx=%zu "
        "max_abs=%.6g max_rel=%.6g max_ulp=%u",
        ToString(c.verdict), c.elements, c.differing_elements,
        c.first_differing_index, c.max_abs_diff, c.max_rel_diff, c.max_ulp_diff);
    std::string out =
        n > 0 ? std::string(buf.data(), static_cast<std::size_t>(n)) : std::string{"verdict=?"};
    if (!c.detail.empty()) {
        out += " detail=\"" + c.detail + "\"";
    }
    return out;
}

}  // namespace p2pgpu::coordinator

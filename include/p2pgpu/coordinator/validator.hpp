#pragma once
//
// Result comparison — steps 3.1-3.3.
//
// ── THE COMPARATOR RETURNS A DISTANCE, NOT A VERDICT (D-0053) ────────────
// "Off by 2 ULP" and "completely different numbers" are the same `false`, and
// 3.7's reputation must treat them as opposites: the first is two honest GPUs
// disagreeing exactly as R6 says they will, the second is a liar or a broken
// card. A boolean throws that away at the one place it is still knowable.
//
// ── WHY THIS IS HARD AT ALL ──────────────────────────────────────────────
// R6: honest workers on different vendors DO disagree. Measured, not assumed —
// 0.16 found 63.3% of transcendental results differing across Apple Metal and
// NVIDIA D3D12, by up to 5 ULP. A validator comparing those bitwise would
// blacklist honest NVIDIA workers on nearly every task. So comparison is
// dispatched on the kernel's declared `Determinism`, and bitwise equality is
// correct ONLY for `Exact`.
//
// ── EVERY BYTE HERE IS ATTACKER-CONTROLLED (R11) ─────────────────────────
// Both payloads came off the wire. No `memcmp`, no pointer arithmetic, no
// casts: `std::span` throughout, and `std::bit_cast` for the float view.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "p2pgpu/coordinator/kernel_registry.hpp"

namespace p2pgpu::coordinator {

enum class Verdict : std::uint8_t {
    Match,
    Mismatch,
    /// The declared determinism class has no comparator yet, or the payloads
    /// are malformed for it.
    ///
    /// A THIRD verdict rather than folding into Match, because a stub that
    /// silently passes everything is indistinguishable from validation that
    /// works — the exact failure this project has already shipped twice
    /// (`never_renews_lease`, and the stub D-0047 refused to write).
    Unsupported,
};

[[nodiscard]] constexpr const char* ToString(Verdict v) noexcept {
    switch (v) {
        case Verdict::Match:       return "Match";
        case Verdict::Mismatch:    return "Mismatch";
        case Verdict::Unsupported: return "Unsupported";
    }
    return "?";
}

/// How far apart two results were — the input to reputation (3.7), not just a
/// pass/fail.
struct Comparison {
    Verdict verdict = Verdict::Unsupported;

    /// Elements compared, in the units the comparator used (bytes for `Exact`,
    /// float32 for `Tolerant`).
    std::size_t elements = 0;
    std::size_t differing_elements = 0;
    /// Index of the first difference, for the 3.3 log line. `elements` when
    /// nothing differed.
    std::size_t first_differing_index = 0;

    /// Populated for `Tolerant`. Zero for `Exact`, which has no notion of
    /// "close".
    double max_abs_diff = 0.0;
    double max_rel_diff = 0.0;

    /// Max difference in units in the last place — the SCALE-FREE number, and
    /// the unit 0.16 measured in. `1e-7` means nothing without knowing whether
    /// the values are near 1 or near 1e9; "3 ULP" means the same everywhere.
    std::uint32_t max_ulp_diff = 0;

    /// Why, when the verdict is `Unsupported`. Empty otherwise.
    std::string detail;

    [[nodiscard]] bool matched() const noexcept { return verdict == Verdict::Match; }
};

/// Compare two results under a kernel's declared determinism class.
///
/// `a` is typically the newer submission and `b` the incumbent; the relative
/// error is computed against `b`, matching the `|a-b| <= abs_eps + rel_eps*|b|`
/// form in 3.1.
///
/// Never throws and never trusts either input: differing lengths are a
/// `Mismatch`, and a `Tolerant` payload that is not a whole number of float32s
/// is `Unsupported` rather than a partial read.
[[nodiscard]] Comparison Compare(const KernelSpec& spec,
                                 std::span<const std::byte> a,
                                 std::span<const std::byte> b);

/// One-line summary for the 3.3 rejection log.
///
/// Rejections are logged at `warn` WITH the numbers, because a misdeclared
/// determinism class presents as a flood of mismatches from honest workers
/// (`RISKS.md` §2) and the deviation is what tells the two apart at a glance:
/// a few ULP means the class is wrong, orders of magnitude means the worker is.
[[nodiscard]] std::string Describe(const Comparison& c);

}  // namespace p2pgpu::coordinator

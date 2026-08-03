#pragma once
//
// CPU reference for `brute_search_v1` — step 1.25. GROUND TRUTH.
//
// Everything about the GPU path so far has been checked for SELF-consistency:
// `found_count` agrees with `match_xor`, chunking does not change the bytes,
// the pipeline delivers them intact. All of that would hold equally well if the
// kernel computed the wrong thing. This file is what closes that gap — from
// here on, "the kernel is correct" means "it agrees with this".
//
// ── HOW INDEPENDENT IS THIS, HONESTLY ────────────────────────────────────
// An independent reimplementation is only evidence if it is actually
// independent. A line-by-line transliteration of the WGSL would copy any
// misunderstanding into both and agree perfectly while both were wrong.
//
// So the split is deliberate:
//
//   - The SEARCH and the three REDUCTIONS are written from the specification
//     (D-0029, kernels/manifest.toml), not from the shader. What they check is
//     the host<->kernel plumbing: params layout, the chunk window, dispatch
//     sizing, bounds handling, readback, and the wire path.
//
//   - `PcgHash` is BIT-IDENTICAL to the WGSL by necessity. It is a specified
//     primitive, not a design choice — two different hashes would simply never
//     agree, and the test would report a failure with nothing behind it.
//
// So this proves the pipeline computes the intended search correctly. It does
// NOT independently verify pcg_hash; that is what step 0.9's cross-vendor
// comparison did (naga, Tint and WebKit produce identical output).
//
// ── WHY IT LIVES HERE ────────────────────────────────────────────────────
// Alongside params.hpp and outside both `worker-core` and `coordinator`, for
// the same reason: the tests need it, and Phase 3's validator may want it for
// spot-checking. Header-only and dependency-free so neither has to link
// anything to use it.

#include <cstdint>

#include "p2pgpu/kernels/params.hpp"

namespace p2pgpu::kernels {

/// Bit-identical to `pcg_hash` in kernels/smoke_hash.wgsl and
/// kernels/brute_search.wgsl.
///
/// WGSL `u32` arithmetic wraps on overflow, and so does `std::uint32_t` — the
/// two agree here without any explicit masking. That is a language guarantee on
/// both sides, not a happy accident, but it is the kind of thing worth saying
/// out loud because a `std::uint64_t` intermediate would silently break it.
[[nodiscard]] constexpr std::uint32_t PcgHash(std::uint32_t v) noexcept {
    const std::uint32_t state = v * 747796405U + 2891336453U;
    const std::uint32_t word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return (word >> 22U) ^ word;
}

/// Run the search on the CPU, single-threaded, over the range the params
/// describe.
///
/// Takes the SAME `BruteSearchParams` the GPU is handed, so a test can feed one
/// struct to both and compare the results byte for byte. `start_lo` and
/// `unit_count` are read as given — this is the whole range, with no chunking,
/// which is exactly what makes it a useful check on the host's chunk loop.
///
/// Returns a `BruteSearchResult` initialised the way the host initialises the
/// GPU's buffer: zeros, with `min_match` at the `atomicMin` identity.
[[nodiscard]] inline BruteSearchResult BruteSearchReference(
    const BruteSearchParams& p) noexcept {
    BruteSearchResult out{};  // found_count = 0, min_match = 0xFFFFFFFF

    for (std::uint32_t i = 0; i < p.unit_count; ++i) {
        // The candidate's low half. Wrapping is correct and intended: the
        // kernel computes `params.start_lo + i` in u32 and wraps identically,
        // so a range straddling 2^32 must behave the same on both sides.
        const std::uint32_t lo = p.start_lo + i;

        // Fold both halves and the seed into one 32-bit state. Without the
        // high half the search would be identical for every task sharing a
        // start_lo window (D-0029).
        std::uint32_t h = lo ^ p.seed ^ (p.base_hi * 2654435769U);
        for (std::uint32_t r = 0; r < p.rounds; ++r) {
            h = PcgHash(h);
        }

        if ((h & p.mask) == p.target_bits) {
            // All three reductions operate on the CANDIDATE, never on h. A
            // candidate is visited exactly once, so the xor cannot self-cancel;
            // two candidates colliding to the same h would annihilate each
            // other's contribution and silently under-report the match set.
            ++out.found_count;
            if (lo < out.min_match) {
                out.min_match = lo;
            }
            out.match_xor ^= lo;
        }
    }

    return out;
}

}  // namespace p2pgpu::kernels

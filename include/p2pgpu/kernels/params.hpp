#pragma once
//
// C++ mirrors of the WGSL params structs — step 1.9.
//
// Deliberately dependency-free and outside both `worker-core` and
// `coordinator`: the COORDINATOR builds these (it owns sizing, R1) and the
// WORKER uploads them, so neither can own the definition.
//
// ── WHY THE static_asserts MATTER ────────────────────────────────────────
// A WGSL params struct and its C++ counterpart must match field for field, in
// order. Nothing enforces that at runtime — a mismatch produces *garbage
// results that look exactly like a kernel bug*, and you will go looking in the
// shader. `BruteSearchParams` is eight consecutive `u32`s, so a reordering is
// both easy to introduce and invisible to review.
//
// Every offset is asserted, not just `sizeof`. Checking size alone would pass
// happily if two fields were swapped — which is the likely mistake, not a
// changed length. See CONVENTIONS.md §5.

#include <cstddef>
#include <cstdint>

namespace p2pgpu::kernels {

/// Mirrors `struct BruteSearchParams` in kernels/brute_search.wgsl.
///
/// WGSL lays out a uniform struct of `u32` as tightly packed 4-byte fields, and
/// the whole struct is 16-byte aligned for a uniform buffer — 32 bytes here, so
/// no tail padding is needed.
struct BruteSearchParams {
    /// THE CHUNK WINDOW. Must be the first two fields (K1 / D-0033): the kernel
    /// host rewrites bytes 0..7 before every dispatch and knows nothing else
    /// about this struct.
    std::uint32_t start_lo = 0;     ///< first candidate in THIS chunk
    std::uint32_t unit_count = 0;   ///< candidates in THIS chunk
    std::uint32_t base_hi = 0;      ///< high half of the 64-bit keyspace cursor
    std::uint32_t seed = 0;
    /// NOT named `target`: that is a RESERVED KEYWORD in WGSL and the shader
    /// fails to parse. Caught by a compile check, not by review.
    std::uint32_t target_bits = 0;  ///< match iff (H(x) & mask) == target_bits
    std::uint32_t mask = 0;         ///< difficulty; ~1-10 matches per task
    std::uint32_t rounds = 8;       ///< pcg_hash iterations; manifest default
    std::uint32_t _pad = 0;         ///< keeps the struct at 32 bytes
};

static_assert(sizeof(BruteSearchParams) == 32, "must match WGSL BruteSearchParams");
static_assert(alignof(BruteSearchParams) == 4);
// Field-by-field. A swapped pair keeps sizeof correct and silently searches the
// wrong keyspace, so the size check alone is not enough.
// The first two are not merely documented — they are what D-0033 relies on, so
// this pair may never be relaxed to a bare sizeof check.
static_assert(offsetof(BruteSearchParams, start_lo) == 0,
              "chunk window must be at byte 0 (K1 / D-0033)");
static_assert(offsetof(BruteSearchParams, unit_count) == 4,
              "chunk window must be at byte 4 (K1 / D-0033)");
static_assert(offsetof(BruteSearchParams, base_hi) == 8);
static_assert(offsetof(BruteSearchParams, seed) == 12);
static_assert(offsetof(BruteSearchParams, target_bits) == 16);
static_assert(offsetof(BruteSearchParams, mask) == 20);
static_assert(offsetof(BruteSearchParams, rounds) == 24);
static_assert(offsetof(BruteSearchParams, _pad) == 28);

/// Mirrors `struct BruteSearchResult` in kernels/brute_search.wgsl.
///
/// The atomics are plain `u32` on the host side — atomicity is a property of
/// how the GPU accesses them, not of how we read the finished buffer.
///
/// **The host must initialise this once per TASK, not per chunk** (step 4.2's
/// chunk-invariance test): zeros throughout, except `min_match = 0xFFFFFFFF` so
/// the first `atomicMin` wins. Re-initialising per chunk is the "state leaks
/// between chunks" bug and would present as scheduling nondeterminism.
struct BruteSearchResult {
    std::uint32_t found_count = 0;
    std::uint32_t min_match = 0xFFFFFFFFU;   ///< identity for atomicMin
    std::uint32_t match_xor = 0;
    std::uint32_t reserved[5] = {};          ///< zeroed; never written by the kernel

    /// No match found. `min_match` keeps its identity value and nothing was
    /// counted — checked together so a real match at lo == 0xFFFFFFFF is not
    /// mistaken for "empty".
    [[nodiscard]] constexpr bool empty() const noexcept { return found_count == 0; }
};

static_assert(sizeof(BruteSearchResult) == 32,
              "output size is LOCKED by the R5 calculation in D-0029(d)");
static_assert(offsetof(BruteSearchResult, found_count) == 0);
static_assert(offsetof(BruteSearchResult, min_match) == 4);
static_assert(offsetof(BruteSearchResult, match_xor) == 8);
static_assert(offsetof(BruteSearchResult, reserved) == 12);

/// Below this, the kernel violates R5 (D-0029(b)). The sizer must not clamp
/// under it even for a very slow worker — at ~32 µs of work it never binds in
/// practice, but it is a floor, not a suggestion.
inline constexpr std::uint32_t kBruteSearchMinUnits = 400000;

/// Mirrors `struct Params` in kernels/calibrate.wgsl (step 0.10, D-0018).
/// Duplicated from bench.hpp's private copy so both targets share one
/// definition; bench.hpp keeps its own static_asserts as a cross-check.
struct CalibrateParams {
    std::uint32_t iterations = 2048;
    std::uint32_t seed = 0;
};

static_assert(sizeof(CalibrateParams) == 8);
static_assert(offsetof(CalibrateParams, iterations) == 0);
static_assert(offsetof(CalibrateParams, seed) == 4);

}  // namespace p2pgpu::kernels

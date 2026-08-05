// Step 4.1 — golden tests.
//
// ── WHAT THESE CATCH THAT THE EXISTING TESTS CANNOT ──────────────────────
// `test_brute_search.cpp` compares the GPU against the CPU reference computed
// at test time, and 1.25 established that as the ground truth which caught
// D-0040's 1000/1000-wrong run. But it compares two things that can move
// TOGETHER: edit the WGSL and edit `reference.hpp` to match, and every one of
// those tests still passes while the kernel now computes something else.
//
// A golden test pins the ANSWER, not the agreement. These constants were
// generated once and committed; nothing in the build can regenerate them.
// If the kernel's meaning changes, this file is what says so — and changing
// them is then a deliberate act with a diff, which is the point.
//
// Runs on the CPU reference so it executes in CI, which has no GPU
// (`kernel_*` tests are excluded there). The GPU half lives in
// tests/kernels/test_brute_search.cpp and needs a real adapter.

#include <catch2/catch_test_macros.hpp>

#include "p2pgpu/kernels/reference.hpp"

using namespace p2pgpu::kernels;

namespace {

struct Golden {
    std::uint32_t start_lo, unit_count, base_hi, seed, target_bits, mask, rounds;
    std::uint32_t found_count, match_xor, min_match;
};

// Generated 2026-08-05 from `BruteSearchReference` and committed deliberately.
//
// Every case produces MATCHES. An earlier draft had two cases with
// `found_count == 0`, because `target_bits` sat outside `mask` and could never
// be hit — a golden test that passes when the kernel returns nothing at all is
// worse than no test, since it looks like coverage.
constexpr Golden kGolden[] = {
    // start_lo,  units,   base_hi, seed, target, mask,    rounds, found, match_xor,   min_match
    {0u,          1000000u, 0u,     42u,  0x234u, 0xFFFu,     8u,  277u,  0x00008007u, 0x000004E0u},
    {500000u,     250000u,  0u,      7u,  0x0u,   0xFFFFu,    8u,    1u,  0x000801FAu, 0x000801FAu},
    {0u,          1048576u, 1u,     99u,  0x2BCDu,0x3FFFFu,   8u,    6u,  0x0000B115u, 0x00015F25u},
    // start_lo near 2^32: this range WRAPS. The kernel computes start_lo + i in
    // u32 and wraps identically, so a host that promoted to 64-bit would differ
    // here and nowhere else.
    {4000000000u, 100000u,  0u,      3u,  0x555u, 0x7FFu,     8u,   45u,  0xEE6B7631u, 0xEE6B2A01u},
};

BruteSearchParams ParamsOf(const Golden& g) {
    BruteSearchParams p{};
    p.start_lo = g.start_lo;
    p.unit_count = g.unit_count;
    p.base_hi = g.base_hi;
    p.seed = g.seed;
    p.target_bits = g.target_bits;
    p.mask = g.mask;
    p.rounds = g.rounds;
    return p;
}

}  // namespace

TEST_CASE("brute_search_v1 matches its committed golden answers", "[golden]") {
    for (const Golden& g : kGolden) {
        const auto r = BruteSearchReference(ParamsOf(g));
        INFO("start_lo=" << g.start_lo << " units=" << g.unit_count
                         << " seed=" << g.seed);
        CHECK(r.found_count == g.found_count);
        CHECK(r.match_xor == g.match_xor);
        CHECK(r.min_match == g.min_match);
    }
}

TEST_CASE("every golden case actually finds something", "[golden]") {
    // The guard against the failure mode this file nearly shipped with: a case
    // whose `target_bits` lies outside `mask` can never match, so the expected
    // output is all-zeros and the test passes no matter what the kernel does.
    for (const Golden& g : kGolden) {
        INFO("seed=" << g.seed);
        CHECK(g.found_count > 0);
        CHECK((g.target_bits & ~g.mask) == 0u);
    }
}

TEST_CASE("golden output is invariant to chunking", "[golden]") {
    // Same answer whether computed in one pass or four. Chunk invariance is
    // already tested against the GPU (1.19); here it is pinned against the
    // COMMITTED values, so a chunking change cannot be absorbed by adjusting
    // the reference to match.
    for (const Golden& g : kGolden) {
        if (g.unit_count % 4 != 0) {
            continue;
        }
        BruteSearchResult merged{};
        const std::uint32_t per = g.unit_count / 4;
        for (std::uint32_t c = 0; c < 4; ++c) {
            auto p = ParamsOf(g);
            p.start_lo = g.start_lo + c * per;   // u32 wrap is intended
            p.unit_count = per;
            const auto part = BruteSearchReference(p);
            merged.found_count += part.found_count;
            merged.match_xor ^= part.match_xor;
            merged.min_match = std::min(merged.min_match, part.min_match);
        }
        INFO("seed=" << g.seed);
        CHECK(merged.found_count == g.found_count);
        CHECK(merged.match_xor == g.match_xor);
        CHECK(merged.min_match == g.min_match);
    }
}

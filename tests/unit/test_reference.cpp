// The CPU reference for brute_search_v1 — step 1.25. NO GPU NEEDED.
//
// Lives in the T1 tier so it runs in CI, where there is no adapter. The
// GPU-vs-reference comparison is necessarily in kernel_tests; these are the
// properties of the reference itself, which are checkable anywhere.
//
// Why that split matters: if the reference is wrong, the GPU comparison agrees
// with it and reports success. Something has to check the reference on its own
// terms, and CI is the only place that runs on every commit.

#include <catch2/catch_test_macros.hpp>

#include "p2pgpu/kernels/reference.hpp"

using namespace p2pgpu::kernels;

TEST_CASE("pcg_hash values are pinned against drift", "[reference]") {
    // BE PRECISE ABOUT WHAT THIS PROVES. These are values produced by the C++
    // copy, so pinning them here cannot show the C++ agrees with the WGSL —
    // that is established by "GPU matches the CPU reference exactly" in
    // kernel_tests, which needs a real adapter and therefore cannot run in CI.
    //
    // What this DOES catch is drift: a mistyped constant or shift in either
    // copy, on every commit, on a machine with no GPU. Given the reference is
    // the ground truth everything else is measured against, a silent edit here
    // would make every downstream comparison agree on the wrong answer.
    CHECK(PcgHash(0u) == 129708002u);
    CHECK(PcgHash(1u) == 2831084092u);
    CHECK(PcgHash(2u) == 2055130248u);
    CHECK(PcgHash(42u) == 1223963391u);
    CHECK(PcgHash(4294967295u) == 3861530882u);
}

TEST_CASE("the reference respects its range", "[reference]") {
    BruteSearchParams p{};
    p.start_lo = 1000;
    p.unit_count = 4096;
    p.mask = 0x000000FF;
    p.target_bits = 0x00000042;
    p.rounds = 8;

    const auto r = BruteSearchReference(p);
    REQUIRE(r.found_count > 0);
    // Every match must lie inside [start_lo, start_lo + unit_count).
    CHECK(r.min_match >= p.start_lo);
    CHECK(r.min_match < p.start_lo + p.unit_count);
}

TEST_CASE("an empty range finds nothing and keeps the identity", "[reference]") {
    BruteSearchParams p{};
    p.unit_count = 0;
    p.mask = 0x000000FF;
    p.rounds = 8;

    const auto r = BruteSearchReference(p);
    CHECK(r.found_count == 0);
    CHECK(r.match_xor == 0);
    // min_match keeps atomicMin's identity, which is what the host uploads and
    // what `empty()` relies on. A zero here would read as "matched candidate 0".
    CHECK(r.min_match == 0xFFFFFFFFu);
    CHECK(r.empty());
}

TEST_CASE("a mask of zero matches everything", "[reference]") {
    // (h & 0) == 0 for every h, so target_bits 0 matches every candidate. A
    // degenerate case, and exactly the one where an off-by-one in the loop
    // bound shows up as a count that is not unit_count.
    BruteSearchParams p{};
    p.unit_count = 1000;
    p.mask = 0;
    p.target_bits = 0;
    p.rounds = 8;

    const auto r = BruteSearchReference(p);
    CHECK(r.found_count == 1000);
    CHECK(r.min_match == 0);
}

TEST_CASE("the range is walked with wrapping arithmetic", "[reference]") {
    // start_lo + i wraps in u32, and the kernel wraps identically. A range
    // straddling 2^32 must behave the same on both sides; holding the cursor in
    // a wider type here would silently diverge from the shader.
    BruteSearchParams p{};
    p.start_lo = 0xFFFFFFFEu;
    p.unit_count = 4;
    p.mask = 0;             // match everything, so the count is the assertion
    p.target_bits = 0;
    p.rounds = 8;

    const auto r = BruteSearchReference(p);
    CHECK(r.found_count == 4);
    // Candidates are 0xFFFFFFFE, 0xFFFFFFFF, 0, 1 — so the minimum is 0.
    CHECK(r.min_match == 0);
    CHECK(r.match_xor == (0xFFFFFFFEu ^ 0xFFFFFFFFu ^ 0u ^ 1u));
}

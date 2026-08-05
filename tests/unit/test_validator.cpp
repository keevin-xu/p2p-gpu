// Steps 3.1-3.3 — determinism-class comparators.
//
// The case these exist for: R6 says honest workers on different vendors DO
// disagree, and 0.16 measured it — 63.3% of transcendental results differed
// across Apple Metal and NVIDIA D3D12, by up to 5 ULP. A comparator that calls
// that cheating blacklists honest workers, and one that calls a wrong answer
// "close enough" is not validation. Both failure directions get tests.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "p2pgpu/coordinator/validator.hpp"

using namespace p2pgpu::coordinator;

namespace {

KernelSpec MakeSpec(Determinism d, float rel = 1e-6F, float abs_eps = 1e-9F) {
    KernelSpec s;
    s.determinism = d;
    s.rel_eps = rel;
    s.abs_eps = abs_eps;
    return s;
}

/// Floats -> bytes without a cast, mirroring how the comparator reads them.
std::vector<std::byte> Bytes(const std::vector<float>& v) {
    std::vector<std::byte> out(v.size() * sizeof(float));
    for (std::size_t i = 0; i < v.size(); ++i) {
        const auto raw = std::bit_cast<std::array<std::byte, sizeof(float)>>(v[i]);
        for (std::size_t k = 0; k < sizeof(float); ++k) {
            out[i * sizeof(float) + k] = raw[k];
        }
    }
    return out;
}

/// The float `n` representable steps above `x` — how a real ULP-level
/// disagreement is constructed, rather than by picking a decimal that looks
/// small.
float NudgeUlps(float x, int n) {
    auto bits = std::bit_cast<std::int32_t>(x);
    bits += n;
    return std::bit_cast<float>(bits);
}

std::vector<std::byte> RawBytes(const std::vector<std::uint8_t>& v) {
    std::vector<std::byte> out(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        out[i] = static_cast<std::byte>(v[i]);
    }
    return out;
}

}  // namespace

// ── Exact ────────────────────────────────────────────────────────────────

TEST_CASE("Exact accepts identical bytes and rejects a single flipped bit",
          "[validator]") {
    const auto spec = MakeSpec(Determinism::Exact);
    const auto a = RawBytes({1, 2, 3, 4, 5, 6, 7, 8});

    CHECK(Compare(spec, a, a).matched());

    auto b = a;
    b[5] = static_cast<std::byte>(0xFF);
    const auto c = Compare(spec, a, b);
    CHECK(c.verdict == Verdict::Mismatch);
    CHECK(c.differing_elements == 1);
    // The INDEX matters for 3.3's log: "one byte at offset 5" and "everything
    // differs" are different evidence for reputation (3.7).
    CHECK(c.first_differing_index == 5);
}

TEST_CASE("Exact counts every differing byte, not just the first",
          "[validator]") {
    const auto spec = MakeSpec(Determinism::Exact);
    const auto a = RawBytes({0, 0, 0, 0});
    const auto b = RawBytes({9, 9, 9, 9});
    const auto c = Compare(spec, a, b);
    // "4 of 4 differ" vs "1 of 4 differs" is the difference between a liar and
    // a bit flip, and stopping at the first match would erase it.
    CHECK(c.differing_elements == 4);
    CHECK(c.elements == 4);
}

// ── Tolerant — the R6 case ───────────────────────────────────────────────

TEST_CASE("Tolerant accepts the cross-vendor divergence 0.16 actually measured",
          "[validator]") {
    // 5 ULP was the WORST case observed between Apple Metal and NVIDIA D3D12.
    // If this fails, honest NVIDIA workers get blacklisted in production.
    const auto spec = MakeSpec(Determinism::Tolerant);
    const std::vector<float> ref{1.0F, 3.14159F, 1e6F, 2.71828F};
    std::vector<float> other;
    for (const float f : ref) {
        other.push_back(NudgeUlps(f, 5));
    }

    const auto c = Compare(spec, Bytes(other), Bytes(ref));
    CHECK(c.matched());
    CHECK(c.max_ulp_diff == 5);
    // Deviation is REPORTED even on a match — 3.7 needs the distance, not the
    // verdict (D-0053).
    CHECK(c.max_rel_diff > 0.0);
}

TEST_CASE("Tolerant rejects a genuinely wrong answer", "[validator]") {
    const auto spec = MakeSpec(Determinism::Tolerant);
    const std::vector<float> ref{1.0F, 2.0F, 3.0F};
    const std::vector<float> wrong{1.0F, 2.5F, 3.0F};

    const auto c = Compare(spec, Bytes(wrong), Bytes(ref));
    CHECK(c.verdict == Verdict::Mismatch);
    CHECK(c.differing_elements == 1);
    CHECK(c.first_differing_index == 1);
    // Orders of magnitude past the epsilon, which is what tells 3.7 this is not
    // two honest GPUs disagreeing.
    CHECK(c.max_rel_diff > 0.2);
}

TEST_CASE("Tolerant epsilon sits comfortably above the measured divergence",
          "[validator]") {
    // The design claim in D-0053: rel_eps 1e-6 leaves headroom over 5 ULP.
    // Pinned as a test so a later tightening of the epsilon fails HERE rather
    // than as a mysterious wave of rejections from one vendor.
    const auto spec = MakeSpec(Determinism::Tolerant);
    const std::vector<float> ref{1.0F};
    CHECK(Compare(spec, Bytes({NudgeUlps(1.0F, 5)}), Bytes(ref)).matched());
    CHECK(Compare(spec, Bytes({NudgeUlps(1.0F, 8)}), Bytes(ref)).matched());
    // Far enough out that it must NOT pass, or the epsilon is doing nothing.
    CHECK_FALSE(Compare(spec, Bytes({NudgeUlps(1.0F, 400)}), Bytes(ref)).matched());
}

TEST_CASE("abs_eps carries values near zero where relative error is meaningless",
          "[validator]") {
    const auto spec = MakeSpec(Determinism::Tolerant, /*rel=*/1e-6F, /*abs=*/1e-9F);
    // Relative error against 0.0 is undefined; without abs_eps this rejects a
    // correct result whose reference happens to be zero.
    CHECK(Compare(spec, Bytes({1e-12F}), Bytes({0.0F})).matched());
    CHECK_FALSE(Compare(spec, Bytes({1e-3F}), Bytes({0.0F})).matched());
}

TEST_CASE("+0.0 and -0.0 are zero ULP apart", "[validator]") {
    const auto spec = MakeSpec(Determinism::Tolerant);
    const auto c = Compare(spec, Bytes({-0.0F}), Bytes({0.0F}));
    CHECK(c.matched());
    // A same-sign-only ULP implementation reports a huge distance here, which
    // would then poison max_ulp_diff for the whole buffer.
    CHECK(c.max_ulp_diff == 0);
}

TEST_CASE("NaN is never silently within tolerance", "[validator]") {
    const auto spec = MakeSpec(Determinism::Tolerant);
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // NaN fails every comparison INCLUDING `>`, so an unguarded
    // `abs_diff > allowed` accepts it — silently passing the one output most
    // likely to mean the kernel broke.
    CHECK_FALSE(Compare(spec, Bytes({nan}), Bytes({1.0F})).matched());
    CHECK_FALSE(Compare(spec, Bytes({1.0F}), Bytes({nan})).matched());
    // Both NaN is agreement in the only sense available.
    CHECK(Compare(spec, Bytes({nan}), Bytes({nan})).matched());
}

TEST_CASE("infinity does not compare equal to a finite value", "[validator]") {
    const auto spec = MakeSpec(Determinism::Tolerant);
    const float inf = std::numeric_limits<float>::infinity();
    CHECK_FALSE(Compare(spec, Bytes({inf}), Bytes({1e30F})).matched());
    CHECK(Compare(spec, Bytes({inf}), Bytes({inf})).matched());
}

// ── Malformed and unsupported ────────────────────────────────────────────

TEST_CASE("differing lengths are a mismatch under every class", "[validator]") {
    for (const auto d : {Determinism::Exact, Determinism::Tolerant,
                         Determinism::Statistical}) {
        const auto c = Compare(MakeSpec(d), RawBytes({1, 2, 3}), RawBytes({1, 2}));
        CHECK(c.verdict == Verdict::Mismatch);
        CHECK_FALSE(c.detail.empty());
    }
}

TEST_CASE("a Tolerant payload that is not whole floats is Unsupported",
          "[validator]") {
    const auto spec = MakeSpec(Determinism::Tolerant);
    const auto a = RawBytes({1, 2, 3, 4, 5});   // 5 bytes
    const auto c = Compare(spec, a, a);
    // NOT Match. Reading 1 float and ignoring the trailing byte would let an
    // attacker hide payload in the remainder.
    CHECK(c.verdict == Verdict::Unsupported);
}

TEST_CASE("Statistical reports Unsupported, never Match", "[validator]") {
    const auto spec = MakeSpec(Determinism::Statistical);
    const auto a = RawBytes({1, 2, 3, 4});
    const auto c = Compare(spec, a, a);
    // Identical inputs, and it still must not claim to have validated them.
    // A stub that passes everything is indistinguishable from validation that
    // works (D-0047, D-0053).
    CHECK(c.verdict == Verdict::Unsupported);
    CHECK_FALSE(c.matched());
    CHECK_FALSE(c.detail.empty());
}

TEST_CASE("Describe carries the numbers 3.3 needs in a rejection log",
          "[validator]") {
    const auto spec = MakeSpec(Determinism::Tolerant);
    const auto c = Compare(spec, Bytes({2.0F}), Bytes({1.0F}));
    const std::string s = Describe(c);
    // A misdeclared determinism class shows up as mismatches from honest
    // workers (RISKS.md §2); the deviation is what distinguishes that from a
    // liar at a glance, so it must be IN the line.
    CHECK(s.find("verdict=Mismatch") != std::string::npos);
    CHECK(s.find("max_ulp=") != std::string::npos);
    CHECK(s.find("max_rel=") != std::string::npos);
    CHECK(s.find("first_idx=") != std::string::npos);
}

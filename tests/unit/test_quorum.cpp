// Steps 3.4-3.6 — replication policy and quorum.
//
// The cases worth constructing directly, because a live fleet produces them
// rarely and unrepeatably: a 2-2 split at the replica cap, a lone dissenter,
// and two honest GPUs agreeing only to within a few ULP.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <vector>

#include "p2pgpu/coordinator/quorum.hpp"

using namespace p2pgpu::coordinator;
using p2pgpu::protocol::WorkerId;

namespace {

KernelSpec ExactSpec() {
    KernelSpec s;
    s.determinism = Determinism::Exact;
    return s;
}

KernelSpec TolerantSpec() {
    KernelSpec s;
    s.determinism = Determinism::Tolerant;
    s.rel_eps = 1e-6F;
    s.abs_eps = 1e-9F;
    return s;
}

Task::Submission Hashed(std::uint64_t worker, std::uint64_t checksum) {
    Task::Submission s;
    s.worker = WorkerId{worker, worker};
    s.checksum = checksum;
    return s;
}

Task::Submission Floats(std::uint64_t worker, const std::vector<float>& v) {
    Task::Submission s;
    s.worker = WorkerId{worker, worker};
    s.payload.resize(v.size() * sizeof(float));
    for (std::size_t i = 0; i < v.size(); ++i) {
        const auto raw = std::bit_cast<std::array<std::byte, sizeof(float)>>(v[i]);
        for (std::size_t k = 0; k < sizeof(float); ++k) {
            s.payload[i * sizeof(float) + k] = raw[k];
        }
    }
    return s;
}

float NudgeUlps(float x, int n) {
    auto bits = std::bit_cast<std::int32_t>(x);
    bits += n;
    return std::bit_cast<float>(bits);
}

QuorumConfig Cfg(ReplicationPolicy p, std::uint32_t need = 2, std::uint32_t cap = 4) {
    return QuorumConfig{p, need, cap};
}

}  // namespace

TEST_CASE("policy None accepts the first answer and validates nothing",
          "[quorum]") {
    const std::vector<Task::Submission> subs{Hashed(1, 0xAAAA)};
    const auto r = Decide(ExactSpec(), Cfg(ReplicationPolicy::None), subs);
    CHECK(r.action == QuorumAction::Accept);
    REQUIRE(r.agreeing.size() == 1);
    // Phase 2's behaviour, preserved deliberately: every Phase 2 measurement was
    // taken without replication, and doubling the work by default would make
    // them incomparable (D-0054).
}

TEST_CASE("one submission under 2x asks for a replica rather than accepting",
          "[quorum]") {
    const std::vector<Task::Submission> subs{Hashed(1, 0xAAAA)};
    const auto r = Decide(ExactSpec(), Cfg(ReplicationPolicy::Fixed2x), subs);
    CHECK(r.action == QuorumAction::NeedMoreReplicas);
    CHECK(r.agreeing.empty());
}

TEST_CASE("two agreeing Exact submissions are accepted", "[quorum]") {
    const std::vector<Task::Submission> subs{Hashed(1, 0xAAAA), Hashed(2, 0xAAAA)};
    const auto r = Decide(ExactSpec(), Cfg(ReplicationPolicy::Fixed2x), subs);
    CHECK(r.action == QuorumAction::Accept);
    CHECK(r.agreeing.size() == 2);
    CHECK(r.dissenting.empty());
}

TEST_CASE("two disagreeing submissions escalate instead of guessing",
          "[quorum]") {
    const std::vector<Task::Submission> subs{Hashed(1, 0xAAAA), Hashed(2, 0xBBBB)};
    const auto r = Decide(ExactSpec(), Cfg(ReplicationPolicy::Fixed2x), subs);
    // With one vote each there is no majority and room left under the cap.
    CHECK(r.action == QuorumAction::NeedMoreReplicas);
    CHECK_FALSE(r.detail.empty());
}

TEST_CASE("a lone dissenter is outvoted and named", "[quorum]") {
    const std::vector<Task::Submission> subs{
        Hashed(1, 0xAAAA), Hashed(2, 0xBBBB), Hashed(3, 0xAAAA)};
    const auto r = Decide(ExactSpec(), Cfg(ReplicationPolicy::Fixed2x), subs);
    CHECK(r.action == QuorumAction::Accept);
    CHECK(r.agreeing.size() == 2);
    REQUIRE(r.dissenting.size() == 1);
    // 3.7 weights the penalty by deviation, so the dissenter is REPORTED, not
    // judged here.
    CHECK(r.dissenting.front() == WorkerId{2, 2});
}

TEST_CASE("a 2-2 split at the cap is inconclusive, NOT a coin flip",
          "[quorum]") {
    const std::vector<Task::Submission> subs{
        Hashed(1, 0xAAAA), Hashed(2, 0xBBBB), Hashed(3, 0xAAAA), Hashed(4, 0xBBBB)};
    const auto r = Decide(ExactSpec(), Cfg(ReplicationPolicy::Fixed2x, 3, 4), subs);
    // THE case this exists for. Two clusters of two: no strict majority, and
    // picking either would put an unvalidated answer into the output while
    // reporting it as validated.
    CHECK(r.action == QuorumAction::Inconclusive);
    CHECK(r.agreeing.empty());
    CHECK_FALSE(r.detail.empty());
}

TEST_CASE("a strict majority at the cap decides", "[quorum]") {
    const std::vector<Task::Submission> subs{
        Hashed(1, 0xAAAA), Hashed(2, 0xBBBB), Hashed(3, 0xAAAA), Hashed(4, 0xCCCC)};
    // required_agreement 3 is never reached, but 2 of 4 is not a majority
    // either — 2*2 > 4 is false, so this must be inconclusive.
    const auto r = Decide(ExactSpec(), Cfg(ReplicationPolicy::Fixed2x, 3, 4), subs);
    CHECK(r.action == QuorumAction::Inconclusive);
}

TEST_CASE("three of four at the cap is a majority and is accepted", "[quorum]") {
    const std::vector<Task::Submission> subs{
        Hashed(1, 0xAAAA), Hashed(2, 0xBBBB), Hashed(3, 0xAAAA), Hashed(4, 0xAAAA)};
    const auto r = Decide(ExactSpec(), Cfg(ReplicationPolicy::Fixed2x, 9, 4), subs);
    CHECK(r.action == QuorumAction::Accept);
    CHECK(r.agreeing.size() == 3);
    CHECK(r.dissenting.size() == 1);
}

// ── Tolerant: the R6 case, where "agree" is not "identical" ──────────────

TEST_CASE("two honest GPUs differing by 5 ULP agree", "[quorum]") {
    // Exactly what 0.16 measured between Apple Metal and NVIDIA D3D12. If this
    // escalates to a replica, every cross-vendor task pays double forever.
    const std::vector<float> ref{1.0F, 3.14159F, 1e6F};
    std::vector<float> other;
    for (const float f : ref) {
        other.push_back(NudgeUlps(f, 5));
    }
    const std::vector<Task::Submission> subs{Floats(1, ref), Floats(2, other)};

    const auto r = Decide(TolerantSpec(), Cfg(ReplicationPolicy::Fixed2x), subs);
    CHECK(r.action == QuorumAction::Accept);
    CHECK(r.agreeing.size() == 2);
    // The deviation inside the ACCEPTED group is reported — 3.7 treats
    // "agreed bitwise" and "agreed at 5 ULP" as different evidence.
    CHECK(r.agreeing_max_ulp == 5);
}

TEST_CASE("a wrong answer does not hide behind the tolerance", "[quorum]") {
    const std::vector<Task::Submission> subs{
        Floats(1, {1.0F, 2.0F}), Floats(2, {1.0F, 2.5F})};
    const auto r = Decide(TolerantSpec(), Cfg(ReplicationPolicy::Fixed2x), subs);
    CHECK(r.action == QuorumAction::NeedMoreReplicas);
}

TEST_CASE("clustering compares against every member, not a representative",
          "[quorum]") {
    // Tolerance is NOT transitive: a can be within epsilon of b, and b of c,
    // while a and c are not. Comparing only against a cluster representative
    // lets the cluster drift arbitrarily far.
    //
    // Built to catch the SPECIFIC wrong implementation: comparing a candidate
    // only against a cluster's first member.
    //
    // C agrees with A (6 ULP) but not with B (12 ULP), and rel_eps 1e-6 is
    // ~8 ULP at 1.0. Compare-against-first admits C into {A,B} and reports 3
    // agreeing; compare-against-every-member correctly keeps it out.
    //
    // A ladder (A, A+6, A+12) would NOT catch this — the third element
    // disagrees with the first under both implementations, so it passes either
    // way and proves nothing.
    const float base = 1.0F;
    const std::vector<Task::Submission> subs{
        Floats(1, {base}),
        Floats(2, {NudgeUlps(base, 6)}),
        Floats(3, {NudgeUlps(base, -6)}),
    };
    const auto r = Decide(TolerantSpec(), Cfg(ReplicationPolicy::Fixed2x, 3, 4), subs);
    CHECK(r.agreeing.size() != 3);
}

TEST_CASE("Statistical never accumulates votes", "[quorum]") {
    KernelSpec s;
    s.determinism = Determinism::Statistical;
    const std::vector<Task::Submission> subs{
        Floats(1, {1.0F}), Floats(2, {1.0F})};
    const auto r = Decide(s, Cfg(ReplicationPolicy::Fixed2x), subs);
    // Identical payloads, and it still must not accept: there is no comparator
    // for this class until Phase 5, and a stub voting yes is indistinguishable
    // from validation that works (D-0053).
    CHECK(r.action != QuorumAction::Accept);
}

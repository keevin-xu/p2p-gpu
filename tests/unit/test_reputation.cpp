// Steps 3.7-3.11 — reputation, adaptive replication, blacklist, and the fault
// classes that must NOT touch any of it.
//
// The most valuable case here is a negative one: 3.11 says exactly one of four
// events may penalise a worker, and conflating them is the fastest way to
// blacklist an honest fleet. Three of the four are things a perfectly honest
// worker does routinely on bad hardware or bad wifi.

#include <catch2/catch_test_macros.hpp>

#include "p2pgpu/coordinator/quorum.hpp"
#include "p2pgpu/coordinator/reputation.hpp"

using namespace p2pgpu::coordinator;
using p2pgpu::protocol::WorkerId;

namespace {
constexpr WorkerId kAlice{1, 1};
constexpr WorkerId kBob{2, 2};
constexpr std::uint64_t kNow = 1'000'000;
}  // namespace

// ── 3.7 — the score carries confidence ───────────────────────────────────

TEST_CASE("one correct result does not look like two hundred", "[reputation]") {
    ReputationTable rep;
    rep.RecordAccepted(kAlice);

    ReputationTable veteran;
    for (int i = 0; i < 200; ++i) {
        veteran.RecordAccepted(kBob);
    }

    // A naive success ratio gives both 1.0, and 3.8 would then skip validating
    // a worker that has been right exactly once (D-0055).
    CHECK(rep.ScoreOf(kAlice) < 0.7);
    CHECK(veteran.ScoreOf(kBob) > 0.98);
}

TEST_CASE("an unknown worker scores the prior, not zero", "[reputation]") {
    const ReputationTable rep;
    // Scoring newcomers 0 would make every arrival look like a liar, and the
    // adaptive policy would replicate the entire incoming fleet forever.
    CHECK(rep.ScoreOf(kAlice) == 0.5);
    CHECK(rep.size() == 0);   // and asking must not allocate
}

TEST_CASE("penalty scales with how wrong the answer was", "[reputation]") {
    // THE guard against R6 leaking into reputation. 0.16 measured 5 ULP between
    // two honest vendors; if that is punished like a fabricated answer, honest
    // NVIDIA workers drift toward the blacklist for running on NVIDIA.
    const double near_miss = SeverityFromDeviation(5, 1e-7);
    const double way_off = SeverityFromDeviation(5'000'000, 0.5);
    const double structural = SeverityFromDeviation(0xFFFFFFFF,
                                                   std::numeric_limits<double>::infinity());

    CHECK(near_miss < 0.5);
    CHECK(way_off > near_miss * 4);
    CHECK(structural >= way_off);
}

TEST_CASE("agreeing within tolerance earns full credit", "[reputation]") {
    ReputationTable a;
    ReputationTable b;
    for (int i = 0; i < 20; ++i) {
        a.RecordAccepted(kAlice, /*deviation_ulp=*/0);
        b.RecordAccepted(kAlice, /*deviation_ulp=*/5);
    }
    // Within tolerance IS correct (R6). A cross-vendor worker must not
    // accumulate a worse record than a same-vendor one for agreeing exactly as
    // the standard says it will.
    CHECK(a.ScoreOf(kAlice) == b.ScoreOf(kAlice));
}

// ── 3.10 — blacklist and probation ───────────────────────────────────────

TEST_CASE("a persistent liar is blacklisted, then returns on probation",
          "[reputation]") {
    ReputationTable rep;
    for (int i = 0; i < 12; ++i) {
        rep.RecordRejected(kAlice, 4.0);
    }
    REQUIRE(rep.ScoreOf(kAlice) < 0.30);
    CHECK(rep.MaybeBlacklist(kAlice, kNow));
    CHECK(rep.IsBlacklisted(kAlice, kNow));

    // Still barred just before the cooldown ends.
    const auto cooldown = rep.config().cooldown_ms;
    CHECK(rep.IsBlacklisted(kAlice, kNow + cooldown - 1));

    // ...and back afterwards, but ON PROBATION. Never permanent — a flaky
    // overclock is not malice, and a permanent ban makes a false positive
    // unrecoverable (which is why 3.15 exists).
    CHECK_FALSE(rep.IsBlacklisted(kAlice, kNow + cooldown));
    CHECK(rep.Find(kAlice)->on_probation);
}

TEST_CASE("probation does not wipe the record", "[reputation]") {
    ReputationTable rep;
    for (int i = 0; i < 12; ++i) {
        rep.RecordRejected(kAlice, 4.0);
    }
    REQUIRE(rep.MaybeBlacklist(kAlice, kNow));
    const double before = rep.ScoreOf(kAlice);
    (void)rep.IsBlacklisted(kAlice, kNow + rep.config().cooldown_ms);
    // Clearing the history on release would let a liar launder its record by
    // waiting out the ban.
    CHECK(rep.ScoreOf(kAlice) == before);
}

TEST_CASE("a failed spot-check counts double", "[reputation]") {
    ReputationTable a;
    ReputationTable b;
    a.RecordRejected(kAlice, 1.0, /*spot_check=*/false);
    b.RecordRejected(kAlice, 1.0, /*spot_check=*/true);
    // There is no honest disagreement with an answer we already hold.
    CHECK(b.ScoreOf(kAlice) < a.ScoreOf(kAlice));
    CHECK(b.Find(kAlice)->spot_check_failures == 1);
}

// ── 3.8 — adaptive replication is the overhead lever ─────────────────────

TEST_CASE("adaptive stops replicating an established worker", "[reputation]") {
    ReputationTable rep;
    for (int i = 0; i < 200; ++i) {
        rep.RecordAccepted(kBob);
    }
    QuorumConfig cfg;
    cfg.policy = ReplicationPolicy::Adaptive;

    // 1 == no replication at all. This is the number that moves overhead from
    // 2x toward 1x (3.8), and it is the claim E4 has to substantiate.
    CHECK(RequiredAgreementFor(cfg, rep, kBob, kNow) == 1);
}

TEST_CASE("a lucky newcomer cannot skip validation", "[reputation]") {
    ReputationTable rep;
    rep.RecordAccepted(kAlice);   // 1 for 1

    QuorumConfig cfg;
    cfg.policy = ReplicationPolicy::Adaptive;
    // Under a naive success ratio this worker scores 1.0 and gets trusted after
    // a single task — exactly the opening a liar wants. The Beta prior is what
    // closes it.
    CHECK(RequiredAgreementFor(cfg, rep, kAlice, kNow) > 1);
}

TEST_CASE("a worker on probation is replicated at maximum", "[reputation]") {
    ReputationTable rep;
    for (int i = 0; i < 12; ++i) {
        rep.RecordRejected(kAlice, 4.0);
    }
    REQUIRE(rep.MaybeBlacklist(kAlice, kNow));
    (void)rep.IsBlacklisted(kAlice, kNow + rep.config().cooldown_ms);

    QuorumConfig cfg;
    cfg.policy = ReplicationPolicy::Adaptive;
    CHECK(RequiredAgreementFor(cfg, rep, kAlice, kNow + rep.config().cooldown_ms) ==
          cfg.max_replicas);
}

TEST_CASE("fixed2x ignores reputation entirely", "[reputation]") {
    ReputationTable rep;
    for (int i = 0; i < 200; ++i) {
        rep.RecordAccepted(kBob);
    }
    QuorumConfig cfg;
    cfg.policy = ReplicationPolicy::Fixed2x;
    // The control condition must not quietly become adaptive, or E4 compares
    // the policy against itself.
    CHECK(RequiredAgreementFor(cfg, rep, kBob, kNow) == cfg.required_agreement);
}

// ── 3.11 — the fault classes, which is the test that matters ─────────────

TEST_CASE("only a wrong answer touches reputation", "[reputation][fault-classes]") {
    ReputationTable rep;
    const double baseline = rep.ScoreOf(kAlice);

    // Three events that a PERFECTLY HONEST worker produces routinely. None has
    // any way to reach this table, and that is enforced by there being no API
    // for it — `ReputationTable` exposes exactly two mutators, both of which
    // mean "this worker computed a result and we judged it".
    //
    //   1. lease expiry        — the worker vanished (R8: normal, not misconduct)
    //   2. checksum mismatch   — the network corrupted bytes in transit
    //   3. malformed frame     — connection hygiene (3.12), scored per-connection
    //
    // If any of those ever needs to be recorded here, this test should be the
    // thing that argues against it.
    CHECK(rep.ScoreOf(kAlice) == baseline);
    CHECK(rep.size() == 0);

    // 4. A wrong answer whose checksum was intact. The ONLY one.
    rep.RecordRejected(kAlice, SeverityFromDeviation(5'000'000, 0.5));
    CHECK(rep.ScoreOf(kAlice) < baseline);
}

TEST_CASE("a near-miss disagreement barely moves the score",
          "[reputation][fault-classes]") {
    ReputationTable rep;
    for (int i = 0; i < 30; ++i) {
        rep.RecordAccepted(kAlice);
    }
    const double before = rep.ScoreOf(kAlice);

    // An honest cross-vendor worker outvoted once, at the divergence 0.16
    // actually measured. It should cost almost nothing — 3.15's false-positive
    // control depends on this not accumulating into a ban.
    rep.RecordRejected(kAlice, SeverityFromDeviation(5, 1e-7));
    CHECK(before - rep.ScoreOf(kAlice) < 0.02);
    CHECK(rep.ScoreOf(kAlice) > rep.config().blacklist_below);
}

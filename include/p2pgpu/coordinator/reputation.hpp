#pragma once
//
// Worker reputation — steps 3.7-3.10.
//
// ── THE SCORE CARRIES ITS OWN CONFIDENCE ─────────────────────────────────
// Reputation is a Beta(alpha, beta) posterior, scored `alpha / (alpha + beta)`.
// A naive success ratio gives 1/1 and 200/200 the same 1.0, and 3.8's whole
// purpose is to stop replicating workers with a LONG record — not workers who
// got lucky once. From a (2,2) prior, one success scores 0.60 and two hundred
// score 0.99, so a single threshold expresses both "is it good" and "do we know
// yet" (D-0055).
//
// ── ONLY ONE FAULT CLASS REACHES THIS FILE ───────────────────────────────
// Lease expiry, checksum mismatch and malformed frames must NOT call anything
// here. They are, respectively, a worker vanishing (R8 — normal), the network
// corrupting bytes, and connection hygiene (3.12). All three are things an
// honest worker on bad hardware or bad wifi does routinely, and penalising them
// is the fastest way to blacklist a healthy fleet (3.11).
//
// The only input is: a worker computed a result, its checksum was fine, and the
// result was wrong.

#include <cstdint>
#include <string>
#include <unordered_map>

#include "p2pgpu/protocol/ids.hpp"

namespace p2pgpu::coordinator {

using protocol::WorkerId;

struct ReputationConfig {
    /// Beta prior. Weak on purpose: a new worker is neither trusted nor
    /// suspected, and (2,2) is washed out by roughly ten observations.
    double alpha0 = 2.0;
    double beta0 = 2.0;

    /// Below this a worker is blacklisted (3.10).
    double blacklist_below = 0.30;
    /// Blacklist duration. NEVER permanent — a flaky overclock is not malice,
    /// and a permanent ban turns a hardware fault into a lost volunteer forever.
    std::uint64_t cooldown_ms = 300'000;

    /// A worker at or above this is considered established, and 3.8 stops
    /// replicating it. This is the knob that moves overhead from 2x toward 1x.
    double trusted_at = 0.90;

    /// Cap on the penalty a single wrong answer can apply. Without it one
    /// absurd deviation (an inf, a NaN, a garbage buffer) would blacklist a
    /// worker outright, and 3.15 exists because we expect some honest results
    /// to look wrong.
    double max_penalty = 4.0;
};

/// What a worker has done, and what we think of it.
struct Reputation {
    double alpha = 2.0;
    double beta = 2.0;

    std::uint32_t accepted = 0;
    std::uint32_t rejected = 0;
    /// Spot-checks failed (3.9). Counted separately because a failed
    /// known-answer task is far stronger evidence than losing a vote — there is
    /// no honest disagreement with an answer we already hold.
    std::uint32_t spot_check_failures = 0;

    /// Set while blacklisted; the value is when probation begins (3.10).
    std::uint64_t blacklisted_until_ms = 0;
    /// True after a blacklist expires, until the worker re-earns its score.
    /// On probation a worker is replicated at the maximum factor — it is not
    /// refused work, because refusing it forever is how a false positive
    /// becomes permanent.
    bool on_probation = false;

    [[nodiscard]] double score() const noexcept {
        const double n = alpha + beta;
        return n > 0.0 ? alpha / n : 0.0;
    }
};

class ReputationTable {
public:
    explicit ReputationTable(ReputationConfig cfg = {}) : cfg_(cfg) {}

    /// The worker computed a result that was ACCEPTED.
    ///
    /// `deviation_ulp` is how far it was from the agreed answer — 0 for bitwise
    /// agreement. Full credit either way: within tolerance IS correct (R6), and
    /// a cross-vendor worker must not accumulate a worse record than a
    /// same-vendor one for agreeing exactly as the standard says it will.
    void RecordAccepted(WorkerId id, std::uint32_t deviation_ulp = 0);

    /// The worker computed a result that was WRONG — checksum intact, answer
    /// outvoted or contradicted by a known answer.
    ///
    /// `severity` scales the penalty and is derived from the comparator's
    /// deviation (3.2): 1.0 for a near miss, larger for an answer that is not
    /// close to anything. Capped by `max_penalty`.
    void RecordRejected(WorkerId id, double severity = 1.0, bool spot_check = false);

    /// Blacklist if the score has fallen below the threshold. Returns true if
    /// this call blacklisted the worker.
    [[nodiscard]] bool MaybeBlacklist(WorkerId id, std::uint64_t now_ms);

    /// Is this worker barred right now? Expiring a blacklist moves the worker
    /// to probation rather than to a clean slate.
    [[nodiscard]] bool IsBlacklisted(WorkerId id, std::uint64_t now_ms);

    [[nodiscard]] const Reputation* Find(WorkerId id) const;
    /// Creates at the prior if absent — asking about an unknown worker is the
    /// normal case, not an error.
    [[nodiscard]] Reputation& Mutable(WorkerId id);

    [[nodiscard]] double ScoreOf(WorkerId id) const;
    [[nodiscard]] const ReputationConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] std::size_t size() const noexcept { return workers_.size(); }

    /// Everything, for persistence (3.13) and metrics.
    [[nodiscard]] const std::unordered_map<WorkerId, Reputation>& All() const noexcept {
        return workers_;
    }

private:
    ReputationConfig cfg_;
    std::unordered_map<WorkerId, Reputation> workers_;
};

/// Severity from a comparator deviation (3.2 -> 3.7).
///
/// THE POINT: 3 ULP and "off by a factor of a million" are both "not equal",
/// and penalising them the same blacklists honest cross-vendor workers. 0.16
/// measured 5 ULP between two real vendors, so anything in that neighbourhood
/// must stay near the floor.
[[nodiscard]] double SeverityFromDeviation(std::uint32_t max_ulp, double max_rel);

}  // namespace p2pgpu::coordinator

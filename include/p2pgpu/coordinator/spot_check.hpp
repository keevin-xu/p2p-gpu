#pragma once
//
// Spot-checking — step 3.9.
//
// ── CATCHES A LIAR WITHOUT PAYING FOR REPLICATION ────────────────────────
// Replication costs a second worker for every task it validates. A spot-check
// costs nothing extra: the coordinator already knows the answer, so one
// worker's result is enough to convict. It is the mechanism that can move the
// measured 1.84x overhead (D-0057) toward 1.0 without giving up detection.
//
// ── IT MUST BE INDISTINGUISHABLE FROM REAL WORK ──────────────────────────
// A spot-check is granted exactly like any other task: same message, same
// shape, no marker on the wire. If a liar could tell, it would compute those
// honestly and cheat on everything else — which makes the mechanism worse than
// useless, because it would then produce confident evidence of honesty from
// precisely the workers it exists to catch (D-0055).
//
// That is also why this header holds no "is_spot_check" field that could be
// serialized by accident. The coordinator knows; the wire does not.

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "p2pgpu/protocol/ids.hpp"

namespace p2pgpu::coordinator {

using protocol::TaskId;
using protocol::WorkerId;

struct SpotCheckConfig {
    /// Probability that a grant is a spot-check for an UNPROVEN worker.
    ///
    /// Biased toward the unproven because that is where the information is: a
    /// worker with two hundred correct results is not the one worth testing,
    /// and testing it is pure waste.
    double rate_unproven = 0.10;
    /// Rate for an established worker. Non-zero on purpose — a worker that
    /// built a record honestly and then started lying is exactly the adversary
    /// a reputation system invites, and a zero rate here would make becoming
    /// trusted a permanent licence.
    double rate_trusted = 0.02;
    /// A worker is "proven" at or above this score.
    double proven_at = 0.90;
};

/// Known-answer tasks the coordinator can inject.
class SpotCheckPool {
public:
    explicit SpotCheckPool(SpotCheckConfig cfg = {}) : cfg_(cfg) {}

    /// Remember a verified answer so this range can be re-issued later.
    ///
    /// Populated from results that CLEARED VALIDATION — a quorum agreed, or a
    /// replica confirmed. Seeding it from a single unvalidated result would
    /// enshrine one worker's answer as ground truth and then punish everyone
    /// who disagreed with it, which is a way to convert one liar into a fleet
    /// of blacklisted honest workers.
    void Remember(std::uint64_t start_unit, std::uint64_t unit_count,
                  std::uint64_t checksum);

    /// Should this grant be a spot-check, and if so over which range?
    ///
    /// `roll` is a caller-supplied uniform [0,1) so the decision is seeded and
    /// replayable — an experiment whose injections cannot be replayed produces
    /// anecdotes (2.3's rule, applied here).
    struct Pick {
        std::uint64_t start_unit = 0;
        std::uint64_t unit_count = 0;
        std::uint64_t expected_checksum = 0;
    };
    [[nodiscard]] std::optional<Pick> Maybe(double worker_score, double roll,
                                            std::uint64_t index_roll) const;

    /// Record that `task` was issued as a spot-check over a known range.
    void MarkIssued(TaskId task, std::uint64_t expected_checksum);

    /// Was this task a spot-check, and what was the right answer?
    [[nodiscard]] std::optional<std::uint64_t> ExpectedFor(TaskId task) const;

    /// Forget a resolved spot-check. Called on any terminal outcome, so the
    /// map cannot grow without bound over a long uptime.
    void Forget(TaskId task);

    [[nodiscard]] std::size_t size() const noexcept { return known_.size(); }
    [[nodiscard]] std::size_t outstanding() const noexcept { return issued_.size(); }
    [[nodiscard]] const SpotCheckConfig& config() const noexcept { return cfg_; }

private:
    struct Known {
        std::uint64_t start_unit = 0;
        std::uint64_t unit_count = 0;
        std::uint64_t checksum = 0;
    };

    SpotCheckConfig cfg_;
    std::vector<Known> known_;
    std::unordered_map<TaskId, std::uint64_t> issued_;
};

}  // namespace p2pgpu::coordinator

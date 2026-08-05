#pragma once
//
// Replication policy and quorum — steps 3.4-3.6.
//
// ── PURE, AND DELIBERATELY SO ────────────────────────────────────────────
// `Decide` takes the submissions and the policy and returns what to do. It
// owns no state, touches no clock, and issues nothing — so every branch is
// unit-testable without a fleet, and the interesting cases (a 2-2 split at the
// cap, a lone dissenter, a worker that disagrees only in the last ULP) can be
// constructed directly instead of hoped for.
//
// ── NO MAJORITY IS NOT "PICK ONE" ────────────────────────────────────────
// With no majority there is no evidence. Choosing a result anyway would put an
// unvalidated answer into the output while reporting it as validated, which is
// worse than admitting the group failed and re-running it with fresh workers.

#include <cstdint>
#include <span>
#include <vector>

#include "p2pgpu/coordinator/job.hpp"
#include "p2pgpu/coordinator/kernel_registry.hpp"
#include "p2pgpu/coordinator/reputation.hpp"

namespace p2pgpu::coordinator {

/// How much replication to demand. `None` is the default because every Phase 2
/// measurement was taken without it, and silently doubling the work would make
/// those numbers incomparable (D-0054).
enum class ReplicationPolicy : std::uint8_t {
    /// Accept the first result. No validation — Phase 2 behaviour.
    None,
    /// Every task computed twice. E4's CONTROL CONDITION (3.6): without it
    /// there is nothing for the adaptive policy (3.8) to claim improvement
    /// over.
    Fixed2x,
    /// Reputation-weighted (3.8). Selectable now so the plumbing exists; it
    /// behaves as `Fixed2x` until 3.8 supplies the weighting.
    Adaptive,
};

[[nodiscard]] constexpr const char* ToString(ReplicationPolicy p) noexcept {
    switch (p) {
        case ReplicationPolicy::None:    return "none";
        case ReplicationPolicy::Fixed2x: return "fixed2x";
        case ReplicationPolicy::Adaptive: return "adaptive";
    }
    return "?";
}

struct QuorumConfig {
    ReplicationPolicy policy = ReplicationPolicy::None;
    /// Agreeing submissions needed to accept.
    std::uint32_t required_agreement = 2;
    /// Hard ceiling on submissions for one task. Bounds what a disagreement can
    /// cost: without it, two workers that disagree forever consume the fleet.
    std::uint32_t max_replicas = 4;
};

enum class QuorumAction : std::uint8_t {
    /// Enough agreement. Accept, and credit the agreeing workers.
    Accept,
    /// Not enough answers yet. Issue another replica to a worker that has not
    /// seen this range (invariant 6).
    NeedMoreReplicas,
    /// At the cap with no majority. Requeue for a FRESH set of workers — this
    /// is not a rejection of anybody, it is an admission that the group
    /// produced no evidence.
    Inconclusive,
};

struct QuorumResult {
    QuorumAction action = QuorumAction::NeedMoreReplicas;
    /// Workers whose answer is the accepted one. Empty unless `Accept`.
    std::vector<protocol::WorkerId> agreeing;
    /// Workers outvoted by an accepted majority — 3.7 weights the penalty by
    /// how far off they were, so this is not the same as "cheaters".
    std::vector<protocol::WorkerId> dissenting;
    /// Worst deviation seen inside the ACCEPTED group, for 3.7. Two honest GPUs
    /// agreeing at 3 ULP is different evidence from two agreeing bitwise.
    std::uint32_t agreeing_max_ulp = 0;
    /// Populated when the action is driven by a disagreement, for the 3.3 log.
    std::string detail;
};

/// Decide what to do with the answers received so far.
///
/// `spec` supplies the determinism class and epsilons, so "agree" means what
/// the kernel says it means — bitwise for `Exact`, within tolerance for
/// `Tolerant` (D-0053).
[[nodiscard]] QuorumResult Decide(const KernelSpec& spec, const QuorumConfig& cfg,
                                  std::span<const Task::Submission> submissions);

/// How many agreeing answers this worker's result needs (3.8).
///
/// **This is the function that moves overhead from 2x toward 1x**, and it is
/// BOINC's technique: replicate the unproven, trust the established.
///
///   - blacklisted or on probation -> the maximum. A worker returning from a
///     ban is exactly the one to check, and probation would be meaningless
///     otherwise.
///   - score >= `trusted_at` -> 1, i.e. no replication at all. The long record
///     IS the evidence; demanding a second opinion from a worker with two
///     hundred correct results buys almost nothing and doubles its cost.
///   - otherwise -> the configured requirement.
///
/// A Beta score is what makes the middle branch safe: 1/1 correct scores 0.60,
/// not 1.0, so a lucky newcomer cannot reach `trusted_at` and skip validation
/// (D-0055). With a naive success ratio this policy would trust anyone who got
/// their first task right — which is precisely the opening a liar wants.
[[nodiscard]] std::uint32_t RequiredAgreementFor(const QuorumConfig& cfg,
                                                 const ReputationTable& rep,
                                                 WorkerId worker,
                                                 std::uint64_t now_ms);

}  // namespace p2pgpu::coordinator

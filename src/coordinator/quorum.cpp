// Replication policy and quorum — steps 3.4-3.6. See quorum.hpp.

#include "p2pgpu/coordinator/quorum.hpp"

#include <algorithm>
#include <limits>
#include <string>

#include "p2pgpu/coordinator/validator.hpp"

namespace p2pgpu::coordinator {
namespace {

/// Do two submissions say the same thing, under this kernel's class?
///
/// For `Exact` the checksums ARE the comparison (D-0054): bitwise equality and
/// checksum equality are the same question, and the payload is not retained.
/// For everything else the bytes are compared through the 3.1 comparator, so
/// "agree" means what the kernel declared it means — an honest 5 ULP
/// cross-vendor divergence agrees, and a wrong answer does not.
struct Agreement {
    bool agree = false;
    std::uint32_t max_ulp = 0;
};

Agreement Agrees(const KernelSpec& spec, const Task::Submission& a,
                 const Task::Submission& b) {
    if (spec.determinism == Determinism::Exact) {
        return {a.checksum == b.checksum, 0};
    }
    const Comparison c = Compare(spec, a.payload, b.payload);
    // `Unsupported` is NOT agreement. A class with no comparator (Statistical,
    // until Phase 5) must not accumulate votes — that would be a stub voting
    // yes, which is the failure D-0053 refused to ship.
    return {c.verdict == Verdict::Match, c.max_ulp_diff};
}

}  // namespace

QuorumResult Decide(const KernelSpec& spec, const QuorumConfig& cfg,
                    std::span<const Task::Submission> submissions) {
    QuorumResult r;

    if (submissions.empty()) {
        r.action = QuorumAction::NeedMoreReplicas;
        return r;
    }

    // Policy None: Phase 2 behaviour, first answer wins, no validation. Stated
    // as a branch rather than a special case elsewhere so the absence of
    // validation is visible at the place validation would happen.
    if (cfg.policy == ReplicationPolicy::None) {
        r.action = QuorumAction::Accept;
        r.agreeing.push_back(submissions.front().worker);
        return r;
    }

    // Group submissions into agreement clusters. O(n^2) over n <= max_replicas,
    // which is a handful — and transitivity does not hold for a tolerance-based
    // comparison, so a cheap union-find would be quietly wrong: a can be within
    // epsilon of b and b of c while a and c are not.
    std::vector<std::vector<std::size_t>> clusters;
    std::vector<std::uint32_t> cluster_ulp;
    for (std::size_t i = 0; i < submissions.size(); ++i) {
        bool placed = false;
        for (std::size_t ci = 0; ci < clusters.size() && !placed; ++ci) {
            // Compare against EVERY member, not just the first. With a
            // tolerance, "agrees with the representative" is weaker than
            // "agrees with the group", and the weaker rule lets a cluster drift.
            bool agrees_with_all = true;
            std::uint32_t worst = cluster_ulp[ci];
            for (const std::size_t member : clusters[ci]) {
                const Agreement ag = Agrees(spec, submissions[i], submissions[member]);
                if (!ag.agree) {
                    agrees_with_all = false;
                    break;
                }
                worst = std::max(worst, ag.max_ulp);
            }
            if (agrees_with_all) {
                clusters[ci].push_back(i);
                cluster_ulp[ci] = worst;
                placed = true;
            }
        }
        if (!placed) {
            clusters.push_back({i});
            cluster_ulp.push_back(0);
        }
    }

    // Largest cluster wins, if anything wins at all.
    std::size_t best = 0;
    for (std::size_t ci = 1; ci < clusters.size(); ++ci) {
        if (clusters[ci].size() > clusters[best].size()) {
            best = ci;
        }
    }
    const std::size_t best_size = clusters[best].size();
    const auto n = static_cast<std::uint32_t>(submissions.size());

    // How far the losers are from the winner. Measured against a member of the
    // accepted cluster, because "wrong" here means "disagrees with what we
    // accepted" — a dissenter's distance from another dissenter is not evidence
    // about anything.
    const auto measure_dissent = [&]() {
        const Task::Submission& winner = submissions[clusters[best].front()];
        for (std::size_t ci = 0; ci < clusters.size(); ++ci) {
            if (ci == best) {
                continue;
            }
            for (const std::size_t idx : clusters[ci]) {
                if (spec.determinism == Determinism::Exact) {
                    // No notion of "how far" for a hash. Treat any Exact
                    // disagreement as structural — for an integer kernel it is:
                    // 0.16 measured 1000/1000 bitwise agreement across vendors,
                    // so there is no honest way to differ.
                    r.dissent_max_ulp = std::numeric_limits<std::uint32_t>::max();
                    r.dissent_max_rel = 1.0;
                    continue;
                }
                const Comparison c = Compare(spec, submissions[idx].payload, winner.payload);
                r.dissent_max_ulp = std::max(r.dissent_max_ulp, c.max_ulp_diff);
                r.dissent_max_rel = std::max(r.dissent_max_rel, c.max_rel_diff);
            }
        }
    };

    const auto collect = [&](bool agreeing) {
        for (std::size_t ci = 0; ci < clusters.size(); ++ci) {
            if ((ci == best) != agreeing) {
                continue;
            }
            for (const std::size_t idx : clusters[ci]) {
                (agreeing ? r.agreeing : r.dissenting).push_back(submissions[idx].worker);
            }
        }
    };

    // 1. Enough agreement — accept.
    if (best_size >= cfg.required_agreement) {
        r.action = QuorumAction::Accept;
        r.agreeing_max_ulp = cluster_ulp[best];
        collect(true);
        collect(false);
        measure_dissent();
        if (!r.dissenting.empty()) {
            r.detail = "accepted " + std::to_string(best_size) + "/" +
                       std::to_string(n) + " with " +
                       std::to_string(r.dissenting.size()) + " dissenting";
        }
        return r;
    }

    // 2. Room left — ask someone else.
    if (n < cfg.max_replicas) {
        r.action = QuorumAction::NeedMoreReplicas;
        if (clusters.size() > 1) {
            r.detail = "disagreement: " + std::to_string(clusters.size()) +
                       " distinct answers from " + std::to_string(n) + " workers";
        }
        return r;
    }

    // 3. At the cap. A STRICT majority still decides.
    if (best_size * 2 > submissions.size()) {
        r.action = QuorumAction::Accept;
        r.agreeing_max_ulp = cluster_ulp[best];
        collect(true);
        collect(false);
        measure_dissent();
        r.detail = "majority " + std::to_string(best_size) + "/" + std::to_string(n) +
                   " at replica cap";
        return r;
    }

    // 4. At the cap with no majority. NOT "pick the biggest" — a 2-2 split is
    // no evidence, and accepting half of it would put an unvalidated answer in
    // the output while reporting it as validated. Re-run with fresh workers.
    r.action = QuorumAction::Inconclusive;
    r.detail = "no majority at cap: " + std::to_string(clusters.size()) +
               " distinct answers from " + std::to_string(n) + " workers";
    return r;
}

std::uint32_t RequiredAgreementFor(const QuorumConfig& cfg, const ReputationTable& rep,
                                   WorkerId worker, std::uint64_t now_ms) {
    switch (cfg.policy) {
        case ReplicationPolicy::None:
            return 1;
        case ReplicationPolicy::Fixed2x:
            // E4's control condition: everyone, always, regardless of record.
            // This is the number the adaptive policy has to beat (3.6).
            return cfg.required_agreement;
        case ReplicationPolicy::Adaptive:
            break;
    }

    // Non-const lookup would create an entry for every worker asked about;
    // `ScoreOf` returns the prior for an unknown one instead, which is the
    // right answer and allocates nothing.
    const Reputation* r = rep.Find(worker);
    const bool probation = r != nullptr &&
                           (r->on_probation || r->blacklisted_until_ms > now_ms);
    if (probation) {
        // The worker most worth checking is the one we just stopped trusting.
        return cfg.max_replicas;
    }
    if (rep.ScoreOf(worker) >= rep.config().trusted_at) {
        // No replication. THE point of 3.8 — a long correct record is the
        // evidence, and a Beta score means a single lucky result cannot get
        // here (D-0055).
        return 1;
    }
    return cfg.required_agreement;
}

}  // namespace p2pgpu::coordinator

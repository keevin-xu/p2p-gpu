// Worker reputation — steps 3.7-3.10. See reputation.hpp.

#include "p2pgpu/coordinator/reputation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <sstream>

namespace p2pgpu::coordinator {

double SeverityFromDeviation(std::uint32_t max_ulp, double max_rel) {
    // A near miss is a near miss. 0.16 measured 5 ULP between Apple Metal and
    // NVIDIA D3D12 on honest hardware, so anything in that neighbourhood must
    // sit at the floor — otherwise reputation re-introduces exactly the
    // cross-vendor rejection R6 exists to prevent, just more slowly.
    if (max_ulp <= 16) {
        return 0.25;
    }
    if (!std::isfinite(max_rel)) {
        // NaN or inf against a finite answer. Not a disagreement about
        // rounding; the kernel produced something structurally wrong.
        return 4.0;
    }
    // Log-scaled in relative error, so "wrong in the 6th digit" and "wrong by a
    // factor of a million" are meaningfully different numbers rather than both
    // saturating.
    const double rel = std::max(max_rel, 1e-9);
    const double decades = std::log10(rel) + 9.0;   // 1e-9 -> 0, 1.0 -> 9
    return std::clamp(0.5 + decades * 0.4, 0.5, 4.0);
}

std::string ReputationTable::MintToken(WorkerId id) {
    // std::random_device, not mt19937 seeded from the clock. A token derived
    // from a predictable seed is guessable, and a guessable token hands an
    // attacker somebody else's reputation (D-0056).
    //
    // 128 bits: enough that guessing is not a strategy, and short enough to sit
    // in a string field without comment.
    static std::random_device rd;
    std::uniform_int_distribution<std::uint64_t> dist;
    const std::uint64_t hi = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    const std::uint64_t lo = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();

    std::ostringstream os;
    os << std::hex << hi << lo;
    std::string token = os.str();

    // Replace any previous token for this worker, so an old one cannot be
    // reused after a reconnect issued a new one.
    if (const auto it = tokens_.find(id); it != tokens_.end()) {
        by_token_.erase(it->second);
    }
    by_token_[token] = id;
    tokens_[id] = token;
    return token;
}

std::optional<WorkerId> ReputationTable::ResolveToken(const std::string& token) const {
    if (token.empty()) {
        return std::nullopt;
    }
    const auto it = by_token_.find(token);
    return it == by_token_.end() ? std::nullopt : std::optional<WorkerId>{it->second};
}

void ReputationTable::Adopt(WorkerId id, const Reputation& rep,
                            const std::string& token) {
    workers_[id] = rep;
    if (!token.empty()) {
        by_token_[token] = id;
        tokens_[id] = token;
    }
}

const Reputation* ReputationTable::Find(WorkerId id) const {
    const auto it = workers_.find(id);
    return it == workers_.end() ? nullptr : &it->second;
}

Reputation& ReputationTable::Mutable(WorkerId id) {
    const auto it = workers_.find(id);
    if (it != workers_.end()) {
        return it->second;
    }
    Reputation r;
    r.alpha = cfg_.alpha0;
    r.beta = cfg_.beta0;
    return workers_.emplace(id, r).first->second;
}

double ReputationTable::ScoreOf(WorkerId id) const {
    const Reputation* r = Find(id);
    // An unknown worker scores the prior rather than 0. Scoring it 0 would make
    // every newcomer look like a liar, and 3.8 would replicate the entire
    // incoming fleet at maximum forever.
    return r != nullptr ? r->score() : cfg_.alpha0 / (cfg_.alpha0 + cfg_.beta0);
}

void ReputationTable::RecordAccepted(WorkerId id, std::uint32_t deviation_ulp) {
    Reputation& r = Mutable(id);
    ++r.accepted;
    // FULL credit regardless of deviation. Within tolerance IS correct (R6):
    // a cross-vendor worker agreeing at 5 ULP did nothing worse than one
    // agreeing bitwise, and giving it less credit would slowly push honest
    // NVIDIA workers toward the blacklist for running on NVIDIA.
    (void)deviation_ulp;
    r.alpha += 1.0;

    // Probation ends by being right, not by waiting.
    if (r.on_probation && r.score() >= cfg_.trusted_at) {
        r.on_probation = false;
    }
}

void ReputationTable::RecordRejected(WorkerId id, double severity, bool spot_check) {
    Reputation& r = Mutable(id);
    ++r.rejected;
    if (spot_check) {
        // Stronger evidence than losing a vote: there is no honest
        // disagreement with an answer we already hold, so a failed spot-check
        // is doubled.
        ++r.spot_check_failures;
        severity *= 2.0;
    }
    r.beta += std::clamp(severity, 0.0, cfg_.max_penalty);
}

bool ReputationTable::MaybeBlacklist(WorkerId id, std::uint64_t now_ms) {
    Reputation& r = Mutable(id);
    if (r.blacklisted_until_ms > now_ms) {
        return false;   // already serving one
    }
    if (r.score() >= cfg_.blacklist_below) {
        return false;
    }
    r.blacklisted_until_ms = now_ms + cfg_.cooldown_ms;
    return true;
}

bool ReputationTable::IsBlacklisted(WorkerId id, std::uint64_t now_ms) {
    Reputation& r = Mutable(id);
    if (r.blacklisted_until_ms == 0) {
        return false;
    }
    if (now_ms < r.blacklisted_until_ms) {
        return true;
    }
    // Cooldown served. PROBATION, not a clean slate: the record stands, and the
    // worker is replicated at maximum until it earns its score back. Wiping the
    // history instead would let a liar launder its record by waiting.
    r.blacklisted_until_ms = 0;
    r.on_probation = true;
    return false;
}

}  // namespace p2pgpu::coordinator

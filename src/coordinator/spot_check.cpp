// Spot-checking — step 3.9. See spot_check.hpp.

#include "p2pgpu/coordinator/spot_check.hpp"

#include <algorithm>

namespace p2pgpu::coordinator {
namespace {
/// Cap on remembered answers. A coordinator with a long uptime would otherwise
/// accumulate one per validated task forever, and the pool's value does not
/// grow with size — a few hundred ranges is already unpredictable to a worker.
constexpr std::size_t kMaxKnown = 256;
}  // namespace

void SpotCheckPool::Remember(std::uint64_t start_unit, std::uint64_t unit_count,
                             std::uint64_t checksum) {
    if (unit_count == 0) {
        return;
    }
    // Ranges must be unique: remembering the same one repeatedly would bias
    // every injection toward it, and a liar that saw the same range twice would
    // learn it is being tested.
    const auto same = [&](const Known& k) {
        return k.start_unit == start_unit && k.unit_count == unit_count;
    };
    if (std::ranges::any_of(known_, same)) {
        return;
    }
    if (known_.size() >= kMaxKnown) {
        // Drop the OLDEST. Keeping the oldest instead would make the pool go
        // stale, and a fixed set of test ranges is one a determined worker
        // could eventually recognise.
        known_.erase(known_.begin());
    }
    known_.push_back(Known{start_unit, unit_count, checksum});
}

std::optional<SpotCheckPool::Pick> SpotCheckPool::Maybe(double worker_score,
                                                        double roll,
                                                        std::uint64_t index_roll) const {
    if (known_.empty()) {
        return std::nullopt;
    }
    // Unproven workers are tested more, because that is where the information
    // is — but the trusted rate is deliberately non-zero, or becoming trusted
    // would be a permanent licence to start lying.
    const double rate =
        worker_score >= cfg_.proven_at ? cfg_.rate_trusted : cfg_.rate_unproven;
    if (roll >= rate) {
        return std::nullopt;
    }
    const Known& k = known_[index_roll % known_.size()];
    return Pick{k.start_unit, k.unit_count, k.checksum};
}

void SpotCheckPool::MarkIssued(TaskId task, std::uint64_t expected_checksum) {
    issued_[task] = expected_checksum;
}

std::optional<std::uint64_t> SpotCheckPool::ExpectedFor(TaskId task) const {
    const auto it = issued_.find(task);
    return it == issued_.end() ? std::nullopt : std::optional<std::uint64_t>{it->second};
}

void SpotCheckPool::Forget(TaskId task) { issued_.erase(task); }

}  // namespace p2pgpu::coordinator

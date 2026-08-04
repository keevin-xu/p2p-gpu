#include "p2pgpu/coordinator/affinity.hpp"

namespace p2pgpu::coordinator {

std::optional<std::size_t> PreferCached(
    std::span<const std::optional<AssetId>> needs,
    std::span<const AssetId> cached) noexcept {
    // Cheap rejection first. Both spans are empty in every call the system
    // makes today (D-0047), so this is the only line that usually runs.
    if (cached.empty()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < needs.size(); ++i) {
        // FIRST match, not best. The queue is already ordered by risk (D-0043 —
        // requeued work goes first because it is what gets forgotten at the
        // tail of a job), and affinity chooses within that order rather than
        // overriding it. Picking the "most affine" task would quietly reorder
        // work by a criterion that has nothing to do with getting the job done.
        if (needs[i].has_value() && HasAsset(cached, *needs[i])) {
            return i;
        }
    }
    return std::nullopt;
}

}  // namespace p2pgpu::coordinator

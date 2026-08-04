#pragma once
//
// Cache-affinity assignment — step 2.16.
//
// Prefer handing a worker a task whose bulk input it already holds. In Phase 6
// assets move peer-to-peer and re-fetching a multi-megabyte input to run a
// two-second task is the difference between a grid that scales and one that
// spends its bandwidth re-sending things.
//
// ── INERT BY DATA, NOT BY CODE (D-0047) ──────────────────────────────────
// No job has an `input_ref` yet and no worker has a cache, so the preferred set
// is always empty and the queue order is untouched. The code path is REAL and
// tested — a stub that is never taken cannot be distinguished from one that is
// broken, which is the `never_renews_lease` mistake this project already made
// once.

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace p2pgpu::coordinator {

/// A bulk input asset, by content hash (BLAKE3-256).
///
/// Coordinator-local on purpose. `input_ref`'s wire shape is still speculative
/// and belongs to Phase 6, and D-0028 is a standing reminder that hash-shaped
/// things on the wire are where the alignment bugs came from. Nothing here is
/// serialized.
using AssetId = std::array<std::byte, 32>;

/// True if `want` is one of the assets `cached` already holds.
///
/// Linear, and deliberately so: a worker's cache is a handful of entries, and a
/// hash set for four elements costs more than it saves.
[[nodiscard]] inline bool HasAsset(std::span<const AssetId> cached,
                                   const AssetId& want) noexcept {
    for (const AssetId& have : cached) {
        if (have == want) {
            return true;
        }
    }
    return false;
}

/// Index into `needs` of the first entry this worker already has cached.
///
/// `needs[i]` is the asset required by queue entry `i`, absent if that task has
/// no bulk input. Returns nullopt when nothing matches — which is every call
/// today, and is why the caller must fall back to plain queue order rather than
/// treating "no affinity" as "no work".
[[nodiscard]] std::optional<std::size_t> PreferCached(
    std::span<const std::optional<AssetId>> needs,
    std::span<const AssetId> cached) noexcept;

}  // namespace p2pgpu::coordinator

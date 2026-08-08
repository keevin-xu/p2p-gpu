#pragma once
//
// Content-addressed asset store — step 5.4.
//
// ── THE FALLBACK PATH PHASE 6 SITS IN FRONT OF (D-0007) ──────────────────
// Workers fetch bulk inputs from here today. Phase 6 puts a peer-to-peer plane
// ahead of it and this becomes the path taken when no peer has the bytes. That
// is why the interface is content-addressed rather than "get the scene": a peer
// and the coordinator must be interchangeable sources, and the only way that is
// safe is if the NAME OF THE THING IS ITS HASH (D-0069).
//
// ── THE KEY IS ATTACKER-CONTROLLED ───────────────────────────────────────
// `{hash}` arrives from the network. It is validated as exactly 64 lowercase
// hex characters BEFORE it is used for anything — R11's "validate every length
// field against its MAX before allocating", applied to a lookup key. It never
// reaches a filesystem path; this store is in-memory precisely so that a path
// traversal is not expressible.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace p2pgpu::coordinator {

class AssetStore {
public:
    /// Store `bytes` under its own BLAKE3 address and return that address.
    ///
    /// Idempotent by construction: the same bytes always land on the same key,
    /// so registering a scene twice costs one entry. That property is what
    /// makes the cache on the worker side meaningful too.
    std::string Put(std::vector<std::byte> bytes);

    /// Look up by content address. Returns nullptr when `hash` is absent OR
    /// malformed — the caller must not care which, and must not tell the peer
    /// which either (`IsWellFormedAddress` distinguishing them would leak
    /// whether an asset exists).
    [[nodiscard]] const std::vector<std::byte>* Find(std::string_view hash) const;

    /// Exactly 64 lowercase hex characters, and nothing else.
    ///
    /// Deliberately strict about CASE. An address is produced by exactly one
    /// function, which emits lowercase, so accepting uppercase would create two
    /// spellings of one asset — two cache entries on every worker, two peer
    /// advertisements, and a cache-affinity hit rate (5.18) quietly measuring
    /// the wrong thing.
    [[nodiscard]] static bool IsWellFormedAddress(std::string_view hash) noexcept;

    [[nodiscard]] std::size_t count() const noexcept { return blobs_.size(); }
    [[nodiscard]] std::uint64_t total_bytes() const noexcept { return total_bytes_; }

    /// Asset bytes this coordinator has SERVED — E6's headline number (6.13).
    ///
    /// Measured here rather than derived from worker telemetry, because that is
    /// the whole point: `TaskStats.asset_source` is what workers CLAIM
    /// (invariant 8), and this is what the coordinator KNOWS. A fleet reporting
    /// peer fetches while this number grows linearly is contradicted by it.
    ///
    /// Counted on both paths — `GET /asset/{hash}` and `AssetChunk` over the
    /// control link — because a measurement that missed one would report a
    /// saving that came from workers switching transports rather than from
    /// peers serving each other.
    void RecordServed(std::uint64_t bytes) noexcept { bytes_served_ += bytes; }
    [[nodiscard]] std::uint64_t bytes_served() const noexcept { return bytes_served_; }

private:
    /// `std::less<>` so a `string_view` looks up without allocating a string —
    /// this runs on the event-loop thread for every asset request.
    std::map<std::string, std::vector<std::byte>, std::less<>> blobs_;
    std::uint64_t total_bytes_ = 0;
    std::uint64_t bytes_served_ = 0;
};

}  // namespace p2pgpu::coordinator

#pragma once
//
// Worker-side content-addressed asset cache — steps 5.4 / 5.5.
//
// ── VERIFY BEFORE USE, WITHOUT EXCEPTION (R11, D-0069) ───────────────────
// `Get` recomputes BLAKE3 over every fetched blob and compares it to the
// address that was asked for. Bytes that do not hash to their own name are
// dropped and never cached, never parsed, never uploaded to a GPU.
//
// Today the bytes come from the coordinator, which is ALREADY not trusted
// (R11). In Phase 6 they come from an arbitrary peer, and this check is the
// only thing standing between a hostile peer and the traversal loop. Building
// it now rather than retrofitting it is what 5.5 asks for — and it is cheap to
// get right here and expensive to add once a data plane depends on the
// behaviour.
//
// ── THE FETCHER IS INJECTED, SO worker-core STAYS PORTABLE (R2) ──────────
// Native reads over HTTP or from disk; the browser uses `emscripten_wget_data`.
// Both live in the thin per-target wrapper, exactly as `KernelFetcher` does
// (1.23). No `#ifdef __EMSCRIPTEN__` reaches this file.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace p2pgpu::worker {

/// Fetch the bytes named by a content address. `std::nullopt` on any failure —
/// this returns what it was given, and makes NO promise the bytes are correct.
/// Checking that is `AssetCache`'s job and must not be duplicated in a
/// platform-specific file where only one of the two targets would get it.
using AssetFetcher =
    std::function<std::optional<std::vector<std::byte>>(std::string_view address)>;

class AssetCache {
public:
    /// `max_bytes` bounds resident assets. R11: a coordinator (or peer) that
    /// can name unlimited assets must not be able to grow a worker's memory
    /// without limit.
    AssetCache(AssetFetcher fetch, std::uint64_t max_bytes);

    /// Return the bytes at `address`, fetching them if absent.
    ///
    /// **Returns nullptr unless the bytes hash to `address`.** A caller may
    /// treat a non-null return as verified; that guarantee is the entire
    /// product of this class.
    [[nodiscard]] const std::vector<std::byte>* Get(std::string_view address);

    /// Already resident and verified. Feeds `Hello.cached_assets`, which is
    /// what 2.16's affinity and 5.18's hit-rate measurement read.
    [[nodiscard]] bool Has(std::string_view address) const;

    /// Addresses currently held, for `Hello`. Sorted, so two workers reporting
    /// the same set report it identically.
    [[nodiscard]] std::vector<std::string> cached() const;

    /// Bytes that failed verification. Non-zero here is the signal a peer is
    /// serving corrupt data (Phase 6) — counted rather than merely logged so
    /// the fleet can be asked about it.
    [[nodiscard]] std::uint32_t rejected_count() const noexcept { return rejected_; }
    [[nodiscard]] std::uint64_t resident_bytes() const noexcept { return resident_; }

    /// Drop everything. Used when a device is lost and buffers must be rebuilt.
    void Clear();

private:
    void EvictUntilRoomFor(std::uint64_t incoming);

    AssetFetcher fetch_;
    std::uint64_t max_bytes_;
    std::uint64_t resident_ = 0;
    std::uint32_t rejected_ = 0;
    /// Insertion counter per entry, so eviction is oldest-first without a
    /// second container. A worker holds a handful of assets, not thousands.
    std::uint64_t tick_ = 0;
    struct Entry {
        std::vector<std::byte> bytes;
        std::uint64_t inserted_at = 0;
    };
    std::map<std::string, Entry, std::less<>> entries_;
};

}  // namespace p2pgpu::worker

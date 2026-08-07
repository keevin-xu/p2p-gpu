// Worker-side content-addressed asset cache — steps 5.4 / 5.5.
//
// PORTABLE. The fetcher is injected (see the header), so nothing here knows
// whether it is running natively or in a browser (R2).

#include "p2pgpu/worker/asset_cache.hpp"

#include <algorithm>
#include <utility>

#include "p2pgpu/worker/checksum.hpp"
#include "p2pgpu/worker/platform.hpp"

namespace p2pgpu::worker {

AssetCache::AssetCache(AssetFetcher fetch, std::uint64_t max_bytes)
    : fetch_(std::move(fetch)), max_bytes_(max_bytes) {}

bool AssetCache::Has(std::string_view address) const {
    return entries_.find(address) != entries_.end();
}

std::vector<std::string> AssetCache::cached() const {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const auto& [address, entry] : entries_) {
        out.push_back(address);
    }
    // Already sorted — std::map iterates in key order — but stated rather than
    // relied on implicitly, since `Hello.cached_assets` is compared across
    // workers by the coordinator's affinity check.
    return out;
}

void AssetCache::Clear() {
    entries_.clear();
    resident_ = 0;
}

void AssetCache::EvictUntilRoomFor(std::uint64_t incoming) {
    while (resident_ + incoming > max_bytes_ && !entries_.empty()) {
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.inserted_at < oldest->second.inserted_at) {
                oldest = it;
            }
        }
        resident_ -= oldest->second.bytes.size();
        entries_.erase(oldest);
    }
}

const std::vector<std::byte>* AssetCache::Get(std::string_view address) {
    if (const auto it = entries_.find(address); it != entries_.end()) {
        return &it->second.bytes;
    }
    if (!fetch_) {
        return nullptr;
    }

    auto bytes = fetch_(address);
    if (!bytes) {
        return nullptr;
    }

    // ── THE CHECK THIS CLASS EXISTS FOR ──────────────────────────────────
    // Recomputed over the bytes we actually received, compared against the
    // address we actually asked for. Not a checksum the sender supplied — that
    // would only prove the sender can hash, which a hostile peer certainly can.
    //
    // Note this also catches an HONEST failure that is otherwise miserable to
    // diagnose: a truncated download, or a proxy that helpfully "fixed" the
    // content type and mangled the bytes. Both present as a corrupt render
    // with nothing in any log naming the transfer.
    const std::string actual = Blake3Hex(*bytes);
    if (actual != address) {
        ++rejected_;
        platform::Log("error",
                      "asset failed verification and was DISCARDED; expected " +
                          std::string(address) + " got " + actual);
        return nullptr;
    }

    const auto size = static_cast<std::uint64_t>(bytes->size());
    if (size > max_bytes_) {
        // Bigger than the whole budget: evicting everything would still not
        // fit, so refuse rather than empty the cache for nothing.
        platform::Log("error", "asset exceeds the entire cache budget; refusing");
        return nullptr;
    }
    EvictUntilRoomFor(size);

    const auto [it, inserted] =
        entries_.emplace(std::string(address), Entry{std::move(*bytes), tick_++});
    resident_ += it->second.bytes.size();
    return &it->second.bytes;
}

}  // namespace p2pgpu::worker

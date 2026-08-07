// Content-addressed asset store — step 5.4.

#include "p2pgpu/coordinator/assets.hpp"

#include "p2pgpu/scene/bvh.hpp"

namespace p2pgpu::coordinator {

std::string AssetStore::Put(std::vector<std::byte> bytes) {
    std::string address = scene::ContentAddress(bytes);
    const auto [it, inserted] = blobs_.emplace(address, std::move(bytes));
    if (inserted) {
        total_bytes_ += it->second.size();
    }
    // Not inserted means the identical bytes were already here — the defining
    // property of content addressing, not a collision to worry about.
    return address;
}

bool AssetStore::IsWellFormedAddress(std::string_view hash) noexcept {
    if (hash.size() != 64) {
        return false;
    }
    for (const char c : hash) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower_hex = c >= 'a' && c <= 'f';
        if (!digit && !lower_hex) {
            return false;
        }
    }
    return true;
}

const std::vector<std::byte>* AssetStore::Find(std::string_view hash) const {
    // Shape first, ALWAYS. A malformed key can never reach the container, so
    // no amount of creativity in the request can express anything but a
    // 64-hex-character lookup (R11).
    if (!IsWellFormedAddress(hash)) {
        return nullptr;
    }
    const auto it = blobs_.find(hash);
    return it == blobs_.end() ? nullptr : &it->second;
}

}  // namespace p2pgpu::coordinator

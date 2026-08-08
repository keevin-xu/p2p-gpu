// Worker liveness — step 2.8. See fleet.hpp.

#include "p2pgpu/coordinator/fleet.hpp"

#include <algorithm>

namespace p2pgpu::coordinator {

void Fleet::Join(WorkerId id, std::uint64_t conn_id, std::uint64_t now_ms) {
    WorkerRecord r;
    r.id = id;
    r.conn_id = conn_id;
    r.last_seen_ms = now_ms;
    workers_[id] = r;
}

void Fleet::Leave(WorkerId id) { workers_.erase(id); }

void Fleet::Touch(WorkerId id, std::uint64_t now_ms) {
    const auto it = workers_.find(id);
    if (it != workers_.end()) {
        it->second.last_seen_ms = now_ms;
    }
}

std::vector<WorkerId> Fleet::FindLost(std::uint64_t now_ms,
                                      std::uint32_t timeout_ms) const {
    std::vector<WorkerId> lost;
    for (const auto& [id, rec] : workers_) {
        // Subtract in this direction, not `now - last_seen > timeout`: both are
        // unsigned, and a clock that appears to go backwards (or a record
        // stamped a moment in the future) would underflow into a colossal
        // elapsed time and declare a live worker dead.
        if (rec.last_seen_ms + timeout_ms < now_ms) {
            lost.push_back(id);
        }
    }
    return lost;
}

const WorkerRecord* Fleet::Find(WorkerId id) const noexcept {
    const auto it = workers_.find(id);
    return it == workers_.end() ? nullptr : &it->second;
}

WorkerRecord* Fleet::Mutable(WorkerId id) noexcept {
    const auto it = workers_.find(id);
    return it == workers_.end() ? nullptr : &it->second;
}

void Fleet::RecordCompletion(WorkerId id) {
    const auto it = workers_.find(id);
    if (it != workers_.end()) {
        ++it->second.tasks_completed;
    }
}

std::vector<WorkerId> Fleet::PeersHolding(const AssetId& asset, WorkerId exclude,
                                          std::size_t max) const {
    std::vector<WorkerId> out;
    if (max == 0) {
        return out;
    }
    for (const auto& [id, rec] : workers_) {
        if (id == exclude) {
            continue;
        }
        if (HasAsset(rec.cached_assets, asset)) {
            out.push_back(id);
        }
    }
    // Sorted BEFORE truncating, so the cap takes a deterministic prefix rather
    // than whatever the hash map happened to iterate first. Truncating an
    // unordered set would make the list vary between identical runs.
    std::ranges::sort(out, [](WorkerId a, WorkerId b) {
        return a.hi() != b.hi() ? a.hi() < b.hi() : a.lo() < b.lo();
    });
    if (out.size() > max) {
        out.resize(max);
    }
    return out;
}

}  // namespace p2pgpu::coordinator

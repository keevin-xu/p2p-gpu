#pragma once
//
// Who is connected, and when we last heard from them — step 2.8.
//
// ── WHY THIS IS SEPARATE FROM JobManager ─────────────────────────────────
// `JobManager` owns work. This owns *workers*. Merging them would mean every
// question about a task's lease had to reason about connection state and vice
// versa, and the two have genuinely different lifetimes: a task outlives the
// worker holding it (that is the whole point of leasing), and a worker outlives
// any particular task.
//
// ── LIVENESS IS DECIDED HERE, ON OUR CLOCK ───────────────────────────────
// Never on a worker's self-report. `PROTOCOL.md` §5 is explicit that worker
// clocks are not trusted, and a worker that claims to be alive is exactly what
// a hung worker also does. What counts is when WE last received a frame.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "p2pgpu/protocol/ids.hpp"

namespace p2pgpu::coordinator {

using protocol::WorkerId;

struct WorkerRecord {
    WorkerId id;
    std::uint64_t conn_id = 0;
    /// Coordinator clock, absolute. Updated on EVERY inbound frame, not only on
    /// an explicit heartbeat — a worker sending results is self-evidently alive,
    /// and requiring a separate keepalive from a busy worker would be a way to
    /// declare our fastest contributors dead.
    std::uint64_t last_seen_ms = 0;
    std::uint32_t tasks_completed = 0;
};

class Fleet {
public:
    void Join(WorkerId id, std::uint64_t conn_id, std::uint64_t now_ms);
    void Leave(WorkerId id);

    /// Called on every inbound frame from this worker.
    void Touch(WorkerId id, std::uint64_t now_ms);

    /// Workers not heard from in `timeout_ms`. The caller releases their leases
    /// and removes them — this only reports, so the policy stays in one place.
    ///
    /// Missing a heartbeat is NOT a fault. R8: absence is not malice, and a
    /// worker that reconnects later is simply a new worker.
    [[nodiscard]] std::vector<WorkerId> FindLost(std::uint64_t now_ms,
                                                 std::uint32_t timeout_ms) const;

    [[nodiscard]] const WorkerRecord* Find(WorkerId id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return workers_.size(); }
    void RecordCompletion(WorkerId id);

private:
    std::unordered_map<WorkerId, WorkerRecord> workers_;
};

}  // namespace p2pgpu::coordinator

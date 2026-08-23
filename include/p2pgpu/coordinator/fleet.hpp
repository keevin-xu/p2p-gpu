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
#include <string>
#include <unordered_map>
#include <vector>

#include "p2pgpu/coordinator/affinity.hpp"
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

    // ── Time-to-ready (6.16) ─────────────────────────────────────────────
    /// When this worker joined, and when its first result arrived. Both on OUR
    /// clock, for the 2.21 reason: a worker reporting its own readiness is
    /// telemetry it chooses (invariant 8), and time-to-ready is exactly the
    /// number a worker would want to look good on.
    ///
    /// `first_result_ms` stays 0 until a result actually lands. A worker that
    /// joined and never produced anything is NOT ready, and recording a
    /// readiness time for it would let the slowest joiners vanish from the
    /// statistic — which is the direction that flatters the result.
    /// Adapter identity, purely for the RECORD (D-0101). Never keyed on for
    /// scheduling or validation — these strings mean different things per
    /// implementation and may be empty (RISKS.md §1).
    ///
    /// Stored because it was already arriving in `Hello` and being thrown
    /// away, which made a real measurement unusable: the `r4-load` capture of
    /// 2026-08-11 cannot be attributed to a machine, so a possibly-valid
    /// Windows result had to be scored as not-met. A run that cannot say which
    /// GPU produced it is not evidence.
    std::string adapter_vendor;
    std::string adapter_device;
    std::string adapter_backend;

    std::uint64_t joined_ms = 0;
    std::uint64_t first_grant_ms = 0;
    std::uint64_t first_result_ms = 0;

    // ── Sizing state (2.11-2.14) ─────────────────────────────────────────
    /// Device throughput in **arithmetic ops per second**, from the join-time
    /// benchmark. NOT units/sec: a worker cannot know which kernel it will be
    /// given, so it reports a device-level number and the sizer divides by the
    /// target kernel's `flop_per_unit` from the manifest.
    ///
    /// 0 until the worker reports one, and until then it gets no work — a
    /// fabricated score would propagate into every future grant through the
    /// correction factor below.
    double score_ops_per_sec = 0.0;

    /// EWMA of actual/predicted duration. 1.0 = the benchmark has been
    /// accurate. >1 = tasks take longer than predicted, so grant less.
    double correction = 1.0;

    /// User-set, 0.0-1.0 (R7). Applied WITHOUT argument — the user's setting is
    /// authoritative and the coordinator does not get an opinion.
    double throttle = 1.0;

    /// What we predicted for the task currently in flight, and when it was
    /// granted. Both on OUR clock: the worker reports a duration in TaskStats
    /// and that number is untrusted telemetry (invariant 8), so the correction
    /// factor is computed from what we observed, not what we were told.
    /// Work this worker has actually completed, measured on OUR clock (2.21).
    ///
    /// Never from `TaskStats.gpu_ms`. A worker that under-reports its duration
    /// would look faster than it is and be granted ever-larger tasks, which is
    /// a way to be handed the whole keyspace by lying (invariant 8).
    std::uint64_t units_completed = 0;
    double observed_ms_total = 0.0;

    double predicted_ms = 0.0;
    std::uint64_t granted_at_ms = 0;

    /// Tasks this worker must stop working on (2.17). Queued here because the
    /// winner is on a DIFFERENT connection, and a Session can only reply on its
    /// own socket — so a revoke rides along on this worker's next reply.
    ///
    /// Latency is bounded by the renewal interval (~3 s), which is acceptable:
    /// the cost of a late revoke is a little wasted compute, and the worker's
    /// result is discarded silently when it arrives (2.10).
    std::vector<protocol::TaskId> pending_revokes;

    /// Bulk inputs this worker already holds (2.16). Empty until Phase 6
    /// distributes assets; `Grant` prefers tasks needing one of these.
    ///
    /// NOT yet populated from anything the worker says. A self-reported cache
    /// is unverifiable, and a worker claiming to hold every asset would be
    /// preferred for everything (R11) — Phase 6 owns making the claim
    /// checkable.
    std::vector<AssetId> cached_assets;
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

    /// Read-only view for metrics (2.21). Const so an observer cannot become a
    /// mutator by accident — the dashboard reports state, it never changes it.
    [[nodiscard]] const std::unordered_map<WorkerId, WorkerRecord>& All() const noexcept {
        return workers_;
    }
    void RecordCompletion(WorkerId id);

    /// Mutable access for the sizing state. Deliberately narrow — everything
    /// else about a record is set through the methods above.
    /// Workers believed to hold `asset`, excluding `exclude`, at most `max`
    /// of them (6.2, D-0086).
    ///
    /// "Believed" is exact: this reads what workers ADVERTISED on their last
    /// `LeaseRequest` (D-0079). A peer may have departed or been lying since.
    /// That is safe because the list is a hint — the requester verifies BLAKE3
    /// against the address it asked for and falls back to the coordinator, so a
    /// wrong entry costs a retry and never a wrong answer.
    ///
    /// Sorted by worker id, lowest first. Not fairness — REPLAYABILITY: 6.13
    /// compares runs, and a list that varied between identical runs would make
    /// two measurements incomparable for a reason unrelated to the experiment.
    [[nodiscard]] std::vector<WorkerId> PeersHolding(const AssetId& asset,
                                                     WorkerId exclude,
                                                     std::size_t max) const;

    [[nodiscard]] WorkerRecord* Mutable(WorkerId id) noexcept;

private:
    std::unordered_map<WorkerId, WorkerRecord> workers_;
};

}  // namespace p2pgpu::coordinator

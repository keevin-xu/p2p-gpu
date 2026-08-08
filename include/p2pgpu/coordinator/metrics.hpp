#pragma once
//
// Metrics — step 2.21.
//
// ── THIS IS AN OBSERVATION, NOT A SOURCE OF TRUTH ────────────────────────
// Every number here is DERIVED, on demand, from `JobManager` and `Fleet`. There
// is no separate counter that a code path has to remember to increment, because
// a counter maintained beside the state it describes is a counter that
// eventually disagrees with it — and a dashboard that disagrees with the
// scheduler is worse than no dashboard, since it will be believed.
//
// The cost is a linear scan per snapshot. At one snapshot per second against
// tens of thousands of tasks that is nothing, and it buys the guarantee that
// what is displayed is what the coordinator actually thinks.
//
// ── WHAT THE HISTOGRAM IS FOR ────────────────────────────────────────────
// `prediction_error` is the 2.13 correction factor observed across the fleet.
// 2.26 plots its convergence; if it does not converge, that is a finding worth
// a DECISIONS entry rather than a quiet retune of alpha.

#include <cstdint>
#include <string>
#include <vector>

#include "p2pgpu/coordinator/fleet.hpp"
#include "p2pgpu/coordinator/job.hpp"

namespace p2pgpu::coordinator {

/// One worker's row in the fleet table.
struct WorkerMetrics {
    std::uint64_t worker_id = 0;
    std::uint32_t tasks_completed = 0;
    double score_ops_per_sec = 0.0;
    /// EWMA of actual/predicted duration (2.13). 1.0 means the join-time
    /// benchmark has been accurate for this worker.
    double correction = 1.0;
    double throttle = 1.0;
    /// Units per second, from OUR observations — never the worker's
    /// self-reported `gpu_ms`, which is telemetry a worker chooses
    /// (invariant 8).
    double observed_units_per_sec = 0.0;
};

/// Everything the dashboard shows, at one instant.
struct Snapshot {
    std::uint64_t timestamp_ms = 0;

    // Fleet
    std::size_t workers = 0;

    // Queue and tasks
    std::size_t queue_depth = 0;
    std::size_t total_tasks = 0;
    /// Indexed by `TaskState`. A vector rather than named fields so adding a
    /// state cannot leave a counter silently absent from the display.
    std::vector<std::size_t> tasks_by_state;

    // Work
    std::uint64_t units_total = 0;
    std::uint64_t units_remaining = 0;
    /// Units in cancelled speculative replicas (2.17). An UPPER BOUND on what
    /// speculation cost — a revoked worker stops early, so the real figure is
    /// lower, and E5 owns measuring the difference.
    std::uint64_t wasted_units = 0;

    /// Cache affinity (5.18). Two counts, not a ratio — a rate alone hides
    /// whether it came from 3 grants or 30,000.
    std::uint64_t asset_grants = 0;
    std::uint64_t asset_grants_hit = 0;

    /// Fetch-source breakdown (6.11/6.14). UNTRUSTED telemetry — a worker
    /// chooses what to report (invariant 8). Credible only because the
    /// coordinator knows how many bytes IT served: a fleet claiming peer
    /// fetches while coordinator egress stays flat is claiming something the
    /// coordinator can contradict.
    std::uint64_t asset_from_peer = 0;
    std::uint64_t asset_from_coordinator = 0;
    std::uint64_t asset_from_cache = 0;

    /// **E6's headline number** (6.13): asset bytes the coordinator actually
    /// sent, over both transports. What the coordinator KNOWS, as against what
    /// workers claim.
    std::uint64_t coordinator_asset_egress = 0;

    // Hygiene, deliberately separate from task reputation (3.11) — conflating
    // them is how an honest-but-buggy client gets blacklisted.
    std::uint64_t rejected_frames = 0;

    std::vector<WorkerMetrics> fleet;
};

/// Build a snapshot. Pure: same inputs, same output, no clock of its own —
/// `now_ms` is passed so a test can pin it.
[[nodiscard]] Snapshot Collect(const JobManager& jobs, const Fleet& fleet,
                               std::uint64_t rejected_frames, std::uint64_t now_ms);

/// Serialize to JSON for the SSE feed (2.21) and the dashboard (2.22).
///
/// Hand-rolled rather than pulling in a JSON library: the output shape is fixed
/// and entirely ours, there is no parsing (this is write-only), and the one
/// genuinely dangerous part — a worker-influenced string reaching the page — is
/// avoided because **every field below is a number**. If a string field is ever
/// added here it must be escaped, and 2.22's dashboard must keep treating it as
/// text rather than markup.
[[nodiscard]] std::string ToJson(const Snapshot& snap);

}  // namespace p2pgpu::coordinator

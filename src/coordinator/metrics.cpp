// coordinator/metrics — step 2.21. See metrics.hpp.
// The coordinator is the ONLY component that makes decisions (rule R1).
// No unwrap-equivalent: never crash on worker input (docs/CONVENTIONS.md §1).

#include "p2pgpu/coordinator/metrics.hpp"

#include <array>
#include <cmath>
#include <cstdio>

namespace p2pgpu::coordinator {
namespace {

/// Format a double for JSON.
///
/// Non-finite values become `null` rather than `nan`/`inf`, which are not JSON
/// and would break the dashboard's parse for the whole document — not just the
/// offending field. A worker cannot inject one directly (scores are validated
/// on arrival), but `observed_units_per_sec` is a division and a zero
/// denominator is one arithmetic accident away.
std::string Num(double v) {
    if (!std::isfinite(v)) {
        return "null";
    }
    std::array<char, 32> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), "%.6g", v);
    return n > 0 ? std::string(buf.data(), static_cast<std::size_t>(n)) : "null";
}

}  // namespace

Snapshot Collect(const JobManager& jobs, const Fleet& fleet,
                 std::uint64_t rejected_frames, std::uint64_t now_ms) {
    Snapshot snap;
    snap.timestamp_ms = now_ms;
    snap.workers = fleet.size();
    snap.queue_depth = jobs.queued();
    snap.total_tasks = jobs.total_tasks();
    snap.tasks_by_state = jobs.CountByState();
    snap.units_total = jobs.total_units();
    snap.units_remaining = jobs.remaining_units();
    snap.wasted_units = jobs.wasted_units();
    // 5.18. Reported as the two counts rather than a ratio: a rate alone hides
    // whether it came from 3 grants or 30,000.
    snap.asset_grants = jobs.asset_grants();
    snap.asset_grants_hit = jobs.asset_grants_hit();
    const auto srcs = jobs.asset_sources();
    snap.asset_from_peer = srcs.peer;
    snap.asset_from_coordinator = srcs.coordinator;
    snap.asset_from_cache = srcs.cached;
    snap.rejected_frames = rejected_frames;

    snap.fleet.reserve(fleet.size());
    for (const auto& [id, rec] : fleet.All()) {
        WorkerMetrics wm;
        wm.worker_id = id.hi();
        wm.tasks_completed = rec.tasks_completed;
        wm.score_ops_per_sec = rec.score_ops_per_sec;
        wm.correction = rec.correction;
        wm.throttle = rec.throttle;
        // Guarded rather than trusted: a worker that has completed nothing has
        // no observed time, and 0/0 would put a NaN on the dashboard.
        wm.observed_units_per_sec =
            rec.observed_ms_total > 0.0 ? 1000.0 * static_cast<double>(rec.units_completed) /
                                              rec.observed_ms_total
                                        : 0.0;
        snap.fleet.push_back(wm);
    }
    return snap;
}

std::string ToJson(const Snapshot& snap) {
    std::string out = "{";
    out += "\"timestamp_ms\":" + std::to_string(snap.timestamp_ms);
    out += ",\"workers\":" + std::to_string(snap.workers);
    out += ",\"queue_depth\":" + std::to_string(snap.queue_depth);
    out += ",\"total_tasks\":" + std::to_string(snap.total_tasks);
    out += ",\"units_total\":" + std::to_string(snap.units_total);
    out += ",\"units_remaining\":" + std::to_string(snap.units_remaining);
    out += ",\"wasted_units\":" + std::to_string(snap.wasted_units);
    out += ",\"asset_grants\":" + std::to_string(snap.asset_grants);
    out += ",\"asset_grants_hit\":" + std::to_string(snap.asset_grants_hit);
    out += ",\"asset_from_peer\":" + std::to_string(snap.asset_from_peer);
    out += ",\"asset_from_coordinator\":" + std::to_string(snap.asset_from_coordinator);
    out += ",\"asset_from_cache\":" + std::to_string(snap.asset_from_cache);
    out += ",\"coordinator_asset_egress\":" + std::to_string(snap.coordinator_asset_egress);
    out += ",\"rejected_frames\":" + std::to_string(snap.rejected_frames);

    out += ",\"tasks_by_state\":[";
    for (std::size_t i = 0; i < snap.tasks_by_state.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += std::to_string(snap.tasks_by_state[i]);
    }
    out += "]";

    // Names travel WITH the counts, from the same enum the scheduler switches
    // on, so the dashboard cannot drift out of sync by hardcoding a stale list.
    out += ",\"state_names\":[";
    for (std::size_t i = 0; i < snap.tasks_by_state.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += "\"";
        out += ToString(static_cast<TaskState>(i));
        out += "\"";
    }
    out += "]";

    out += ",\"fleet\":[";
    for (std::size_t i = 0; i < snap.fleet.size(); ++i) {
        const WorkerMetrics& w = snap.fleet[i];
        if (i > 0) {
            out += ",";
        }
        out += "{\"worker_id\":" + std::to_string(w.worker_id);
        out += ",\"tasks_completed\":" + std::to_string(w.tasks_completed);
        out += ",\"score_ops_per_sec\":" + Num(w.score_ops_per_sec);
        out += ",\"correction\":" + Num(w.correction);
        out += ",\"throttle\":" + Num(w.throttle);
        out += ",\"observed_units_per_sec\":" + Num(w.observed_units_per_sec);
        out += "}";
    }
    out += "]}";
    return out;
}

}  // namespace p2pgpu::coordinator

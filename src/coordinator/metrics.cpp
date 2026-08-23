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

/// JSON string literal from an ATTACKER-CONTROLLED label (D-0101).
///
/// Adapter strings come off the wire. Emitting one raw lets a worker close the
/// string and inject arbitrary JSON into every dashboard and every saved
/// metrics snapshot — the same class as the length checks in R11, applied to
/// output instead of input. Quotes and backslashes are escaped, control
/// characters are dropped, and anything above ASCII is dropped rather than
/// half-encoded: a valid document with a mangled GPU name beats an invalid one.
std::string Quote(const std::string& v) {
    std::string out = "\"";
    for (const char c : v) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (u >= 0x20 && u < 0x7F) {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
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
        wm.adapter_vendor = rec.adapter_vendor;
        wm.adapter_device = rec.adapter_device;
        wm.adapter_backend = rec.adapter_backend;
        if (rec.first_grant_ms != 0) {
            wm.ms_to_first_grant =
                static_cast<double>(rec.first_grant_ms - rec.joined_ms);
        }
        if (rec.first_result_ms != 0) {
            wm.ms_to_first_result =
                static_cast<double>(rec.first_result_ms - rec.joined_ms);
        }
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
        out += ",\"adapter_vendor\":" + Quote(w.adapter_vendor);
        out += ",\"adapter_device\":" + Quote(w.adapter_device);
        out += ",\"adapter_backend\":" + Quote(w.adapter_backend);
        out += ",\"ms_to_first_grant\":" + Num(w.ms_to_first_grant);
        out += ",\"ms_to_first_result\":" + Num(w.ms_to_first_result);
        out += "}";
    }
    out += "]}";
    return out;
}

}  // namespace p2pgpu::coordinator

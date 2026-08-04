#pragma once
//
// Experiment event log — steps 2.23-2.26. THE DELIVERABLE'S RAW DATA.
//
// One CSV row per scheduling event, on the coordinator's clock. E1 (scaling),
// E3 (fault tolerance), E5 (stragglers) and 2.26 (sizing convergence) are all
// computed OFFLINE from this one file rather than from four bespoke harnesses —
// four instruments measuring one system is four chances for them to disagree,
// and the disagreement would surface as a contradiction between plots nobody
// could resolve after the fact.
//
// ── EVERYTHING HERE IS OUR OBSERVATION, NOT A WORKER'S REPORT ────────────
// `duration_ms` is grant-to-accept measured on the coordinator's clock, never
// `TaskStats.gpu_ms` (invariant 8). A worker that under-reports its duration
// would otherwise be able to make the throughput curve say whatever it likes.
//
// ── DEV-ONLY, LIKE --verify-reference ────────────────────────────────────
// Null unless `--events-csv` is given. A coordinator serving real volunteers
// does not write a row per task to disk.

#include <cstdint>
#include <cstdio>
#include <string>

#include "p2pgpu/protocol/error.hpp"
#include "p2pgpu/protocol/ids.hpp"

namespace p2pgpu::coordinator {

/// Appends CSV rows. Buffered by the C runtime and flushed on close; a crash
/// mid-experiment loses the tail, which is acceptable because an experiment
/// that crashed is not evidence anyway.
class EventLog {
public:
    [[nodiscard]] static protocol::Result<std::unique_ptr<EventLog>> Open(
        const std::string& path);

    ~EventLog();
    EventLog(const EventLog&) = delete;
    EventLog& operator=(const EventLog&) = delete;

    /// A task was handed to a worker.
    void Grant(std::uint64_t t_ms, protocol::TaskId task, protocol::WorkerId worker,
               std::uint64_t unit_count, double predicted_ms, bool speculative);

    /// A result was accepted. `duration_ms` is grant-to-accept on OUR clock.
    void Accept(std::uint64_t t_ms, protocol::TaskId task, protocol::WorkerId worker,
                std::uint64_t unit_count, double duration_ms, double predicted_ms,
                double correction);

    /// A lease expired and the task was requeued (R8 — not a fault).
    void Expire(std::uint64_t t_ms, protocol::TaskId task, protocol::WorkerId worker,
                std::uint64_t unit_count);

    /// A speculative replica lost its race and was cancelled (D-0045).
    void Cancel(std::uint64_t t_ms, protocol::TaskId task, protocol::WorkerId worker,
                std::uint64_t unit_count);

    void WorkerJoin(std::uint64_t t_ms, protocol::WorkerId worker);
    void WorkerLost(std::uint64_t t_ms, protocol::WorkerId worker);

private:
    explicit EventLog(std::FILE* f) : f_(f) {}
    void Row(std::uint64_t t_ms, const char* event, std::uint64_t task,
             std::uint64_t worker, std::uint64_t unit_count, double duration_ms,
             double predicted_ms, double correction, int speculative);

    std::FILE* f_ = nullptr;
};

}  // namespace p2pgpu::coordinator

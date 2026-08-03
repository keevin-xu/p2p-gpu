#pragma once
//
// One simulated worker — steps 2.1 and 2.2.
//
// A FULL protocol client that never touches a GPU. N of these share one
// process and one event loop, which is what makes a 200-worker fleet a
// 30-second test run instead of a week of machines.
//
// ── IT COMPUTES THE REAL ANSWER ──────────────────────────────────────────
// Honest workers run `kernels::BruteSearchReference` — the same CPU ground
// truth step 1.25 built — and then SLEEP for their simulated duration.
//
// The obvious alternative, canned result bytes, quietly destroys Phase 3: if
// every mock returns the same fabricated payload then "honest" and "lying" are
// indistinguishable, and `lies_probabilistically` is a deviation from nothing.
// Replication and quorum cannot be measured against a fleet where nobody is
// right (D-0042).
//
// Consequence worth stating: task size is bounded by CPU feasibility, so these
// experiments measure SCHEDULING and never GPU throughput. Nothing from this
// harness is throughput evidence.

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "behaviors.hpp"
#include "p2pgpu/protocol/ids.hpp"
#include "p2pgpu/transport/transport.hpp"

namespace p2pgpu::mock {

// Transport still lives in p2pgpu::worker — it was extracted into its own
// library (D-0042) without renaming the namespace, since the real worker is
// still its primary consumer and a rename would touch more than it is worth.
using worker::Transport;

struct WorkerStats {
    std::uint32_t tasks_completed = 0;
    std::uint32_t tasks_lied_about = 0;
    std::uint32_t tasks_abandoned = 0;
    std::uint32_t reconnects = 0;
    std::uint32_t malformed_sent = 0;
};

class VirtualWorker {
public:
    VirtualWorker(std::uint32_t index, std::string url, Behaviors behaviors,
                  std::uint64_t run_seed);
    ~VirtualWorker();

    VirtualWorker(const VirtualWorker&) = delete;
    VirtualWorker& operator=(const VirtualWorker&) = delete;

    void Start();
    void Stop();

    /// Drive one iteration. Non-blocking, like the real worker's Poll() — the
    /// fleet loop calls this for every worker in turn, so one slow worker
    /// cannot stall the others.
    void Poll();

    [[nodiscard]] const WorkerStats& stats() const noexcept { return stats_; }
    [[nodiscard]] bool connected() const noexcept { return connected_; }
    [[nodiscard]] std::uint32_t index() const noexcept { return index_; }

private:
    void OnFrame(std::span<const std::byte> bytes);
    void SendHello();
    void RequestLease();
    void SendMalformed();

    struct Pending {
        protocol::TaskId id;
        std::vector<std::byte> params;
        std::uint64_t start_unit = 0;
        std::uint64_t work_units = 0;
        std::uint32_t output_bytes = 0;
        std::chrono::steady_clock::time_point finish_at;
        std::vector<std::byte> result;   // computed at grant, sent at finish_at
    };

    void BeginTask(Pending task);
    void FinishTask(const Pending& task);

    std::uint32_t index_;
    std::string url_;
    Behaviors behaviors_;
    Dice dice_;
    Transport transport_;
    WorkerStats stats_;

    std::mutex inbox_mutex_;
    std::deque<std::vector<std::byte>> inbox_;

    bool running_ = false;
    bool connected_ = false;
    bool handshaked_ = false;
    bool lease_outstanding_ = false;
    std::deque<Pending> in_flight_;
    std::chrono::steady_clock::time_point next_action_;
};

}  // namespace p2pgpu::mock

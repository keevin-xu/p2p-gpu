#pragma once
//
// The worker's task loop — steps 1.20 and 1.21. PORTABLE, no platform
// conditionals (R2); tools/check_seam.py enforces that.
//
// Lease -> resolve inputs -> execute -> report -> renew.
//
// ── R1 IS THE WHOLE DESIGN HERE ──────────────────────────────────────────
// This class applies coordinator policy; it never computes it. It does not
// decide which task to run, how big a task should be, whether a result is
// correct, or whether a peer is trustworthy. It asks whether there is work,
// does exactly the work it is handed, and reports facts. Every branch below
// that looks like a decision is either a hard rule the worker must satisfy
// locally (R4 chunking, R7 consent) or a report of a local fact.
//
// ── WHY POLL() RATHER THAN CALLBACKS ALL THE WAY DOWN ────────────────────
// Transport callbacks arrive on a libdatachannel thread natively and on the JS
// event loop in the browser. Running GPU work directly from them would mean two
// genuinely different concurrency stories for the two targets — which is the
// drift R2 exists to prevent. So callbacks only ENQUEUE, and all work happens
// in Poll(), which the host calls from its own loop. One story, both targets.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "p2pgpu/protocol/ids.hpp"
#include "p2pgpu/worker/kernel_host.hpp"
#include "p2pgpu/worker/recovery.hpp"
#include "p2pgpu/transport/transport.hpp"

namespace p2pgpu::worker {

/// Supplies WGSL for a kernel id.
///
/// Named KernelFetcher rather than KernelSource because smoke.hpp already uses
/// the latter, in this same namespace, for the step 0.9 fixture struct.
///
/// Deliberately a callback rather than something worker-core does itself:
/// native reads from disk, the browser fetches over HTTP from the coordinator
/// (step 1.12). Keeping acquisition out of here is what stops an
/// `#ifdef __EMSCRIPTEN__` from appearing in portable code (R2).
///
/// Returns nullopt if the kernel is unavailable; the loop declines the task
/// rather than guessing.
using KernelFetcher = std::function<std::optional<std::string>(std::string_view kernel_id)>;

struct TaskLoopConfig {
    std::string coordinator_url = "ws://localhost:8080/ws";

    /// Bounds ONE dispatch, so no single dispatch approaches the ~2 s at which
    /// Windows TDR resets the driver (R4). A LOCAL execution detail, not
    /// scheduling — see RunTask. The default is deliberately conservative;
    /// step 2.11's benchmark replaces it with a measured number.
    std::uint64_t units_per_chunk = 1u << 20;

    /// How many tasks to ask for at once. A hint the coordinator may ignore.
    std::uint32_t max_tasks_in_flight = 1;
};

/// What the UI shows and the user controls (R7). Read-only from outside.
struct WorkerStatus {
    bool connected = false;
    bool contributing = false;   ///< actually running GPU work right now
    bool device_ready = false;
    std::uint32_t tasks_completed = 0;
    std::uint32_t tasks_failed = 0;
    std::uint32_t device_recoveries = 0;
    std::string last_message;
};

class TaskLoop {
public:
    TaskLoop(TaskLoopConfig config, DeviceSession& device, KernelFetcher kernels);
    ~TaskLoop();

    TaskLoop(const TaskLoop&) = delete;
    TaskLoop& operator=(const TaskLoop&) = delete;

    /// Connect and begin. **Only ever called from an affirmative user action**
    /// (R7) — worker-browser wires this to a button, and there is no path that
    /// reaches it otherwise.
    void Start();

    /// Instant stop (R7). Releases every held lease, closes the socket, and
    /// stops asking for work.
    ///
    /// "Instant" is a promise to the user, so this must not wait for the
    /// current dispatch: it cannot cancel work already submitted, but chunking
    /// bounds that to one chunk (~250 ms at most), and nothing further is
    /// submitted. That bound is another thing R4's chunking buys.
    void Stop();

    /// Drive one iteration. Call from the host's loop; never blocks.
    void Poll();

    /// User-set throttle, 0.0–1.0 (R7). 1.0 is full speed; 0.0 stops asking for
    /// new work without disconnecting.
    ///
    /// Implemented as duty cycle between tasks rather than by shrinking tasks:
    /// task size is the coordinator's (R1), and a worker that silently returned
    /// less work than it was granted would look like a slow liar.
    ///
    /// The ONLY method safe to call from a thread other than the one running
    /// Poll(). The browser sets it from a slider on the main thread while the
    /// loop runs on a Web Worker (D-0037); `throttle_` is atomic for that
    /// reason, and nothing else here is.
    void SetThrottle(float fraction);
    [[nodiscard]] float throttle() const noexcept { return throttle_.load(); }

    [[nodiscard]] const WorkerStatus& status() const noexcept { return status_; }

private:
    // Inbound
    void OnFrame(std::span<const std::byte> bytes);
    void HandleWelcome(const wire::Welcome& welcome);
    void HandleTaskGrant(const wire::TaskGrant& grant);
    void HandleRevoke(const wire::Revoke& revoke);
    void HandleShutdown();
    void HandleError(const wire::Error& error);

    // Outbound
    void SendHello();
    void RequestLease();
    void SendResult(protocol::TaskId task, const TaskOutcome& outcome);
    void SendRelease(protocol::TaskId task, wire::ReleaseReason reason);

    /// Device loss: release leases FIRST, then re-acquire, then re-register
    /// (step 1.21). Backwards means the coordinator waits out a lease on work
    /// this worker can no longer do, turning a 200 ms hiccup into a
    /// lease-duration outage.
    void OnDeviceLost();
    void OnDeviceReady();

    struct PendingTask {
        protocol::TaskId id;
        protocol::JobId job;
        std::string kernel_id;
        std::vector<std::byte> params;
        std::uint64_t start_unit = 0;
        std::uint64_t work_units = 0;
        std::uint32_t output_bytes = 0;
        std::uint32_t workgroup_size = 64;
    };

    /// Runs one task to completion. Returns false if it could not be run at all
    /// — in which case the lease is RELEASED rather than left to expire (R8:
    /// the coordinator should get the work back immediately, and a voluntary
    /// give-back carries no penalty).
    [[nodiscard]] bool Execute(const PendingTask& task);

    TaskLoopConfig config_;
    DeviceSession& device_;
    KernelFetcher kernels_;
    Transport transport_;

    /// Inbound frames, filled by the transport callback and drained by Poll().
    /// The mutex is genuinely needed natively (libdatachannel has its own
    /// threads) and is uncontended in the browser — one code path, both targets.
    std::mutex inbox_mutex_;
    std::deque<std::vector<std::byte>> inbox_;

    std::deque<PendingTask> queue_;
    std::vector<protocol::TaskId> held_;

    protocol::WorkerId worker_id_;
    bool running_ = false;
    bool handshaked_ = false;
    bool lease_outstanding_ = false;
    std::atomic<float> throttle_{1.0F};
    WorkerStatus status_;

    /// Kernel descriptors from Welcome, keyed by id. The coordinator is the
    /// authority on entry point and workgroup size; the worker never guesses.
    struct KernelInfo {
        std::string entry_point;
        std::uint32_t workgroup_size = 64;
        std::uint32_t output_bytes = 0;
        /// Bytes to write into the result buffer once per task, from the
        /// coordinator (D-0040). Empty means zero-fill. NOT optional in
        /// practice: a kernel whose reduction has a non-zero identity returns
        /// silently wrong results without it.
        std::vector<std::byte> output_init;
    };
    std::vector<std::pair<std::string, KernelInfo>> kernel_info_;

    [[nodiscard]] const KernelInfo* FindKernel(std::string_view id) const;
};

}  // namespace p2pgpu::worker

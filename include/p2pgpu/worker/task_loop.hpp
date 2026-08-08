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
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "p2pgpu/protocol/ids.hpp"
#include "p2pgpu/transport/peer_link.hpp"
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
    /// scheduling — see RunTask.
    ///
    /// **A FALLBACK ONLY.** Once the join-time benchmark has run, the worker
    /// replaces this with a size derived from its own measured throughput. A
    /// fixed value cannot be right for both targets: at an M4 Pro's ~3e10
    /// units/sec this default is ~0.03 ms of work, and browser submissions cost
    /// ~5 ms each (D-0026), so a large task became tens of thousands of chunks
    /// that were ~99.99% overhead and expired before finishing (D-0044).
    std::uint64_t units_per_chunk = 1u << 20;

    /// How many tasks to ask for at once. A hint the coordinator may ignore.
    std::uint32_t max_tasks_in_flight = 1;
};

/// What the UI shows and the user controls (R7). Read-only from outside.
struct WorkerStatus {
    bool connected = false;
    bool contributing = false;   ///< actually running GPU work right now
    bool device_ready = false;
    /// The GPU is gone and will not come back in this document (D-0065).
    /// TERMINAL: `Start()` will not resume from it, and the UI offers a reload
    /// rather than pretending a retry might help. Distinct from
    /// `!device_ready`, which is the recoverable case — conflating the two is
    /// what made a dead tab indistinguishable from a briefly stalled one.
    bool gpu_unavailable = false;
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
    /// Step 2.11. Runs the calibration kernel and reports achieved throughput.
    ///
    /// The score is **arithmetic ops per second**, NOT tasks or candidates per
    /// second. The worker cannot know which kernel it will be given, so it
    /// reports a device-level number and the COORDINATOR divides by the target
    /// kernel's `flop_per_unit` from the manifest. Reporting units/sec here
    /// would silently mean "units of whatever I happened to benchmark".
    void HandleBenchmarkRequest(const wire::BenchmarkRequest& request);
    void HandleShutdown();
    /// Candidate peers holding the asset this worker needs (6.2). Stored only;
    /// 6.4 is what connects to them.
    void HandlePeerList(const wire::PeerList& list);
    void HandleError(const wire::Error& error);

    // Outbound
    void SendHello();
    void RequestLease();
    void SendProgress(protocol::TaskId task, float fraction);
    void SendResult(protocol::TaskId task, const TaskOutcome& outcome);
    /// Upload progress WITHOUT finishing the task (5.13, D-0074).
    ///
    /// Carries the full accumulator so far, not a delta — the coordinator must
    /// not need to know the payload's arithmetic (R1). Keeps the lease, and the
    /// upload itself renews it, so an accumulating worker never needs a separate
    /// heartbeat.
    void SendPartialResult(protocol::TaskId task, const TaskOutcome& outcome,
                           std::uint32_t sequence, std::uint64_t units_done);
    void SendResultFrame(protocol::TaskId task, const TaskOutcome& outcome,
                         std::uint32_t sequence, std::uint64_t units_done);
    void SendAssetRequest(const std::string& address, std::uint32_t from,
                          std::uint32_t to);
    void SendRelease(protocol::TaskId task, wire::ReleaseReason reason);
    /// Clean departure: "I am done, do not wait for me." Sent when the GPU is
    /// permanently gone (D-0065) — the coordinator already handles `Goodbye`,
    /// and telling it beats making it infer our death from a lease timeout.
    void SendGoodbye(wire::ReleaseReason reason);

    /// Device loss: release leases FIRST, then re-acquire, then re-register
    /// (step 1.21). Backwards means the coordinator waits out a lease on work
    /// this worker can no longer do, turning a 200 ms hiccup into a
    /// lease-duration outage.
    void OnDeviceLost();
    void OnDeviceReady();
    /// Recovery gave up (D-0065). Leases were already released by OnDeviceLost;
    /// this says goodbye and stops the loop for good.
    void OnDeviceUnrecoverable();

    struct PendingTask {
        protocol::TaskId id;
        /// Content address of the bulk input this task reads, empty if none.
        std::string asset;
        protocol::JobId job;
        std::string kernel_id;
        std::vector<std::byte> params;
        std::uint64_t start_unit = 0;
        std::uint64_t work_units = 0;
        std::uint32_t output_bytes = 0;
        std::uint32_t workgroup_size = 64;
        std::uint32_t workgroup_size_y = 1;
        /// Invocation grid, when a unit is not an invocation (D-0073). Zero
        /// keeps the one-invocation-per-unit default.
        std::uint32_t invocations_x = 0;
        std::uint32_t invocations_y = 1;
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

    /// Resume token from the last `Welcome` (3.13). In memory only — neither
    /// target has secret storage, and losing it costs nothing but a fresh
    /// identity.
    std::string resume_token_;
    bool running_ = false;
    bool handshaked_ = false;
    /// A lease request is in flight. Cleared by a grant — OR by the timeout
    /// below, because the coordinator answers "no work" with SILENCE.
    ///
    /// Without the timeout a worker that asks while the queue is momentarily
    /// empty never asks again: it stays quiet, is declared lost after the
    /// heartbeat window, and its capacity is gone for the rest of the run.
    bool lease_outstanding_ = false;
    std::chrono::steady_clock::time_point lease_requested_at_{};

    /// Consecutive empty replies. Drives jittered exponential backoff (2.15) so
    /// a 200-worker fleet does not synchronise into a thundering herd against
    /// an empty queue (RISKS.md §2).
    // ── Bulk assets over the control link (5.16, D-0077) ─────────────────

    /// An asset being reassembled. One at a time: a worker renders one job, so
    /// concurrent fetches would be a cache-thrashing pattern with no caller.
    struct AssetFetch {
        std::string address;
        std::vector<std::byte> bytes;
        std::vector<bool> have;      ///< per chunk, so a duplicate is idempotent
        std::uint32_t total = 0;
        /// The coordinator's authoritative byte length (D-0091). A peer cannot
        /// influence it, so a claimed chunk count can be REJECTED rather than
        /// believed.
        std::uint64_t expected_bytes = 0;
        std::uint32_t received = 0;
        std::chrono::steady_clock::time_point started{};
        std::chrono::steady_clock::time_point last_progress{};
    };
    std::optional<AssetFetch> fetch_;

    /// Peers the coordinator believes hold the asset we are fetching (6.2).
    ///
    /// A HINT. A listed peer may have departed, may refuse, or may serve corrupt
    /// bytes — all safe, because whatever arrives is verified against the
    /// address we asked for and the coordinator remains the fallback (D-0007).
    /// Bounded on receipt: a worker that opened a connection per listed entry
    /// would be one malicious frame from the mesh 6.2 exists to avoid.
    std::vector<protocol::WorkerId> peer_candidates_;
    /// Which candidate we are currently attempting. Advances on failure; when
    /// it passes the end, or the budget is spent, we fall back.
    std::size_t peer_attempt_ = 0;
    /// The peer we are negotiating with, nil when not attempting one.
    protocol::WorkerId peer_target_{};
    /// Deadline for the WHOLE peer phase.
    ///
    /// Timeout-based, not failure-only (6.9): a merely SLOW peer must not block
    /// a task, and waiting for a TCP-like failure signal that may never come is
    /// how a fetch hangs for minutes. The coordinator is always there.
    std::chrono::steady_clock::time_point peer_deadline_{};

    /// STUN/TURN URLs from `Welcome` (6.5). Bounded on receipt — an unbounded
    /// list would be an allocation the coordinator chooses, and it is not more
    /// trusted than any peer (R11).
    std::vector<std::string> ice_servers_;

    /// Tasks waiting on an asset. PARKED, not released: releasing returns the
    /// task to the queue where this same worker is likely to be granted it
    /// again and re-request the same asset — a loop that looks like progress
    /// (D-0077).
    std::vector<PendingTask> waiting_on_asset_;

    /// Validated, GPU-ready views of the resident asset. Rebuilt when a new
    /// asset arrives, and kept alive for as long as tasks reference them.
    /// The raw asset blob we hold, kept so we can SERVE it to peers (6.6).
    /// The parsed arrays below are for our own GPU; a peer wants the bytes.
    std::vector<std::byte> resident_bytes_;
    /// The one peer connection, if any (6.4). One at a time: a worker fetching
    /// one asset needs one source, and the connection cap is a scheduling
    /// question (6.12) that does not belong in the fetch path.
    std::unique_ptr<transport::PeerLink> peer_;

    std::vector<std::byte> asset_nodes_;
    std::vector<std::byte> asset_prims_;
    std::vector<std::byte> asset_materials_;
    std::string resident_asset_;

    void RequestAssetChunks();
    void HandleAssetChunk(const wire::AssetChunk& chunk);
    void HandleAssetMiss(const wire::AssetMiss& miss);

    // ── Peer data plane (6.6) ────────────────────────────────────────────

    /// Bytes off a DataChannel. Verified as an `AssetMsg` — a DIFFERENT root
    /// from the control link's `Envelope`, so a peer cannot express a control
    /// message at all (D-0090). Routed into the SAME reassembly the coordinator
    /// path uses: one implementation, two transports, so invariant 10 cannot be
    /// enforced on one and forgotten on the other.
    void OnPeerBytes(std::span<const std::byte> bytes);

    // ── Peer fetch lifecycle (6.8 / 6.9) ─────────────────────────────────

    /// Try the next candidate peer, or give up and use the coordinator.
    ///
    /// PEERS FIRST, coordinator as the fallback (D-0007) — that ordering is the
    /// whole point of the data plane, and the fallback is what makes it safe to
    /// try something unreliable first.
    void TryNextPeerOrFallBack();
    /// Abandon the peer attempt and fetch from the coordinator instead.
    void FallBackToCoordinator(const char* why);
    /// Route a relayed `Signal` into the peer connection.
    void HandleSignal(const wire::Signal& signal);
    /// Send one signalling message to the peer we are negotiating with.
    void SendSignal(protocol::WorkerId to, const transport::SignalOut& out);

    /// Serve a peer's `AssetRequest` from what we hold.
    void ServePeerAssetRequest(const wire::AssetRequest& req);

    /// Send an `AssetMsg` to the connected peer, if any.
    void SendToPeer(const std::vector<std::byte>& msg);
    /// Verify, validate, and publish a fully-received asset. Releases every
    /// parked task on failure — the bytes are wrong and re-requesting them
    /// from the same source would produce the same wrong bytes.
    void FinishAssetFetch();
    /// Give up on a stalled fetch and release what was waiting on it.
    void AbandonAssetFetch(const char* why);
    std::uint32_t empty_replies_ = 0;
    /// Earliest time the next lease request may go out, under backoff.
    std::chrono::steady_clock::time_point next_action_{};
    std::atomic<float> throttle_{1.0F};
    WorkerStatus status_;

    /// Kernel descriptors from Welcome, keyed by id. The coordinator is the
    /// authority on entry point and workgroup size; the worker never guesses.
    struct KernelInfo {
        std::string entry_point;
        std::uint32_t workgroup_size = 64;
        /// The y dimension. On the wire since 1.11 and DISCARDED until 5.16 —
        /// every kernel before the path tracer was 1D, so `@workgroup_size(8,8,1)`
        /// was silently read as (8,1,1) and the dispatch covered an eighth of
        /// the grid.
        std::uint32_t workgroup_size_y = 1;
        std::uint32_t output_bytes = 0;
        /// Arithmetic ops per work unit, from the manifest via Welcome. Needed
        /// to turn a device-level ops/sec figure into a chunk size in units.
        std::uint64_t flop_per_unit = 0;
        /// Bytes to write into the result buffer once per task, from the
        /// coordinator (D-0040). Empty means zero-fill. NOT optional in
        /// practice: a kernel whose reduction has a non-zero identity returns
        /// silently wrong results without it.
        std::vector<std::byte> output_init;
    };
    std::vector<std::pair<std::string, KernelInfo>> kernel_info_;

    /// Measured device throughput in arithmetic ops/sec, from our own benchmark.
    /// Drives chunk sizing; 0 until the benchmark has run.
    double measured_ops_per_sec_ = 0.0;

    /// Chunk size for the kernel being run, derived from the two numbers above.
    /// Falls back to `config_.units_per_chunk` when either is unknown.
    [[nodiscard]] std::uint64_t ChunkUnitsFor(const KernelInfo& info) const;

    [[nodiscard]] const KernelInfo* FindKernel(std::string_view id) const;
};

}  // namespace p2pgpu::worker

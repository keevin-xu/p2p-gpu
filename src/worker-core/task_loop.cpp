// Portable. NO #ifdef permitted in this file (rule R2) — tools/check_seam.py
// fails the build if one appears.
//
// Lease -> resolve inputs -> execute -> report -> renew.
// Applies coordinator policy; never computes it (rule R1).
//
// Steps 1.20 (loop) and 1.21 (device-loss wiring). See task_loop.hpp for why
// transport callbacks only enqueue and all work happens in Poll().

#include "p2pgpu/worker/task_loop.hpp"

#include <algorithm>
#include <deque>
#include <utility>

#include "p2pgpu/protocol/encode.hpp"
#include "p2pgpu/worker/bench.hpp"
#include "p2pgpu/worker/checksum.hpp"
#include "p2pgpu/protocol/invariants.hpp"
#include "p2pgpu/protocol/limits.hpp"
#include "p2pgpu/protocol/verify.hpp"
#include "p2pgpu/protocol/limits.hpp"
#include "p2pgpu/scene/bvh.hpp"

namespace p2pgpu::worker {
namespace {
/// Chunks asked for per request. Matches the coordinator's per-request cap, so
/// a full batch is one round trip rather than a partial reply plus a retry.
constexpr std::uint32_t kAssetChunkRequestBatch = 64;
/// Give up if no chunk arrives for this long. A stalled fetch must not pin a
/// task forever — the coordinator can hand it to someone else.
constexpr auto kAssetStallTimeout = std::chrono::seconds(20);

/// How long the WHOLE peer phase gets before falling back (6.9).
///
/// Timeout-based, NOT failure-only: ICE can take many seconds to admit defeat,
/// and a merely slow peer must not block a task that the coordinator could have
/// served in that time. Short enough that a failed attempt costs less than the
/// fetch it replaced.
constexpr auto kPeerPhaseTimeout = std::chrono::seconds(8);

/// Candidate peers tried before giving up on the data plane for this asset.
///
/// TWO, not eight. "No retry storms" (6.8): cycling every listed candidate with
/// a timeout each turns one slow fetch into a minute of nothing, and the
/// coordinator would have finished long before. The peer list is an
/// optimisation, and an optimisation that delays the work it optimises is a
/// regression.
constexpr std::size_t kMaxPeerAttempts = 2;
}  // namespace

namespace {

using platform::Log;
using Clock = std::chrono::steady_clock;

}  // namespace

TaskLoop::TaskLoop(TaskLoopConfig config, DeviceSession& device, KernelFetcher kernels)
    // Transport takes a log sink rather than calling platform::Log itself, so
    // the library stays GPU-free (D-0042). Passing it here keeps browser
    // console routing exactly as it was.
    : config_(std::move(config)), device_(device), kernels_(std::move(kernels)),
      transport_(platform::Log) {
    // Wired here, not in Start(), so a loss during connection is still handled.
    device_.OnLost([this] { OnDeviceLost(); });
    device_.OnReady([this] { OnDeviceReady(); });
    device_.OnUnrecoverable([this] { OnDeviceUnrecoverable(); });
}

TaskLoop::~TaskLoop() {
    // Same shape as the mock's (4.14): `transport_` is declared before
    // `inbox_mutex_`/`inbox_`, so reverse-declaration order destroys the mutex
    // and the queue while the socket is still alive — and the transport
    // callback locks that mutex from libdatachannel's thread.
    //
    // Locking a destroyed mutex is undefined behaviour, not a benign race, so
    // this is worse in the real worker than in the mock. TSan found it in the
    // mock because that is what the harness runs; the exposure here is
    // identical and was fixed at the same time rather than waiting for a
    // report from a browser.
    transport_.Shutdown();
}

void TaskLoop::Start() {
    if (running_) {
        return;
    }
    // A terminal GPU loss (D-0065) is not something restarting can fix — the
    // adapter is dead for the life of this document. Reconnecting would give
    // the coordinator a worker that accepts leases and computes nothing, which
    // is strictly worse than the tab staying gone.
    if (status_.gpu_unavailable) {
        Log("warn", "GPU is permanently unavailable in this page; reload to rejoin");
        return;
    }
    running_ = true;
    status_.last_message = "connecting";

    transport_.OnOpen([this] {
        status_.connected = true;
        SendHello();
    });
    transport_.OnClosed([this] {
        status_.connected = false;
        handshaked_ = false;
        lease_outstanding_ = false;
        // Leases are the coordinator's to reclaim once the socket is gone —
        // it already releases them on disconnect. Dropping our local record
        // stops us reporting results for tasks we no longer hold.
        held_.clear();
        queue_.clear();
        status_.last_message = "disconnected";
    });
    transport_.OnError([this](const std::string& e) { status_.last_message = "error: " + e; });
    transport_.OnMessage([this](std::span<const std::byte> bytes) {
        // ENQUEUE ONLY. This runs on a libdatachannel thread natively and on
        // the JS event loop in the browser; doing real work here would give the
        // two targets different concurrency stories (R2).
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        inbox_.emplace_back(bytes.begin(), bytes.end());
    });

    transport_.Connect(config_.coordinator_url);
}

void TaskLoop::Stop() {
    if (!running_) {
        return;
    }
    running_ = false;

    // Give back everything held BEFORE closing, so the coordinator can requeue
    // immediately rather than waiting out the leases. Voluntary release carries
    // no reputation penalty (R8) — stopping is a right, not a fault.
    for (const protocol::TaskId task : held_) {
        SendRelease(task, wire::ReleaseReason::UserStopped);
    }
    held_.clear();
    queue_.clear();

    transport_.Close();
    status_.connected = false;
    status_.contributing = false;
    status_.last_message = "stopped";
    Log("info", "worker stopped by user");
}

void TaskLoop::SetThrottle(float fraction) {
    throttle_.store(std::clamp(fraction, 0.0F, 1.0F));
}

void TaskLoop::Poll() {
    // Drain the inbox first: a Revoke or Shutdown must be seen before we start
    // another task, not after.
    for (;;) {
        std::vector<std::byte> frame;
        {
            std::lock_guard<std::mutex> lock(inbox_mutex_);
            if (inbox_.empty()) {
                break;
            }
            frame = std::move(inbox_.front());
            inbox_.pop_front();
        }
        OnFrame(frame);
    }

    if (!running_ || !handshaked_) {
        return;
    }

    status_.device_ready = device_.healthy();
    status_.device_recoveries = device_.recovery_count();

    if (!queue_.empty()) {
        const PendingTask task = std::move(queue_.front());
        queue_.pop_front();
        status_.contributing = true;
        const bool ok = Execute(task);
        status_.contributing = false;
        if (ok) {
            ++status_.tasks_completed;
        } else {
            ++status_.tasks_failed;
        }
        return;  // one task per Poll, so the host stays responsive
    }

    // Throttle 0 means "stay connected but stop taking work" (R7). Not a
    // disconnect: the user asked to pause, and dropping the socket would look
    // to the coordinator like a worker that vanished.
    if (throttle_.load() <= 0.0F) {
        return;
    }
    // A lease request that was never answered. The coordinator says "no work"
    // by sending NOTHING, so a timeout is the only way to learn — and a worker
    // that does not time out is silently retired: it stops sending frames, is
    // declared lost after the heartbeat window, and its capacity is gone for the
    // rest of the run while it sits there perfectly healthy.
    //
    // Observed exactly that: one task completed, the keyspace emptied, and the
    // worker went quiet forever 20 s later.
    if (lease_outstanding_ &&
        Clock::now() - lease_requested_at_ > std::chrono::milliseconds(1500)) {
        lease_outstanding_ = false;
        ++empty_replies_;
    }

    // A fetch that has stopped progressing must not pin its parked tasks
    // forever — the coordinator can give them to someone else, and R8 says a
    // worker going quiet is the normal case rather than an error.
    // The peer phase has its own, much shorter deadline (6.9). Timeout-based
    // rather than failure-only: ICE can take many seconds to admit defeat, and
    // a merely slow peer must not block a task the coordinator could already
    // have served.
    // A finished SERVING connection is torn down here rather than in its own
    // callback (see above). `fetch_` being absent is what distinguishes a
    // serving link from one we are fetching over.
    if (peer_serving_done_ && !fetch_) {
        peer_serving_done_ = false;
        peer_.reset();
        peer_target_ = protocol::WorkerId{};
    }

    // 6.15 — accumulate the candidate types across the whole fetch (D-0102).
    // Sampled here rather than at each `peer_.reset()`: there are five reset
    // sites and a new one would silently stop being counted. OR-ing is right
    // because the question is what this worker COULD gather, and a second
    // attempt does not un-gather what the first one found.
    if (peer_) {
        const auto g = peer_->GatheredTypes();
        ice_gathered_ = static_cast<std::uint8_t>(
            ice_gathered_ | (g.host ? 1U : 0U) | (g.server_reflexive ? 2U : 0U) |
            (g.relay ? 4U : 0U));
    }

    if (fetch_ && peer_ && Clock::now() > peer_deadline_) {
        // WHY it timed out, not just that it did. The first cross-machine run
        // logged only "no peer candidates left", which reads as "the list was
        // empty" when in fact a candidate was tried and never connected —
        // two different failures with two different fixes. `IsOpen()`
        // separates them: channel never opened is ICE/NAT, channel open but
        // silent is the peer not answering.
        // SYMPTOM, not a cause. The first version of this line said
        // "(ICE/NAT — check --ice-server)", which is a guess: it fired three
        // times on LOOPBACK during the N=6 E6 run, where NAT cannot exist and
        // the real cause was a holder whose single serving slot was already
        // taken (R-I). A diagnostic that names one cause sends the next reader
        // to the wrong place.
        Log("info", peer_->IsOpen()
                        ? "peer timed out with the channel OPEN "
                          "(connected, but the peer sent nothing)"
                        : "peer timed out with the channel NEVER OPEN "
                          "(peer busy or refusing, or no route: on loopback it "
                          "is not the route)");
        peer_.reset();
        TryNextPeerOrFallBack();
    }

    if (fetch_ && Clock::now() - fetch_->last_progress > kAssetStallTimeout) {
        AbandonAssetFetch("no asset chunk received before the stall timeout");
    }

    // A worker holding a parked task is NOT idle. Asking for more work while
    // waiting on an asset would pile up tasks it cannot start, and each would
    // expire in turn.
    if (!lease_outstanding_ && held_.empty() && waiting_on_asset_.empty()) {
        RequestLease();
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Inbound
// ─────────────────────────────────────────────────────────────────────────

void TaskLoop::OnFrame(std::span<const std::byte> bytes) {
    // The coordinator is not more trusted than anyone else — a worker can be
    // pointed at a hostile URL. Same sanctioned path as the coordinator's own
    // ingress (R11): nothing below reads a field any other way.
    std::vector<std::byte> scratch;
    const auto aligned = protocol::AlignFrame(bytes, scratch);

    const auto verified = protocol::VerifyFrame(aligned);
    if (!verified) {
        Log("warn", "dropped a frame that failed verification: " +
                        std::string{verified.error().message});
        return;
    }

    const wire::Envelope& env = *verified->envelope();
    switch (env.body_type()) {
        case wire::Body::Welcome:
            if (const auto* m = env.body_as_Welcome()) {
                HandleWelcome(*m);
            }
            return;
        case wire::Body::TaskGrant:
            if (const auto* m = env.body_as_TaskGrant()) {
                HandleTaskGrant(*m);
            }
            return;
        case wire::Body::Revoke:
            if (const auto* m = env.body_as_Revoke()) {
                HandleRevoke(*m);
            }
            return;
        case wire::Body::Shutdown:
            HandleShutdown();
            return;
        case wire::Body::Error:
            if (const auto* m = env.body_as_Error()) {
                HandleError(*m);
            }
            return;

        // Not yet handled, but listed so adding a Body variant breaks THIS
        // switch too (-Werror=switch). A `default:` would let a new
        // coordinator-to-worker message be ignored with nobody noticing.
        case wire::Body::BenchmarkRequest:
            if (const auto* m = env.body_as_BenchmarkRequest()) {
                HandleBenchmarkRequest(*m);
            }
            return;

        case wire::Body::LeaseAck:
        case wire::Body::PeerList:
            if (const auto* m = env.body_as_PeerList()) {
                HandlePeerList(*m);
            }
            return;
        case wire::Body::Throttle:
            Log("debug", "unhandled coordinator message");
            return;

        // Worker-to-coordinator messages. Receiving one means we are talking to
        // something that is not a coordinator.
        case wire::Body::Hello:
        case wire::Body::LeaseRequest:
        case wire::Body::ResultHeader:
        case wire::Body::Progress:
        case wire::Body::Release:
        case wire::Body::AssetChunk:
            if (const auto* m = env.body_as_AssetChunk()) {
                HandleAssetChunk(*m);
            }
            break;
        case wire::Body::AssetMiss:
            if (const auto* m = env.body_as_AssetMiss()) {
                HandleAssetMiss(*m);
            }
            break;
        case wire::Body::Signal:
            if (const auto* m = env.body_as_Signal()) {
                HandleSignal(*m);
            }
            return;
        case wire::Body::AssetRequest:
            // The coordinator does not ask US for assets. Ignored rather than
            // acted on; Phase 6 is where a peer legitimately can.
            break;
        case wire::Body::Goodbye:
        case wire::Body::BenchmarkResult:
        case wire::Body::NONE:
            Log("warn", "peer sent a worker-to-coordinator message; ignoring");
            return;
    }
}

void TaskLoop::HandleWelcome(const wire::Welcome& welcome) {
    handshaked_ = true;
    // 3.13 — keep the resume token so a reconnect keeps our standing.
    //
    // Held in memory only. Persisting it to disk would make it a stealable
    // credential at rest for a worker process that has no secret storage on
    // either target, and the cost of losing it is merely starting fresh.
    if (welcome.session_token() != nullptr) {
        resume_token_ = welcome.session_token()->str();
    }

    if (welcome.worker_id() != nullptr) {
        worker_id_ = protocol::WorkerId{*welcome.worker_id()};
    }

    kernel_info_.clear();
    if (const auto* kernels = welcome.kernels()) {
        for (const auto* k : *kernels) {
            if (k == nullptr || k->kernel_id() == nullptr) {
                continue;
            }
            KernelInfo info;
            info.entry_point =
                k->entry_point() != nullptr ? k->entry_point()->str() : "main";
            // The COORDINATOR is the authority on workgroup size and output
            // size — both come from the manifest (R1/R3). A worker that read
            // them out of the WGSL instead would disagree the moment the
            // manifest changed, and the disagreement would look like a bad
            // result rather than a stale worker.
            if (const auto* wg = k->workgroup_size()) {
                info.workgroup_size = wg->x() != 0 ? wg->x() : 64;
                info.workgroup_size_y = wg->y() != 0 ? wg->y() : 1;
            }
            info.flop_per_unit = k->flop_per_unit();
            if (const auto* out = k->output()) {
                info.output_bytes = out->bytes();
                if (const auto* init = out->init()) {
                    // Copied, not aliased: the frame is freed when this returns.
                    info.output_init.assign(
                        static_cast<const std::byte*>(
                            static_cast<const void*>(init->data())),
                        static_cast<const std::byte*>(
                            static_cast<const void*>(init->data())) + init->size());
                }
            }
            kernel_info_.emplace_back(k->kernel_id()->str(), std::move(info));
        }
    }

    // ICE servers for the data plane (6.5, D-0089). Stored, not connected —
    // 6.6+ decides when a peer connection is worth opening.
    ice_servers_.clear();
    if (const auto* ice = welcome.ice_servers()) {
        // Bounded like every other list off the wire: the coordinator is not
        // more trusted than any peer (R11), and an unbounded list would be an
        // allocation an attacker chooses.
        constexpr flatbuffers::uoffset_t kMaxIceServers = 8;
        for (flatbuffers::uoffset_t i = 0;
             i < ice->size() && ice_servers_.size() < kMaxIceServers; ++i) {
            if (const auto* url = ice->Get(i); url != nullptr && url->size() > 0) {
                ice_servers_.emplace_back(url->str());
            }
        }
    }

    status_.last_message = "connected";
    Log("info", "handshake complete; kernels=" + std::to_string(kernel_info_.size()) +
                    " ice_servers=" + std::to_string(ice_servers_.size()));
}

void TaskLoop::HandleTaskGrant(const wire::TaskGrant& grant) {
    lease_outstanding_ = false;
    empty_replies_ = 0;   // work exists again; stop backing off

    const auto* env = grant.envelope();
    if (env == nullptr || env->task_id() == nullptr || env->job_id() == nullptr) {
        Log("warn", "TaskGrant without an envelope or ids");
        return;
    }

    PendingTask task;
    task.id = protocol::TaskId{*env->task_id()};
    task.job = protocol::JobId{*env->job_id()};
    task.kernel_id = env->kernel_id() != nullptr ? env->kernel_id()->str() : "";
    task.start_unit = env->start_unit();
    task.work_units = env->work_units();

    if (const auto* params = env->params()) {
        // Bounded by invariant 1, which the coordinator's own Verifier pass
        // already enforced — but this buffer came off a socket, so it is copied
        // into our own storage rather than aliasing the frame, which is freed
        // as soon as this returns.
        task.params.assign(
            static_cast<const std::byte*>(static_cast<const void*>(params->data())),
            static_cast<const std::byte*>(static_cast<const void*>(params->data())) +
                params->size());
    }
    if (const auto* out = env->output_spec()) {
        task.output_bytes = out->bytes();
    }

    // A task with no bulk input reports `None` rather than inheriting whatever
    // the last asset-bearing task set. Done per TASK, here, because that is the
    // granularity the field describes.
    if (task.asset.empty()) {
        asset_source_ = wire::AssetSource::None;
    }

    const KernelInfo* info = FindKernel(task.kernel_id);
    if (info == nullptr) {
        Log("warn", "granted an unknown kernel: " + task.kernel_id);
        SendRelease(task.id, wire::ReleaseReason::KernelUnavailable);
        return;
    }
    task.workgroup_size = info->workgroup_size;
    task.workgroup_size_y = info->workgroup_size_y;
    // A 2D workgroup means the kernel indexes a GRID rather than a linear range
    // of units, so the dispatch cannot be derived from the chunk (D-0073). The
    // grid comes from the params the coordinator built — bytes 8..23 of
    // PathTraceParams are (tile_x, tile_y, tile_w, tile_h), and tile_w/tile_h
    // are the invocation extents.
    if (info->workgroup_size_y > 1 && task.params.size() >= 24) {
        const auto read_u32 = [&](std::size_t at) {
            std::uint32_t v = 0;
            for (std::size_t i = 0; i < 4; ++i) {
                v |= static_cast<std::uint32_t>(
                         std::to_integer<std::uint8_t>(task.params[at + i]))
                     << (i * 8);
            }
            return v;
        };
        task.invocations_x = read_u32(16);   // tile_w
        task.invocations_y = read_u32(20);   // tile_h
    }

    // ── PARKED ONLY AFTER THE TASK IS FULLY DESCRIBED ────────────────────
    //
    // This block sat ABOVE the kernel lookup, and a parked task therefore kept
    // the DEFAULT workgroup and invocation fields forever — nothing filled them
    // in when it was later released into the queue.
    //
    // The effect was subtle enough to be worth recording: only the FIRST task
    // of a render was ever parked (later ones find the asset resident), so
    // exactly one tile came out wrong. It dispatched 16x1 groups of an
    // @workgroup_size(8,8,1) kernel instead of 8x8, the kernel's own bounds
    // check clipped the overhang, and the tile rendered its top 8 rows and
    // nothing else. 512 pixels of 4096 — no error, no warning, one dark
    // rectangle in the corner of the image.
    // ── A TASK NAMING A BULK ASSET WAITS FOR IT (5.16, D-0077) ───────────
    //
    // Running the kernel without its storage bindings is not a wrong answer,
    // it is a PROCESS ABORT — observed at 5.16 bring-up as a Rust panic inside
    // wgpu-native when the bind group had two entries and the pipeline layout
    // wanted five. One misconfigured job would have taken down the fleet.
    //
    // PARKED, not released. Releasing returns the task to the queue, where this
    // same worker is likely to be granted it again and re-request the same
    // asset — a loop that looks like progress.
    if (const auto* ref = env->input_ref()) {
        std::string address;
        address.reserve(64);
        static constexpr char kHexDigits[] = "0123456789abcdef";
        for (const std::uint64_t lane : {ref->a(), ref->b(), ref->c(), ref->d()}) {
            for (std::size_t byte = 0; byte < 8; ++byte) {
                const auto v = static_cast<std::uint8_t>((lane >> (byte * 8)) & 0xFFU);
                address.push_back(kHexDigits[v >> 4]);
                address.push_back(kHexDigits[v & 0x0FU]);
            }
        }
        task.asset = address;

        if (address == resident_asset_) {
            // Already held — no fetch at all. A distinct outcome from either
            // kind of fetch, which is what lets 6.14 compute a peer share that
            // does not fall as the fleet reuses the asset.
            asset_source_ = wire::AssetSource::Cached;
        }
        if (address != resident_asset_) {
            waiting_on_asset_.push_back(std::move(task));
            if (!fetch_ || fetch_->address != address) {
                // A different asset than the one in flight supersedes it: the
                // coordinator has moved on, and finishing the old fetch would
                // spend bandwidth on bytes nothing is waiting for.
                fetch_ = AssetFetch{};
                fetch_->address = address;
                // AUTHORITATIVE, from the coordinator (D-0091). A peer cannot
                // influence it, which is what lets us reject a claimed chunk
                // count instead of believing one.
                fetch_->expected_bytes = env->input_bytes();
                fetch_->started = Clock::now();
                fetch_->last_progress = fetch_->started;
                peer_attempt_ = 0;
                // 6.15 — per-FETCH, so a cached task does not inherit the ICE
                // story of an earlier one (D-0102).
                ice_gathered_ = 0;
                peer_attempts_made_ = 0;
                peer_connected_ = false;
                Log("info", "fetching asset " + address.substr(0, 12) + "...");
                // PEERS FIRST (D-0007). The whole point of the data plane is
                // that the coordinator does not serve this; the mandatory
                // fallback is what makes trying something unreliable first a
                // safe thing to do.
                TryNextPeerOrFallBack();
            }
            return;
        }
    }

    if (task.output_bytes == 0) {
        task.output_bytes = info->output_bytes;
    }

    held_.push_back(task.id);
    queue_.push_back(std::move(task));
}

void TaskLoop::HandleRevoke(const wire::Revoke& revoke) {
    if (revoke.task_id() == nullptr) {
        return;
    }
    const protocol::TaskId id{*revoke.task_id()};

    // Drop it from the queue if it has not started. A task already running is
    // left to finish — the result is discarded silently by the coordinator
    // (2.10), and aborting mid-dispatch buys nothing since chunking already
    // bounds how long that takes (R4).
    //
    // With speculation (2.17) this is now the COMMON case rather than an
    // exception: a revoke means somebody else already finished this range.
    std::erase_if(queue_, [&](const PendingTask& t) { return t.id == id; });
    std::erase(held_, id);
    Log("info", "task revoked");
}

std::uint64_t TaskLoop::ChunkUnitsFor(const KernelInfo& info) const {
    // ── WHY THIS IS MEASURED AND NOT A CONSTANT (D-0044) ─────────────────
    // R4 caps a dispatch at ~250 ms of expected work; D-0021 measured that
    // browser chunks below ~50 ms collapse into submission overhead, because
    // browsers clamp nested timers to >=4 ms and each submit costs ~5 ms.
    //
    // A single constant cannot satisfy both. The old fixed 2^20 was ~0.03 ms of
    // work on an M4 Pro, so a two-second task became ~70,000 chunks of almost
    // pure overhead and expired before it could finish — the worker then never
    // completed anything, so the coordinator's correction factor never got a
    // sample to learn from.
    //
    // 100 ms sits between D-0021's floor and R4's ceiling with room on both
    // sides for an estimate that is wrong by 2x in either direction.
    constexpr double kTargetChunkMs = 100.0;

    if (!(measured_ops_per_sec_ > 0.0) || info.flop_per_unit == 0) {
        return config_.units_per_chunk;   // not benchmarked yet
    }
    const double units_per_sec =
        measured_ops_per_sec_ / static_cast<double>(info.flop_per_unit);
    const double units = (kTargetChunkMs / 1000.0) * units_per_sec;
    if (!(units >= 1.0)) {
        return 1;
    }
    return static_cast<std::uint64_t>(units);
}

void TaskLoop::HandleBenchmarkRequest(const wire::BenchmarkRequest& request) {
    const std::string kernel_id =
        request.kernel_id() != nullptr ? request.kernel_id()->str() : "";
    const auto wgsl = kernels_(kernel_id);
    if (!wgsl || !device_.healthy()) {
        // Report NOTHING rather than a guess. The coordinator refuses work to a
        // worker with no score, which is the correct outcome — a fabricated
        // number would feed its correction factor and mis-size every future
        // grant for this machine.
        Log("warn", "cannot benchmark " + kernel_id + "; no work will be granted");
        return;
    }

    // A short sweep, not a single size: one measurement at one problem size can
    // land on a bad occupancy point and under-report a fast device by a lot.
    const std::vector<std::uint32_t> sizes{1u << 16, 1u << 18, 1u << 20};
    const auto samples = RunCalibration(device_.context(), *wgsl, sizes, 2048);

    // BEST, not mean. The slower samples are dominated by launch overhead and
    // by whatever else the machine was doing; the best is the closest estimate
    // of what this device can sustain. The EWMA correction (2.13) is what pulls
    // it back toward reality if it turns out optimistic — which is exactly the
    // division of labour that lets this be a rough number.
    double best_ops_per_sec = 0.0;
    for (const auto& s : samples) {
        if (s.wall_ms > 0.0) {
            best_ops_per_sec =
                std::max(best_ops_per_sec,
                         static_cast<double>(s.flops) / (s.wall_ms / 1000.0));
        }
    }
    if (!(best_ops_per_sec > 0.0)) {
        Log("warn", "benchmark produced no usable sample");
        return;
    }

    auto frame = protocol::EncodeMessage(
        wire::Body::BenchmarkResult, [&](flatbuffers::FlatBufferBuilder& fbb) {
            auto kid = fbb.CreateString(kernel_id);
            wire::BenchmarkResultBuilder b(fbb);
            b.add_kernel_id(kid);
            b.add_score(best_ops_per_sec);
            b.add_samples(static_cast<std::uint32_t>(samples.size()));
            return b.Finish();
        });
    measured_ops_per_sec_ = best_ops_per_sec;
    (void)transport_.Send(frame);
    Log("info", "benchmark: " + std::to_string(best_ops_per_sec / 1e9) + " Gops/s");
}

void TaskLoop::HandlePeerList(const wire::PeerList& list) {
    // Candidate sources for the asset this worker was just told it needs
    // (6.2, D-0086). A HINT, not an instruction: a listed peer may have
    // departed, may refuse, or may serve corrupt bytes. All three are safe
    // because whatever arrives is verified against the address we asked for
    // (5.4) and the coordinator remains the fallback (D-0007).
    //
    // Stored, not acted on — 6.4 is what opens a connection. Keeping those
    // separate means this step can be tested without a WebRTC stack.
    peer_candidates_.clear();
    const auto* peers = list.peers();
    if (peers == nullptr) {
        return;
    }
    // Bounded on receipt regardless of what the coordinator claims. The
    // coordinator is not more trusted than any other peer (R11), and a worker
    // that opened a connection per listed entry would be one malicious frame
    // away from building the mesh 6.2 exists to avoid.
    constexpr std::size_t kMaxPeerCandidates = 8;
    for (flatbuffers::uoffset_t i = 0;
         i < peers->size() && peer_candidates_.size() < kMaxPeerCandidates; ++i) {
        const auto* entry = peers->Get(i);
        if (entry == nullptr || entry->worker_id() == nullptr) {
            continue;
        }
        peer_candidates_.push_back(protocol::WorkerId{*entry->worker_id()});
    }
    Log("info", "peer list: " + std::to_string(peer_candidates_.size()) +
                    " candidate source(s) for the asset");
}

void TaskLoop::HandleShutdown() {
    Log("info", "coordinator asked us to shut down");
    Stop();
}

void TaskLoop::HandleError(const wire::Error& error) {
    const std::string msg =
        error.message() != nullptr ? error.message()->str() : "(no message)";
    if (error.fatal()) {
        // Fatal means retrying cannot succeed (PROTOCOL.md §5). Reconnecting
        // against a version we can never satisfy is exactly the loop the fatal
        // flag exists to prevent.
        Log("error", "fatal error from coordinator: " + msg);
        status_.last_message = "fatal: " + msg;
        Stop();
        return;
    }
    Log("warn", "coordinator reported: " + msg);
    status_.last_message = msg;
}

// ─────────────────────────────────────────────────────────────────────────
// Outbound
// ─────────────────────────────────────────────────────────────────────────

void TaskLoop::SendHello() {
    const auto desc = platform::DescribeAdapter(device_.context());

    auto frame = protocol::EncodeMessage(
        wire::Body::Hello, [&](flatbuffers::FlatBufferBuilder& fbb) {
            // Empty on a first connect; set after a Welcome, so a reconnect
            // reclaims the same identity and its reputation (3.13).
            auto resume = fbb.CreateString(resume_token_);
            auto vendor = fbb.CreateString(desc.vendor);
            auto arch = fbb.CreateString(desc.architecture);
            auto dev = fbb.CreateString(desc.device);
            auto backend = fbb.CreateString(desc.backend);
            wire::AdapterInfoBuilder ab(fbb);
            ab.add_vendor(vendor);
            ab.add_architecture(arch);
            ab.add_device(dev);
            ab.add_backend(backend);
            auto adapter = ab.Finish();

            std::vector<flatbuffers::Offset<flatbuffers::String>> feature_offsets;
            feature_offsets.reserve(desc.features.size());
            for (const auto& f : desc.features) {
                feature_offsets.push_back(fbb.CreateString(f));
            }
            auto features = fbb.CreateVector(feature_offsets);

            // K4: the DEVICE's queried numbers, never the spec defaults. The
            // sizer uses these, so a hardcoded value here becomes a task that
            // does not fit on the machine that was granted it.
            wire::GpuLimitsBuilder lb(fbb);
            lb.add_max_storage_buffer_binding_size(desc.max_storage_buffer_binding_size);
            lb.add_max_buffer_size(desc.max_buffer_size);
            lb.add_max_compute_workgroups_per_dim(desc.max_compute_workgroups_per_dim);
            lb.add_max_compute_invocations_per_group(
                desc.max_compute_invocations_per_workgroup);
            lb.add_max_compute_workgroup_storage_size(
                desc.max_compute_workgroup_storage_size);
            auto limits = lb.Finish();

            auto ua = fbb.CreateString(desc.description);
            wire::WorkerCapabilitiesBuilder cb(fbb);
            cb.add_user_agent(ua);
            cb.add_adapter(adapter);
            cb.add_features(features);
            cb.add_limits(limits);
            cb.add_supports_webrtc(true);
            auto caps = cb.Finish();

            // What this worker already holds (5.18). Advisory: the coordinator
            // uses it to prefer tasks whose asset is resident, and a worker that
            // over-reports simply fetches something it claimed to have.
            //
            // Created BEFORE the HelloBuilder opens — a nested vector cannot be
            // built while a table is under construction.
            std::vector<flatbuffers::Offset<flatbuffers::String>> held;
            if (!resident_asset_.empty()) {
                held.push_back(fbb.CreateString(resident_asset_));
            }
            auto cached = fbb.CreateVector(held);

            wire::HelloBuilder hb(fbb);
            hb.add_protocol_version(protocol::kProtocolVersion);
            hb.add_resume_token(resume);
            hb.add_capabilities(caps);
            hb.add_cached_assets(cached);
            return hb.Finish();
        });

    (void)transport_.Send(frame);
}

void TaskLoop::SendProgress(protocol::TaskId task, float fraction) {
    auto frame = protocol::EncodeMessage(
        wire::Body::Progress, [&](flatbuffers::FlatBufferBuilder& fbb) {
            const wire::Uuid tid = task.to_wire();
            wire::ProgressBuilder b(fbb);
            b.add_task_id(&tid);
            // Telemetry the coordinator must not act on (invariant 8), reported
            // honestly regardless — a worker that lied here by default would
            // make the dishonest mock profiles less distinguishable.
            b.add_fraction_done(fraction);
            b.add_request_renew(true);
            return b.Finish();
        });
    (void)transport_.Send(frame);
}

void TaskLoop::RequestLease() {
    // Exponential backoff with jitter on repeated empty replies (2.15). Capped
    // so a worker that waited through a lull still picks work up promptly when
    // a job arrives — backing off to minutes would make an idle fleet look dead.
    const auto now = Clock::now();
    if (empty_replies_ > 0) {
        const std::uint32_t step = std::min<std::uint32_t>(empty_replies_, 6);
        const auto base = std::chrono::milliseconds(100u << (step - 1));
        // Jitter, so a fleet that emptied the queue together does not come back
        // together (RISKS.md §2).
        const auto jitter = std::chrono::milliseconds(
            // Jitter seed from our own address — NOT network input, and never
            // dereferenced. `reinterpret_cast` is the only spelling that
            // converts a pointer to an integer (R11 audit, 4.16).
            static_cast<int>(reinterpret_cast<std::uintptr_t>(this) % 250));
        if (now < next_action_) {
            return;
        }
        next_action_ = now + std::min(base, std::chrono::milliseconds(5000)) + jitter;
    }

    auto frame = protocol::EncodeMessage(
        wire::Body::LeaseRequest, [&](flatbuffers::FlatBufferBuilder& fbb) {
            // What we hold RIGHT NOW (5.18). Sent on every request rather than
            // once at Hello, because the handshake precedes every fetch and so
            // always reports an empty cache. Built before the table opens.
            std::vector<flatbuffers::Offset<flatbuffers::String>> held;
            if (!resident_asset_.empty()) {
                held.push_back(fbb.CreateString(resident_asset_));
            }
            auto cached = fbb.CreateVector(held);

            wire::LeaseRequestBuilder b(fbb);
            b.add_cached_assets(cached);
            // A HINT. The coordinator decides how much work we get and how big
            // it is (R1/D-0005); asking is not the same as sizing.
            b.add_max_tasks(config_.max_tasks_in_flight);
            return b.Finish();
        });
    if (transport_.Send(frame)) {
        lease_outstanding_ = true;
    }
}

void TaskLoop::SendResult(protocol::TaskId task, const TaskOutcome& outcome) {
    SendResultFrame(task, outcome, 0, 0);
}

void TaskLoop::SendPartialResult(protocol::TaskId task, const TaskOutcome& outcome,
                                 std::uint32_t sequence, std::uint64_t units_done) {
    // `units_done` must be NON-ZERO and less than the task's total, or the
    // coordinator reads it as complete and finishes the task (D-0074). The
    // caller owns that; passing 0 here would end the task early with a partial
    // image, which would render as a dark tile rather than as an error.
    SendResultFrame(task, outcome, sequence, units_done);
}

void TaskLoop::SendResultFrame(protocol::TaskId task, const TaskOutcome& outcome,
                               std::uint32_t sequence, std::uint64_t units_done) {
    const std::span<const std::byte> payload{outcome.output};

    auto frame = protocol::EncodeMessage(
        wire::Body::ResultHeader,
        [&](flatbuffers::FlatBufferBuilder& fbb) {
            const wire::Uuid tid = task.to_wire();

            // FACTS, not claims we act on. The coordinator treats every field
            // here as untrusted telemetry (invariant 8) because a worker can
            // lie about all of them — including this one.
            wire::TaskStatsBuilder sb(fbb);
            sb.add_gpu_ms(outcome.stats.gpu_ms);
            sb.add_transfer_ms(outcome.stats.transfer_ms);
            sb.add_idle_ms(outcome.stats.idle_ms);
            sb.add_dispatches(outcome.stats.dispatches);
            sb.add_iterations(outcome.stats.iterations);
            // 6.11 — where this task's bulk input came from. Telemetry, and the
            // coordinator treats it as such: invariant 8 says a worker's
            // self-report never decides anything. It is E6 evidence, not a
            // scheduling input.
            sb.add_asset_source(asset_source_);
            // 6.15 (D-0102). Diagnostics, and labelled as such: `ice_gathered`
            // is what ICE COULD offer, not what the connection used, and
            // `peer_connected` is the field the relay ratio is differenced
            // from across a run with TURN and a run without.
            // ONLY for the task that actually fetched. A worker fetches once
            // and then runs many tasks from cache, and those tasks would
            // otherwise inherit the fetch's ICE state — measured: 9 reported
            // attempts for 2 real fetches, a denominator inflated 4.5x by
            // tasks that never touched ICE. `Cached` and `None` mean no
            // candidates were tried for THIS task, so the honest report is
            // zero.
            const bool this_task_fetched =
                asset_source_ == wire::AssetSource::Peer ||
                asset_source_ == wire::AssetSource::Coordinator;
            sb.add_ice_gathered(this_task_fetched ? ice_gathered_ : 0);
            sb.add_peer_attempts(this_task_fetched ? peer_attempts_made_ : 0);
            sb.add_peer_connected(this_task_fetched && peer_connected_);
            auto stats = sb.Finish();

            wire::ResultHeaderBuilder b(fbb);
            b.add_task_id(&tid);
            b.add_payload_bytes(static_cast<std::uint32_t>(payload.size()));
            b.add_checksum(Blake3_64(payload));
            b.add_stats(stats);
            // 0/0 for a one-shot result, which is what every kernel before the
            // path tracer sends and what the coordinator's defaults describe
            // (D-0074).
            b.add_sequence(sequence);
            b.add_units_done(units_done);
            return b.Finish();
        },
        payload);

    (void)transport_.Send(frame);
    // A PARTIAL keeps the lease: the task is not finished and this worker still
    // holds it. Only a complete result (units_done == 0, meaning "all of it"
    // per D-0074) gives it up.
    if (units_done == 0) {
        std::erase(held_, task);
    }
}

void TaskLoop::SendRelease(protocol::TaskId task, wire::ReleaseReason reason) {
    auto frame = protocol::EncodeMessage(
        wire::Body::Release, [&](flatbuffers::FlatBufferBuilder& fbb) {
            const wire::Uuid tid = task.to_wire();
            wire::ReleaseBuilder b(fbb);
            b.add_task_id(&tid);
            b.add_reason(reason);
            return b.Finish();
        });
    (void)transport_.Send(frame);
    std::erase(held_, task);
}

// ─────────────────────────────────────────────────────────────────────────
// Bulk assets over the control link — step 5.16 (D-0077)
// ─────────────────────────────────────────────────────────────────────────

void TaskLoop::SendAssetRequest(const std::string& address, std::uint32_t from,
                                std::uint32_t to) {
    // Hex back to Hash32's four little-endian u64 lanes. The inverse of the
    // encoder in HandleTaskGrant; both spell the byte order out rather than
    // memcpy-ing a struct, because the two live far apart and are exactly the
    // kind of pairing that drifts.
    if (address.size() != 64) {
        return;
    }
    std::array<std::uint64_t, 4> lanes{};
    for (std::size_t lane = 0; lane < 4; ++lane) {
        std::uint64_t v = 0;
        for (std::size_t byte = 0; byte < 8; ++byte) {
            const std::string pair = address.substr(lane * 16 + byte * 2, 2);
            v |= static_cast<std::uint64_t>(std::stoul(pair, nullptr, 16)) << (byte * 8);
        }
        lanes[lane] = v;
    }
    auto frame = protocol::EncodeMessage(
        wire::Body::AssetRequest, [&](flatbuffers::FlatBufferBuilder& fbb) {
            const wire::Hash32 h(lanes[0], lanes[1], lanes[2], lanes[3]);
            wire::AssetRequestBuilder b(fbb);
            b.add_hash(&h);
            b.add_chunk_from(from);
            b.add_chunk_to(to);
            return b.Finish();
        });
    (void)transport_.Send(frame);
}

void TaskLoop::RequestAssetChunks() {
    if (!fetch_) {
        return;
    }
    // Ask for the next contiguous run of missing chunks. Contiguous rather than
    // "every gap": a request carries one range, and the common case after a
    // batch is a single gap at the end.
    std::uint32_t first_missing = 0;
    bool found = false;
    for (std::uint32_t i = 0; i < fetch_->total; ++i) {
        if (!fetch_->have[i]) {
            first_missing = i;
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }
    SendAssetRequest(fetch_->address, first_missing,
                     std::min(fetch_->total, first_missing + kAssetChunkRequestBatch));
}

void TaskLoop::HandleAssetChunk(const wire::AssetChunk& chunk) {
    if (!fetch_) {
        return;   // nothing outstanding; a late chunk from a superseded fetch
    }
    const auto* bytes = chunk.bytes();
    if (bytes == nullptr) {
        return;
    }

    // INVARIANT 10, and it is the reason this path was fuzzed before it had a
    // caller (4.13). `index * kChunkBytes` wraps for a large enough index, and
    // an unsigned wrap is not UB — no sanitizer fires, and the write lands
    // somewhere it should not.
    if (fetch_->total == 0) {
        // ── THE SIZE IS OURS, NOT THE SENDER'S (6.7, D-0091) ─────────────
        //
        // `expected_chunks_for(hash)`, computed from the length the COORDINATOR
        // gave us. Before this the count came from whatever the first arriving
        // chunk claimed, which let a peer choose the size of this buffer.
        if (fetch_->expected_bytes == 0 ||
            fetch_->expected_bytes > protocol::kMaxAssetBytes) {
            AbandonAssetFetch("no authoritative asset size, or it exceeds the limit");
            return;
        }
        const std::uint64_t expected_chunks =
            (fetch_->expected_bytes + protocol::kChunkBytes - 1) / protocol::kChunkBytes;
        if (expected_chunks == 0 || expected_chunks > 0xFFFFFFFFULL) {
            AbandonAssetFetch("asset chunk count is out of range");
            return;
        }
        if (chunk.total() != static_cast<std::uint32_t>(expected_chunks)) {
            // The sender disagrees with the coordinator about how big this asset
            // is. One of them is lying and it is not the one that owns it.
            AbandonAssetFetch("sender's chunk count disagrees with the coordinator");
            return;
        }
        fetch_->total = static_cast<std::uint32_t>(expected_chunks);
        fetch_->have.assign(fetch_->total, false);
        // Sized to the TRUE length, so there is nothing to trim afterwards.
        fetch_->bytes.assign(static_cast<std::size_t>(fetch_->expected_bytes),
                             std::byte{0});
    }
    if (const auto s = protocol::CheckAssetChunk(chunk.index(), chunk.total(),
                                                 fetch_->total, bytes->size());
        !s) {
        AbandonAssetFetch("asset chunk failed invariant 10");
        return;
    }
    const auto offset = protocol::ChunkOffset(chunk.index());
    if (!offset || *offset + bytes->size() > fetch_->bytes.size()) {
        AbandonAssetFetch("asset chunk lands outside the reassembly buffer");
        return;
    }
    if (fetch_->have[chunk.index()]) {
        return;   // duplicate; idempotent by design, retries are normal
    }

    // A NON-FINAL CHUNK MUST BE EXACTLY kChunkBytes.
    //
    // The subtle one. A short non-final chunk leaves a HOLE at a computed
    // offset: every individual bound still holds, the received count still
    // reaches `total`, and only the final BLAKE3 notices — reporting "the asset
    // was corrupt" about a peer that actually lied about a length. Rejecting it
    // here names the real fault.
    const bool is_final = (chunk.index() + 1 == fetch_->total);
    const std::size_t want =
        is_final ? static_cast<std::size_t>(fetch_->expected_bytes) -
                       (static_cast<std::size_t>(fetch_->total - 1) * protocol::kChunkBytes)
                 : protocol::kChunkBytes;
    if (bytes->size() != want) {
        AbandonAssetFetch("chunk length disagrees with its position in the asset");
        return;
    }

    const auto* src = static_cast<const std::byte*>(
        static_cast<const void*>(bytes->data()));
    std::copy(src, src + bytes->size(), fetch_->bytes.begin() +
                                            static_cast<std::ptrdiff_t>(*offset));
    fetch_->have[chunk.index()] = true;
    ++fetch_->received;
    fetch_->last_progress = Clock::now();

    // No trim: the buffer was sized to the coordinator's exact length, so the
    // last chunk fills it precisely. Trimming used to be necessary because the
    // buffer was a whole number of chunks, and it is the kind of step that
    // quietly stops being correct when the sizing changes (D-0091).

    if (fetch_->received == fetch_->total) {
        FinishAssetFetch();
    } else if (fetch_->received % kAssetChunkRequestBatch == 0) {
        RequestAssetChunks();
    }
}

// ── Peer data plane (6.6, D-0090) ────────────────────────────────────────

void TaskLoop::PumpSignalling() {
    // ── SERVING A PEER MUST NOT REQUIRE BEING IDLE ───────────────────────
    //
    // The inbox is drained in `Poll()`, and `Poll()` is blocked inside
    // `RunTask` for the whole task. With multi-second tasks that makes a BUSY
    // worker unable to answer a peer's offer at all — the offerer waits out its
    // deadline and falls back to the coordinator.
    //
    // Measured as E6's curve failing to flatten: at 6 workers, 4 still fetched
    // from the coordinator, because the workers that HELD the asset were all
    // mid-task and therefore unreachable. **The swarm worked only among idle
    // workers, which is exactly when it is least needed.**
    //
    // R4's chunking is what makes the fix cheap: the host already calls back
    // between dispatches, so there is a natural point to service signalling
    // without interrupting GPU work.
    //
    // ONLY signalling is handled here. A `TaskGrant` processed mid-task would
    // start a second task inside the first, so everything else is put back in
    // order and waits for `Poll()`.
    std::deque<std::vector<std::byte>> pending;
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        pending.swap(inbox_);
    }
    std::deque<std::vector<std::byte>> keep;
    for (auto& frame : pending) {
        const auto verified = protocol::VerifyFrame(frame);
        if (verified) {
            const auto* env = verified->envelope();
            if (env != nullptr && env->body_type() == wire::Body::Signal) {
                if (const auto* m = env->body_as_Signal()) {
                    HandleSignal(*m);
                }
                continue;
            }
        }
        keep.push_back(std::move(frame));
    }
    if (!keep.empty()) {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        // Order preserved: anything that arrived while we were pumping goes
        // after what was already queued.
        for (auto it = keep.rbegin(); it != keep.rend(); ++it) {
            inbox_.push_front(std::move(*it));
        }
    }
}

void TaskLoop::SendSignal(protocol::WorkerId to, const transport::SignalOut& out) {
    // The inner PeerSignal, verified by the RECEIVING worker. The coordinator
    // relays `payload` without reading it (D-0085), so the structure is ours to
    // agree on with the peer and nobody else's business.
    const auto inner = protocol::EncodePeerSignal(out.kind, out.text, out.mid);
    const wire::Uuid dest = to.to_wire();
    auto frame = protocol::EncodeMessage(
        wire::Body::Signal, [&](flatbuffers::FlatBufferBuilder& fbb) {
            auto pv = fbb.CreateVector(
                static_cast<const std::uint8_t*>(
                    static_cast<const void*>(inner.data())),
                inner.size());
            wire::SignalBuilder b(fbb);
            b.add_peer(&dest);
            b.add_payload(pv);
            return b.Finish();
        });
    (void)transport_.Send(frame);
}

void TaskLoop::HandleSignal(const wire::Signal& signal) {
    // `peer` is the SENDER here — the coordinator rewrote it on relay (D-0085).
    if (signal.peer() == nullptr || signal.payload() == nullptr) {
        return;
    }
    const protocol::WorkerId from{*signal.peer()};

    const auto* p = signal.payload();
    const std::span<const std::byte> bytes{
        static_cast<const std::byte*>(static_cast<const void*>(p->data())),
        p->size()};
    // Relayed peer bytes, no more trusted for having passed through the
    // coordinator. Schema-verified rather than hand-parsed (ARCHITECTURE.md §9).
    const auto verified = protocol::VerifyPeerSignal(bytes);
    if (!verified) {
        Log("warn", "peer signal failed verification; dropped");
        return;
    }
    const wire::PeerSignal* sig = *verified;
    const std::string kind = sig->kind()->str();
    const std::string text = sig->text()->str();
    const std::string mid = sig->mid() != nullptr ? sig->mid()->str() : std::string{};

    // ── THE ANSWERING SIDE (6.8) ─────────────────────────────────────────
    //
    // A worker that HOLDS an asset must accept an incoming connection, or the
    // data plane only ever has offerers and nothing is served. This was the
    // missing half at bring-up: signalling flowed, three frames were relayed,
    // and the offer was dropped because the receiver had no PeerLink — so every
    // fetch fell back and the logs showed a timeout rather than a cause.
    if (!peer_ && kind == "offer") {
        // Only if there is something to serve. Accepting a connection with an
        // empty cache spends ICE on a negotiation that can only end in
        // AssetMiss, and 6.12 would count it as a peer that did nothing.
        if (resident_bytes_.empty()) {
            return;
        }
        peer_target_ = from;
        peer_ = std::make_unique<transport::PeerLink>(ice_servers_);
        const protocol::WorkerId target = from;
        peer_->OnSignal([this, target](const transport::SignalOut& out) {
            SendSignal(target, out);
        });
        peer_->OnMessage([this](std::span<const std::byte> b) { OnPeerBytes(b); });
        peer_->OnOpen([this](bool open) {
            Log("info", open ? "peer channel open (serving)"
                             : "peer channel closed (serving)");
            if (!open) {
                // FREE THE SLOT. A worker holds one connection at a time, and
                // without this a worker that served ONE peer kept `peer_` set
                // forever and refused every later offer — so each holder could
                // seed exactly one other worker for the life of the process,
                // capping peer fetches at roughly half the fleet. That is the
                // shape E6 measured: coordinator fetches of 1, 1, 2, 4 for
                // fleets of 1, 2, 4, 6.
                //
                // DEFERRED to Poll(), not done here: this runs inside the
                // link's own callback, and destroying the object mid-callback
                // is the D-0060 use-after-free in a new place.
                peer_serving_done_ = true;
            }
        });
        Log("info", "accepting a peer connection from worker " +
                        std::to_string(from.hi()));
        peer_->AcceptOffer(text);
        return;
    }

    if (!peer_ || from != peer_target_) {
        // Signalling from someone we are not negotiating with. Dropped rather
        // than acted on: a peer able to inject candidates into someone else's
        // connection can redirect it.
        return;
    }

    if (kind == "answer") {
        peer_->AcceptAnswer(text);
    } else if (kind == "candidate") {
        peer_->AddRemoteCandidate(text, mid);
    }
    // Anything else is ignored. The set is closed, and a peer inventing a
    // fourth kind is telling us it disagrees about the protocol.
}

void TaskLoop::TryNextPeerOrFallBack() {
    if (!fetch_) {
        return;
    }
    if (peer_attempt_ >= peer_candidates_.size() ||
        peer_attempt_ >= kMaxPeerAttempts) {
        FallBackToCoordinator("no peer candidates left");
        return;
    }

    peer_target_ = peer_candidates_[peer_attempt_++];
    if (peer_attempts_made_ < 255) {
        ++peer_attempts_made_;   // 6.15 (D-0102)
    }
    peer_deadline_ = Clock::now() + kPeerPhaseTimeout;

    peer_ = std::make_unique<transport::PeerLink>(ice_servers_);
    // Handlers BEFORE Offer(): negotiation starts the moment the channel is
    // created, and a description produced before the handler exists is lost.
    const protocol::WorkerId target = peer_target_;
    peer_->OnSignal([this, target](const transport::SignalOut& out) {
        SendSignal(target, out);
    });
    peer_->OnMessage([this](std::span<const std::byte> bytes) { OnPeerBytes(bytes); });
    peer_->OnOpen([this](bool open) {
        if (open) {
            peer_connected_ = true;   // 6.15 (D-0102)
        }
        if (!open || !fetch_) {
            return;
        }
        // The channel is up: ask for the asset. Same message the coordinator
        // path sends, over a different transport (D-0090).
        Log("info", "peer channel open; requesting the asset");
        SendToPeer(protocol::EncodeAssetMsg(
            wire::AssetBody::AssetRequest,
            [&](flatbuffers::FlatBufferBuilder& fbb) {
                const wire::Hash32 h = protocol::HashFromHex(fetch_->address);
                wire::AssetRequestBuilder b(fbb);
                b.add_hash(&h);
                b.add_chunk_from(0);
                b.add_chunk_to(kAssetChunkRequestBatch);
                return b.Finish();
            }));
    });
    Log("info", "trying peer " + std::to_string(peer_target_.hi()) +
                    " for the asset (attempt " + std::to_string(peer_attempt_) + ")");
    peer_->Offer();
}

void TaskLoop::FallBackToCoordinator(const char* why) {
    // D-0007: the coordinator is the mandatory fallback, and it is what makes
    // trying an unreliable peer first a safe thing to do. A fetch that reaches
    // here still completes; it just costs the coordinator egress the data plane
    // was meant to save.
    Log("info", std::string("falling back to the coordinator: ") + why);
    peer_.reset();
    peer_target_ = protocol::WorkerId{};
    if (fetch_) {
        fetch_->last_progress = Clock::now();
        SendAssetRequest(fetch_->address, 0, kAssetChunkRequestBatch);
    }
}

void TaskLoop::SendToPeer(const std::vector<std::byte>& msg) {
    if (peer_ && peer_->IsOpen()) {
        (void)peer_->Send(msg);
    }
}

void TaskLoop::OnPeerBytes(std::span<const std::byte> bytes) {
    // ── THE LEAST TRUSTED INPUT IN THE SYSTEM ────────────────────────────
    // No coordinator mediation at all. Verified as `AssetMsg`, whose root is
    // deliberately NOT `Envelope`: a peer therefore cannot form a control
    // message, rather than forming one that a switch arm has to remember to
    // reject (D-0090).
    //
    // ALIGNMENT. `flatbuffers::Verifier` assumes the buffer base is aligned to
    // minalign and checks only RELATIVE offsets — that is D-0027, which was UB
    // on every message for weeks and ran correctly on two architectures. What
    // arrives here is a `std::vector<std::byte>`, so it is over-aligned; the
    // check is cheap and states the requirement rather than inheriting it from
    // a library's allocator.
    std::vector<std::byte> aligned;
    if ((std::bit_cast<std::uintptr_t>(bytes.data()) %
         protocol::kFrameAlignment) != 0) {
        aligned.assign(bytes.begin(), bytes.end());
        bytes = aligned;
    }

    const auto verified = protocol::VerifyAssetMsg(bytes);
    if (!verified) {
        Log("warn", "peer sent an unverifiable asset message; dropped");
        return;
    }
    const wire::AssetMsg* msg = *verified;
    switch (msg->body_type()) {
        case wire::AssetBody::AssetRequest:
            if (const auto* m = msg->body_as_AssetRequest()) {
                ServePeerAssetRequest(*m);
            }
            return;
        case wire::AssetBody::AssetChunk:
            if (const auto* m = msg->body_as_AssetChunk()) {
                // THE SAME reassembly the coordinator path uses. Invariant 10,
                // the 64-bit offset arithmetic and the BLAKE3 check are enforced
                // once, so a second transport cannot get a weaker version of
                // them (4.13 is why that matters).
                HandleAssetChunk(*m);
            }
            return;
        case wire::AssetBody::AssetMiss:
            if (const auto* m = msg->body_as_AssetMiss()) {
                HandleAssetMiss(*m);
            }
            return;
        case wire::AssetBody::NONE:
            return;
    }
}

void TaskLoop::ServePeerAssetRequest(const wire::AssetRequest& req) {
    // We serve only what we hold and have already VALIDATED (5.5). Serving
    // unvalidated bytes would make this worker an amplifier for a blob it never
    // checked — and the requester verifies the hash anyway, so the only thing
    // that would buy is wasted bandwidth on both ends.
    const wire::Hash32* want = req.hash();
    if (want == nullptr || resident_asset_.empty() || resident_bytes_.empty()) {
        return;
    }
    std::string address;
    address.reserve(64);
    static constexpr char kHexDigits[] = "0123456789abcdef";
    for (const std::uint64_t lane : {want->a(), want->b(), want->c(), want->d()}) {
        for (std::size_t byte = 0; byte < 8; ++byte) {
            const auto v = static_cast<std::uint8_t>((lane >> (byte * 8)) & 0xFFU);
            address.push_back(kHexDigits[v >> 4]);
            address.push_back(kHexDigits[v & 0x0FU]);
        }
    }
    if (address != resident_asset_) {
        SendToPeer(protocol::EncodeAssetMsg(
            wire::AssetBody::AssetMiss, [&](flatbuffers::FlatBufferBuilder& fbb) {
                wire::AssetMissBuilder b(fbb);
                b.add_hash(want);
                return b.Finish();
            }));
        return;
    }

    const std::size_t total =
        (resident_bytes_.size() + protocol::kChunkBytes - 1) / protocol::kChunkBytes;
    // CLAMPED to what exists. `chunk_to` is attacker-chosen, and an unclamped
    // range turns one request into an unbounded send loop — an amplification
    // attack costing the requester one message (the same guard the coordinator
    // has at 5.16).
    const std::uint32_t from = req.chunk_from();
    const auto to = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(req.chunk_to(), total));
    if (from >= to) {
        return;
    }
    constexpr std::uint32_t kMaxChunksPerPeerRequest = 64;
    const std::uint32_t end = std::min(to, from + kMaxChunksPerPeerRequest);

    for (std::uint32_t i = from; i < end; ++i) {
        const auto offset = protocol::ChunkOffset(i);
        if (!offset || *offset >= resident_bytes_.size()) {
            break;
        }
        const std::size_t len =
            std::min<std::size_t>(protocol::kChunkBytes, resident_bytes_.size() - *offset);

        // 6.17's attacker. A byte flipped mid-chunk: correct length, correct
        // count, correct index — nothing but the hash can catch it.
        std::vector<std::uint8_t> payload(
            static_cast<const std::uint8_t*>(
                static_cast<const void*>(resident_bytes_.data() + *offset)),
            static_cast<const std::uint8_t*>(
                static_cast<const void*>(resident_bytes_.data() + *offset)) + len);
        if (config_.serve_corrupt_assets && !payload.empty()) {
            payload[payload.size() / 2] ^= 0xFFU;
        }
        SendToPeer(protocol::EncodeAssetMsg(
            wire::AssetBody::AssetChunk, [&](flatbuffers::FlatBufferBuilder& fbb) {
                auto bytes = fbb.CreateVector(payload.data(), payload.size());
                wire::AssetChunkBuilder b(fbb);
                b.add_hash(want);
                b.add_index(i);
                b.add_total(static_cast<std::uint32_t>(total));
                b.add_bytes(bytes);
                return b.Finish();
            }));
    }
}

void TaskLoop::HandleAssetMiss(const wire::AssetMiss& /*miss*/) {
    // WHO said no decides what happens next.
    //
    // A PEER not having it is ordinary — its cache advertisement was stale by
    // the time we asked — so try the next candidate, then the coordinator. A
    // peer's "no" must not condemn the task.
    if (peer_) {
        peer_.reset();
        TryNextPeerOrFallBack();
        return;
    }
    // The COORDINATOR not having it is different: it is the authority that
    // named this asset in the grant. Waiting cannot help, so release and let it
    // hand the task to a worker that already holds the blob (2.16 affinity).
    AbandonAssetFetch("the coordinator does not have this asset");
}

void TaskLoop::FinishAssetFetch() {
    if (!fetch_) {
        return;
    }
    const std::string address = fetch_->address;

    // VERIFY BEFORE PARSE. The bytes must hash to the name we asked for — not
    // to a checksum the sender supplied, which would only prove the sender can
    // hash. In Phase 6 these arrive from an arbitrary peer and this is the only
    // thing between one and the GPU.
    const std::string actual = Blake3Hex(fetch_->bytes);
    if (actual != address) {
        // D-0097 — DISCARD AND CONTINUE, not abandon. Abandoning here let one
        // peer serving garbage cost its victim the task, repeatably.
        RejectBytesAndRetry("asset failed verification");
        return;
    }

    // VALIDATE BEFORE USE. Every index bounds-checked on the CPU, once, before
    // any of it becomes a GPU array index (D-0069/D-0070).
    auto bvh = scene::LoadBvh(fetch_->bytes);
    if (!bvh) {
        // Reachable only from the COORDINATOR: bytes that hash to the address
        // we asked for are the bytes the coordinator published, so a peer
        // cannot get here — it would have had to break BLAKE3. Retrying
        // another source cannot help, but the same call handles it correctly
        // (no peer to blame, so it falls through to abandon).
        RejectBytesAndRetry("asset hashed correctly but failed structural validation");
        return;
    }

    const auto to_bytes = [](const auto& v) {
        const auto* p = static_cast<const std::byte*>(static_cast<const void*>(v.data()));
        return std::vector<std::byte>(p, p + v.size() * sizeof(v[0]));
    };
    // Kept so this worker can SERVE the asset to peers (6.6). The parsed arrays
    // below are for our own GPU; a peer wants the bytes, and re-serialising them
    // would produce a different blob with a different hash.
    resident_bytes_ = fetch_->bytes;
    asset_nodes_ = to_bytes(bvh->nodes);
    asset_prims_ = to_bytes(bvh->prims);
    asset_materials_ = to_bytes(bvh->materials);
    // WHICH transport delivered it (6.11). Recorded here, where the answer is
    // known, rather than inferred later from whether a peer connection happens
    // to still be open — it is closed immediately below.
    asset_source_ = peer_ ? wire::AssetSource::Peer : wire::AssetSource::Coordinator;
    resident_asset_ = address;
    fetch_.reset();
    // The connection has done its job. Held open, it would be a peer we are not
    // using and a connection someone else's cap has to account for (6.12).
    peer_.reset();
    peer_target_ = protocol::WorkerId{};

    Log("info", "asset ready " + address.substr(0, 12) + "... nodes=" +
                    std::to_string(bvh->nodes.size()) + " prims=" +
                    std::to_string(bvh->prims.size()) + " depth=" +
                    std::to_string(bvh->max_depth));

    // Release the parked tasks into the run queue.
    for (auto& parked : waiting_on_asset_) {
        if (parked.asset == resident_asset_) {
            queue_.push_back(std::move(parked));
        } else {
            SendRelease(parked.id, wire::ReleaseReason::AssetUnavailable);
        }
    }
    waiting_on_asset_.clear();
}

/// Throw away bytes that failed verification and try the next source (D-0097).
///
/// The buffer RESET is the half that is easy to miss. Chunks are idempotent by
/// index — `if (have[i]) return;` — so continuing with a poisoned buffer means
/// the honest chunks that follow are dropped as duplicates, `received` stays at
/// `total`, and completion never re-fires. The fetch would hang to the stall
/// timeout and abandon anyway, and the fallback would look implemented while
/// doing nothing.
void TaskLoop::RejectBytesAndRetry(const char* why) {
    if (!fetch_) {
        return;
    }
    const bool from_peer = static_cast<bool>(peer_);
    Log("error", std::string("rejecting asset bytes from ") +
                     (from_peer ? "a peer" : "the coordinator") + ": " + why);

    if (!from_peer) {
        // The coordinator is the last source. Nothing left to fall back to.
        AbandonAssetFetch(why);
        return;
    }
    ++assets_rejected_;

    // Wipe the reassembly state, keeping only what the COORDINATOR told us
    // (D-0091). `expected_bytes` and `total` are not a peer's to influence.
    std::fill(fetch_->bytes.begin(), fetch_->bytes.end(), std::byte{0});
    std::fill(fetch_->have.begin(), fetch_->have.end(), false);
    fetch_->received = 0;
    fetch_->last_progress = Clock::now();

    // The offending peer is at `peer_attempt_ - 1` and this advances past it,
    // so it is not retried for THIS fetch. It is not reported to the
    // coordinator: a false accusation would be free, and acting on one would
    // put a trust decision in the worker (R1). See D-0097.
    TryNextPeerOrFallBack();
}

void TaskLoop::AbandonAssetFetch(const char* why) {
    Log("error", std::string("asset fetch abandoned: ") + why);
    fetch_.reset();
    peer_.reset();
    peer_target_ = protocol::WorkerId{};
    for (const auto& parked : waiting_on_asset_) {
        SendRelease(parked.id, wire::ReleaseReason::AssetUnavailable);
    }
    waiting_on_asset_.clear();
}

void TaskLoop::SendGoodbye(wire::ReleaseReason reason) {
    auto frame = protocol::EncodeMessage(
        wire::Body::Goodbye, [&](flatbuffers::FlatBufferBuilder& fbb) {
            wire::GoodbyeBuilder b(fbb);
            b.add_reason(reason);
            return b.Finish();
        });
    (void)transport_.Send(frame);
}

// ─────────────────────────────────────────────────────────────────────────
// Execution
// ─────────────────────────────────────────────────────────────────────────

bool TaskLoop::Execute(const PendingTask& task) {
    if (!device_.healthy()) {
        Log("warn", "no healthy device; releasing the task");
        SendRelease(task.id, wire::ReleaseReason::DeviceLost);
        return false;
    }

    const auto wgsl = kernels_(task.kernel_id);
    if (!wgsl) {
        Log("warn", "could not fetch WGSL for " + task.kernel_id);
        SendRelease(task.id, wire::ReleaseReason::KernelUnavailable);
        return false;
    }

    // A task with no bulk input reports `None` rather than inheriting whatever
    // the last asset-bearing task set. Done per TASK, here, because that is the
    // granularity the field describes.
    if (task.asset.empty()) {
        asset_source_ = wire::AssetSource::None;
    }

    const KernelInfo* info = FindKernel(task.kernel_id);
    if (info == nullptr) {
        SendRelease(task.id, wire::ReleaseReason::KernelUnavailable);
        return false;
    }

    TaskRequest req;
    req.wgsl = *wgsl;
    req.entry_point = info->entry_point;
    req.params = task.params;
    // The task's range, from the wire field that is its authority (D-0041).
    // This used to be decoded out of the params chunk window, and hardcoding 0
    // there meant every task searched the same range while the coordinator
    // recorded full coverage (D-0040). A named field makes that mistake look
    // wrong on sight.
    req.start_unit = task.start_unit;
    req.unit_count = task.work_units;
    req.output_bytes = task.output_bytes;
    req.workgroup_size = task.workgroup_size;
    req.workgroup_size_y = task.workgroup_size_y;
    req.invocations_x = task.invocations_x;
    req.invocations_y = task.invocations_y;

    // The bulk input, as three GPU-ready arrays. Present only when this task
    // named an asset and that asset is the resident one — checked rather than
    // assumed, because running with the wrong scene renders a plausible image
    // of something else.
    std::vector<std::span<const std::byte>> inputs;
    if (!task.asset.empty() && task.asset == resident_asset_) {
        inputs = {asset_nodes_, asset_prims_, asset_materials_};
        req.inputs = inputs;
    }
    // Once per task, before the first dispatch. Empty means zero-fill; for
    // brute_search this carries atomicMin's identity, and without it that
    // output is pinned at 0 forever (D-0040).
    req.output_init = info->output_init;

    // Renew from inside the chunk loop. Rate-limited rather than per chunk:
    // chunks are ~100 ms, and a renewal per chunk would be ten frames a second
    // per worker for no benefit.
    auto last_renew = std::chrono::steady_clock::now();
    const auto outcome = RunTask(
        device_.context(), req, ChunkUnitsFor(*info),
        [&](std::uint64_t done, std::uint64_t total) {
            // Between dispatches (R4/K1). Service peer signalling here or a
            // busy worker cannot answer an offer at all — see PumpSignalling.
            PumpSignalling();

            const auto now = std::chrono::steady_clock::now();
            if (now - last_renew < std::chrono::seconds(3)) {
                return;
            }
            last_renew = now;
            SendProgress(task.id, total > 0 ? static_cast<float>(done) /
                                                  static_cast<float>(total)
                                            : 0.0F);
        });
    if (!outcome) {
        // Could be device loss, a compile failure, or a limit we could not
        // satisfy. All of them mean the same thing to the coordinator: this
        // worker cannot do this task, take it back. RELEASE rather than let the
        // lease expire — the queue should not wait 30 s for news we already
        // have (R8).
        Log("warn", "task execution failed; releasing");
        SendRelease(task.id, device_.healthy() ? wire::ReleaseReason::ExecutionFailed
                                               : wire::ReleaseReason::DeviceLost);
        return false;
    }

    SendResult(task.id, *outcome);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// Device loss — step 1.21
// ─────────────────────────────────────────────────────────────────────────

void TaskLoop::OnDeviceLost() {
    // ORDER IS THE WHOLE POINT (recovery.hpp): release FIRST, re-acquire after.
    // Backwards, the coordinator keeps waiting on tasks this worker can no
    // longer compute, and a 200 ms driver hiccup becomes a lease-duration
    // outage for every task we were holding.
    Log("warn", "device lost; releasing held leases");
    for (const protocol::TaskId task : held_) {
        SendRelease(task, wire::ReleaseReason::DeviceLost);
    }
    held_.clear();
    queue_.clear();
    status_.device_ready = false;
    status_.contributing = false;
    status_.last_message = "device lost; recovering";
}

void TaskLoop::OnDeviceUnrecoverable() {
    // OnDeviceLost already released every lease and cleared the queue, so by
    // the time we get here we owe the coordinator nothing. What is left is to
    // say so: a `Goodbye` costs one frame and saves the coordinator a full
    // lease timeout per task, and — more importantly — it distinguishes a
    // worker that is FINISHED from one that is merely quiet. 4.10 measured
    // exactly that ambiguity from the other side.
    //
    // Order: goodbye BEFORE closing, or the frame never leaves.
    Log("error", "GPU unavailable for the rest of this page; leaving the fleet");
    SendGoodbye(wire::ReleaseReason::DeviceLost);
    transport_.Close();

    running_ = false;
    handshaked_ = false;
    lease_outstanding_ = false;
    status_.connected = false;
    status_.contributing = false;
    status_.device_ready = false;
    status_.gpu_unavailable = true;
    status_.last_message = "GPU lost and could not be recovered — reload the page to rejoin";
}

void TaskLoop::OnDeviceReady() {
    status_.device_ready = true;
    status_.last_message = "device recovered";
    Log("info", "device re-acquired; resuming");
    // Nothing else to do: Poll() asks for work again on its next turn, because
    // held_ is empty. Re-registering is not needed — the WebSocket survived the
    // device loss, and our identity is the connection.
}

const TaskLoop::KernelInfo* TaskLoop::FindKernel(std::string_view id) const {
    const auto it = std::ranges::find(kernel_info_, id,
                                      &std::pair<std::string, KernelInfo>::first);
    return it == kernel_info_.end() ? nullptr : &it->second;
}

}  // namespace p2pgpu::worker

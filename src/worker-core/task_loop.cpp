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
#include <utility>

#include "p2pgpu/protocol/encode.hpp"
#include "p2pgpu/worker/bench.hpp"
#include "p2pgpu/worker/checksum.hpp"
#include "p2pgpu/protocol/verify.hpp"

namespace p2pgpu::worker {
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

    if (!lease_outstanding_ && held_.empty()) {
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
        case wire::Body::Goodbye:
        case wire::Body::BenchmarkResult:
        case wire::Body::Signal:
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

    status_.last_message = "connected";
    Log("info", "handshake complete; kernels=" + std::to_string(kernel_info_.size()));
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

    const KernelInfo* info = FindKernel(task.kernel_id);
    if (info == nullptr) {
        Log("warn", "granted an unknown kernel: " + task.kernel_id);
        SendRelease(task.id, wire::ReleaseReason::KernelUnavailable);
        return;
    }
    task.workgroup_size = info->workgroup_size;
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

            wire::HelloBuilder hb(fbb);
            hb.add_protocol_version(protocol::kProtocolVersion);
            hb.add_resume_token(resume);
            hb.add_capabilities(caps);
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
            wire::LeaseRequestBuilder b(fbb);
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
            auto stats = sb.Finish();

            wire::ResultHeaderBuilder b(fbb);
            b.add_task_id(&tid);
            b.add_payload_bytes(static_cast<std::uint32_t>(payload.size()));
            b.add_checksum(Blake3_64(payload));
            b.add_stats(stats);
            return b.Finish();
        },
        payload);

    (void)transport_.Send(frame);
    std::erase(held_, task);
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

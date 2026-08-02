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
#include "p2pgpu/worker/checksum.hpp"
#include "p2pgpu/protocol/verify.hpp"

namespace p2pgpu::worker {
namespace {

using platform::Log;

}  // namespace

TaskLoop::TaskLoop(TaskLoopConfig config, DeviceSession& device, KernelFetcher kernels)
    : config_(std::move(config)), device_(device), kernels_(std::move(kernels)) {
    // Wired here, not in Start(), so a loss during connection is still handled.
    device_.OnLost([this] { OnDeviceLost(); });
    device_.OnReady([this] { OnDeviceReady(); });
}

TaskLoop::~TaskLoop() = default;

void TaskLoop::Start() {
    if (running_) {
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
        case wire::Body::LeaseAck:
        case wire::Body::PeerList:
        case wire::Body::BenchmarkRequest:
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
            if (const auto* out = k->output()) {
                info.output_bytes = out->bytes();
            }
            kernel_info_.emplace_back(k->kernel_id()->str(), std::move(info));
        }
    }

    status_.last_message = "connected";
    Log("info", "handshake complete; kernels=" + std::to_string(kernel_info_.size()));
}

void TaskLoop::HandleTaskGrant(const wire::TaskGrant& grant) {
    lease_outstanding_ = false;

    const auto* env = grant.envelope();
    if (env == nullptr || env->task_id() == nullptr || env->job_id() == nullptr) {
        Log("warn", "TaskGrant without an envelope or ids");
        return;
    }

    PendingTask task;
    task.id = protocol::TaskId{*env->task_id()};
    task.job = protocol::JobId{*env->job_id()};
    task.kernel_id = env->kernel_id() != nullptr ? env->kernel_id()->str() : "";
    task.work_units = env->work_units();

    if (const auto* params = env->params()) {
        // Bounded by invariant 1, which the coordinator's own Verifier pass
        // already enforced — but this buffer came off a socket, so it is copied
        // into our own storage rather than aliasing the frame, which is freed
        // as soon as this returns.
        task.params.assign(
            reinterpret_cast<const std::byte*>(params->data()),
            reinterpret_cast<const std::byte*>(params->data()) + params->size());
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
    // left to finish — the result will be discarded by the coordinator, and
    // aborting mid-dispatch buys nothing since chunking already bounds how long
    // that takes (R4).
    std::erase_if(queue_, [&](const PendingTask& t) { return t.id == id; });
    std::erase(held_, id);
    Log("info", "task revoked");
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
            hb.add_capabilities(caps);
            return hb.Finish();
        });

    (void)transport_.Send(frame);
}

void TaskLoop::RequestLease() {
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
    req.start_unit = 0;
    req.unit_count = task.work_units;
    req.output_bytes = task.output_bytes;
    req.workgroup_size = task.workgroup_size;

    const auto outcome = RunTask(device_.context(), req, config_.units_per_chunk);
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

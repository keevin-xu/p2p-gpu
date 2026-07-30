// Throughput and dispatch-overhead measurement (steps 0.10-0.12).
// Portable; no #ifdef (R2). See bench.hpp for why it lives in worker-core.

#include "p2pgpu/worker/bench.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <string>

#include "p2pgpu/worker/wgpu_util.hpp"

namespace p2pgpu::worker {
namespace {

using platform::Log;

// Params parity with kernels/calibrate.wgsl (CONVENTIONS.md §5). WGSL lays the
// struct out as two tightly packed u32s; if either side gains a field, this
// fires at compile time instead of producing garbage at runtime.
static_assert(sizeof(CalibrateParams) == 8, "CalibrateParams must match WGSL Params");
static_assert(offsetof(CalibrateParams, iterations) == 0);
static_assert(offsetof(CalibrateParams, seed) == 4);

constexpr std::uint32_t kWorkgroupSize = 256;

template <typename T, void (*Release)(T)>
class Handle {
public:
    Handle() = default;
    explicit Handle(T h) noexcept : h_(h) {}
    ~Handle() { if (h_ != nullptr) { Release(h_); } }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) { if (h_ != nullptr) { Release(h_); } h_ = o.h_; o.h_ = nullptr; }
        return *this;
    }
    [[nodiscard]] T get() const noexcept { return h_; }
    [[nodiscard]] explicit operator bool() const noexcept { return h_ != nullptr; }
private:
    T h_ = nullptr;
};

using ShaderModule    = Handle<WGPUShaderModule, wgpuShaderModuleRelease>;
using ComputePipeline = Handle<WGPUComputePipeline, wgpuComputePipelineRelease>;
using BindGroupLayout = Handle<WGPUBindGroupLayout, wgpuBindGroupLayoutRelease>;
using BindGroup       = Handle<WGPUBindGroup, wgpuBindGroupRelease>;
using Buffer          = Handle<WGPUBuffer, wgpuBufferRelease>;
using CommandEncoder  = Handle<WGPUCommandEncoder, wgpuCommandEncoderRelease>;
using ComputePass     = Handle<WGPUComputePassEncoder, wgpuComputePassEncoderRelease>;
using CommandBuffer   = Handle<WGPUCommandBuffer, wgpuCommandBufferRelease>;

struct DoneFlag { bool done = false; };

void WorkDoneThunk(WGPUQueueWorkDoneStatus, WGPUStringView, void* ud, void*) {
    static_cast<DoneFlag*>(ud)->done = true;
}

/// Compiled pipeline plus its buffers, reused across a sweep so setup cost
/// never lands inside a timed region.
struct Prepared {
    ShaderModule module;
    ComputePipeline pipeline;
    BindGroupLayout layout;
    Buffer params_buf;
    Buffer output_buf;
    BindGroup bind_group;
    bool ok = false;
};

Prepared Prepare(const platform::GpuContext& ctx, std::string_view wgsl,
                 std::uint32_t workgroups) {
    Prepared p;

    WGPUShaderSourceWGSL src{};
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = WGPUStringView{wgsl.data(), wgsl.size()};

    WGPUShaderModuleDescriptor md{};
    md.nextInChain = &src.chain;
    md.label = wgpu::Str("calibrate");
    p.module = ShaderModule{wgpuDeviceCreateShaderModule(ctx.device, &md)};
    if (!p.module) { Log("error", "calibrate: shader module failed"); return p; }

    WGPUComputePipelineDescriptor pd{};
    pd.label = wgpu::Str("calibrate");
    pd.compute.module = p.module.get();
    pd.compute.entryPoint = wgpu::Str("main");
    p.pipeline = ComputePipeline{wgpuDeviceCreateComputePipeline(ctx.device, &pd)};
    if (!p.pipeline) { Log("error", "calibrate: pipeline failed"); return p; }

    WGPUBufferDescriptor pbd{};
    pbd.label = wgpu::Str("params");
    pbd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    pbd.size = sizeof(CalibrateParams);
    p.params_buf = Buffer{wgpuDeviceCreateBuffer(ctx.device, &pbd)};

    WGPUBufferDescriptor obd{};
    obd.label = wgpu::Str("output");
    obd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc;
    obd.size = static_cast<std::uint64_t>(workgroups) * 4U;
    p.output_buf = Buffer{wgpuDeviceCreateBuffer(ctx.device, &obd)};

    if (!p.params_buf || !p.output_buf) {
        Log("error", "calibrate: buffer allocation failed");
        return p;
    }

    p.layout = BindGroupLayout{wgpuComputePipelineGetBindGroupLayout(p.pipeline.get(), 0)};
    std::array<WGPUBindGroupEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].buffer = p.params_buf.get();
    entries[0].size = sizeof(CalibrateParams);
    entries[1].binding = 1;
    entries[1].buffer = p.output_buf.get();
    entries[1].size = obd.size;

    WGPUBindGroupDescriptor bd{};
    bd.layout = p.layout.get();
    bd.entryCount = entries.size();
    bd.entries = entries.data();
    p.bind_group = BindGroup{wgpuDeviceCreateBindGroup(ctx.device, &bd)};
    if (!p.bind_group) { Log("error", "calibrate: bind group failed"); return p; }

    p.ok = true;
    return p;
}

/// Submit `dispatches` passes and block until the GPU has finished all of them.
/// Returns host-observed milliseconds.
///
/// Waiting for actual completion is essential: wgpuQueueSubmit only *enqueues*.
/// Timing without the wait measures how fast we can fill a queue, which is a
/// number that looks impressive and means nothing.
double TimedSubmit(const platform::GpuContext& ctx, const Prepared& p,
                   std::uint32_t workgroups, std::uint32_t dispatches) {
    CommandEncoder enc{wgpuDeviceCreateCommandEncoder(ctx.device, nullptr)};
    if (!enc) { return -1.0; }

    for (std::uint32_t d = 0; d < dispatches; ++d) {
        ComputePass pass{wgpuCommandEncoderBeginComputePass(enc.get(), nullptr)};
        if (!pass) { return -1.0; }
        wgpuComputePassEncoderSetPipeline(pass.get(), p.pipeline.get());
        wgpuComputePassEncoderSetBindGroup(pass.get(), 0, p.bind_group.get(), 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(pass.get(), workgroups, 1, 1);
        wgpuComputePassEncoderEnd(pass.get());
    }

    CommandBuffer cmds{wgpuCommandEncoderFinish(enc.get(), nullptr)};
    if (!cmds) { return -1.0; }

    DoneFlag flag;
    WGPUQueueWorkDoneCallbackInfo cb{};
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = WorkDoneThunk;
    cb.userdata1 = &flag;

    const auto t0 = std::chrono::steady_clock::now();
    WGPUCommandBuffer raw = cmds.get();
    wgpuQueueSubmit(ctx.queue, 1, &raw);
    (void)wgpuQueueOnSubmittedWorkDone(ctx.queue, cb);

    if (!platform::WaitUntil(ctx, [&] { return flag.done; }, 60000)) {
        Log("error", "calibrate: timed out waiting for GPU");
        return -1.0;
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

}  // namespace

std::vector<BenchSample> RunCalibration(const platform::GpuContext& ctx,
                                        std::string_view wgsl,
                                        const std::vector<std::uint32_t>& invocation_counts,
                                        std::uint32_t iterations) {
    std::vector<BenchSample> samples;
    if (!ctx.valid()) { return samples; }

    for (const std::uint32_t invocations : invocation_counts) {
        const std::uint32_t workgroups =
            (invocations + kWorkgroupSize - 1) / kWorkgroupSize;

        // K4: never assume the dispatch fits. 65535 per dimension is the common
        // ceiling and a real device may be lower.
        WGPULimits limits{};
        if (wgpuDeviceGetLimits(ctx.device, &limits) == WGPUStatus_Success &&
            workgroups > limits.maxComputeWorkgroupsPerDimension) {
            Log("warn", "skipping size: needs " + std::to_string(workgroups) +
                            " workgroups, limit is " +
                            std::to_string(limits.maxComputeWorkgroupsPerDimension));
            continue;
        }

        Prepared p = Prepare(ctx, wgsl, workgroups);
        if (!p.ok) { continue; }

        const CalibrateParams params{iterations, 0x9E3779B9U};
        const auto raw = std::bit_cast<std::array<std::byte, sizeof(CalibrateParams)>>(params);
        wgpuQueueWriteBuffer(ctx.queue, p.params_buf.get(), 0, raw.data(), raw.size());

        // Warm up: first dispatch pays shader compilation and lazy driver
        // allocation. Timing it would report the compiler, not the GPU.
        (void)TimedSubmit(ctx, p, workgroups, 1);

        // Several dispatches per timed batch so fixed submission overhead
        // amortises and the measurement reflects compute.
        constexpr std::uint32_t kDispatches = 10;
        double best_ms = -1.0;
        for (int trial = 0; trial < 3; ++trial) {
            const double ms = TimedSubmit(ctx, p, workgroups, kDispatches);
            if (ms >= 0.0 && (best_ms < 0.0 || ms < best_ms)) { best_ms = ms; }
        }
        if (best_ms < 0.0) { continue; }

        BenchSample s;
        s.invocations = workgroups * kWorkgroupSize;   // actual, after rounding up
        s.iterations = iterations;
        s.dispatches = kDispatches;
        // Exactly the D-0018 derivation: 4 FMAs x 2 FLOP per pass per invocation.
        s.flops = static_cast<std::uint64_t>(s.invocations) * iterations * 8ULL *
                  kDispatches;
        s.wall_ms = best_ms;
        s.gflops = static_cast<double>(s.flops) / (best_ms * 1.0e6);
        samples.push_back(s);
    }
    return samples;
}

double MeasureDispatchOverheadUs(const platform::GpuContext& ctx,
                                 std::string_view wgsl,
                                 std::uint32_t repetitions) {
    if (!ctx.valid() || repetitions == 0) { return -1.0; }

    // One workgroup, one iteration: as close to zero compute as a real dispatch
    // gets, so what remains is encoding and driver cost.
    Prepared p = Prepare(ctx, wgsl, 1);
    if (!p.ok) { return -1.0; }

    const CalibrateParams params{1U, 1U};
    const auto raw = std::bit_cast<std::array<std::byte, sizeof(CalibrateParams)>>(params);
    wgpuQueueWriteBuffer(ctx.queue, p.params_buf.get(), 0, raw.data(), raw.size());

    (void)TimedSubmit(ctx, p, 1, 1);   // warm up

    // N dispatches, ONE submit: divides out to the marginal cost of an extra
    // dispatch, with the single submission amortised away.
    double best_us = -1.0;
    for (int trial = 0; trial < 3; ++trial) {
        const double ms = TimedSubmit(ctx, p, 1, repetitions);
        if (ms < 0.0) { continue; }
        const double us = (ms * 1000.0) / static_cast<double>(repetitions);
        if (best_us < 0.0 || us < best_us) { best_us = us; }
    }
    return best_us;
}

double MeasureSubmitOverheadUs(const platform::GpuContext& ctx,
                               std::string_view wgsl,
                               std::uint32_t repetitions) {
    if (!ctx.valid() || repetitions == 0) { return -1.0; }

    Prepared p = Prepare(ctx, wgsl, 1);
    if (!p.ok) { return -1.0; }

    const CalibrateParams params{1U, 1U};
    const auto raw = std::bit_cast<std::array<std::byte, sizeof(CalibrateParams)>>(params);
    wgpuQueueWriteBuffer(ctx.queue, p.params_buf.get(), 0, raw.data(), raw.size());

    (void)TimedSubmit(ctx, p, 1, 1);   // warm up

    // ONE dispatch per submit, repeated: each iteration pays the full encode →
    // submit → wait-for-completion round trip, which is the cost a real task
    // cannot avoid. Median rather than min, because the tail here is real
    // scheduling variance a worker would actually experience, not noise.
    std::vector<double> samples;
    samples.reserve(repetitions);
    for (std::uint32_t i = 0; i < repetitions; ++i) {
        const double ms = TimedSubmit(ctx, p, 1, 1);
        if (ms >= 0.0) { samples.push_back(ms * 1000.0); }
    }
    if (samples.empty()) { return -1.0; }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

}  // namespace p2pgpu::worker

// Portable. Fetch WGSL by kernel id, compile, allocate against QUERIED limits
// (K4 — never hardcode), dispatch in <=250ms chunks with yields (R4/K1),
// read back, populate every TaskStats field.
// Written once, compiled to both targets — browser and native cannot drift.
// Full task execution lands in Phase 1 step 1.19.
//
// Step 0.8 establishes the shape with RunUnaryF32Kernel: one dispatch, one
// readback, one assertion. THERE IS NO #ifdef IN THIS FILE AND THERE MUST NEVER
// BE ONE (R2) — every platform difference goes through the seam.

#include "p2pgpu/worker/kernel_host.hpp"

#include <algorithm>
#include <string>

namespace p2pgpu::worker {
namespace {

using platform::Log;

constexpr WGPUStringView Str(std::string_view s) noexcept {
    return WGPUStringView{s.data(), s.size()};
}

/// RAII for the WGPU handles in this function. WebGPU objects are refcounted
/// with explicit Release, and this routine has ~8 early-exit paths; hand-rolled
/// cleanup would leak on at least one of them (CONVENTIONS.md §3: RAII for
/// everything, no manual cleanup paths).
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
        if (this != &o) {
            if (h_ != nullptr) { Release(h_); }
            h_ = o.h_;
            o.h_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] T get() const noexcept { return h_; }
    [[nodiscard]] explicit operator bool() const noexcept { return h_ != nullptr; }

private:
    T h_ = nullptr;
};

using ShaderModule    = Handle<WGPUShaderModule,    wgpuShaderModuleRelease>;
using ComputePipeline = Handle<WGPUComputePipeline, wgpuComputePipelineRelease>;
using BindGroupLayout = Handle<WGPUBindGroupLayout, wgpuBindGroupLayoutRelease>;
using BindGroup       = Handle<WGPUBindGroup,       wgpuBindGroupRelease>;
using Buffer          = Handle<WGPUBuffer,          wgpuBufferRelease>;
using CommandEncoder  = Handle<WGPUCommandEncoder,  wgpuCommandEncoderRelease>;
using ComputePass     = Handle<WGPUComputePassEncoder, wgpuComputePassEncoderRelease>;
using CommandBuffer   = Handle<WGPUCommandBuffer,   wgpuCommandBufferRelease>;

struct MapResult {
    WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Error;
    bool done = false;
};

void MapThunk(WGPUMapAsyncStatus status, WGPUStringView, void* ud, void*) {
    auto* r = static_cast<MapResult*>(ud);
    r->status = status;
    r->done = true;
}

}  // namespace

std::optional<std::vector<std::byte>> RunUnaryKernel(
    const platform::GpuContext& ctx,
    std::string_view wgsl_source,
    std::string_view entry_point,
    std::span<const std::byte> input,
    std::uint32_t element_count,
    std::uint32_t workgroup_size) {

    if (!ctx.valid() || input.empty() || element_count == 0 || workgroup_size == 0) {
        Log("error", "RunUnaryKernel: invalid arguments");
        return std::nullopt;
    }

    // size_t, not uint64_t. webgpu.h descriptors take uint64_t sizes but the
    // *functions* (WriteBuffer, MapAsync, GetConstMappedRange) take size_t —
    // which is 32-bit on wasm32 and 64-bit natively. Holding this as size_t
    // means every conversion is a silent WIDENING; holding it as uint64_t
    // meant a narrowing that only warned on the browser target.
    const std::size_t byte_size = input.size_bytes();

    // K4: check the DEVICE's real limits, never a hardcoded assumption. A
    // buffer that fits on this Mac may not fit on a stranger's phone.
    WGPULimits limits{};
    if (wgpuDeviceGetLimits(ctx.device, &limits) == WGPUStatus_Success) {
        if (byte_size > limits.maxStorageBufferBindingSize) {
            Log("error", "input exceeds maxStorageBufferBindingSize (" +
                             std::to_string(limits.maxStorageBufferBindingSize) + ")");
            return std::nullopt;
        }
    }

    // ── shader module ──
    WGPUShaderSourceWGSL wgsl{};
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code = Str(wgsl_source);

    WGPUShaderModuleDescriptor module_desc{};
    module_desc.nextInChain = &wgsl.chain;
    module_desc.label = Str("p2pgpu-kernel");

    ShaderModule module{wgpuDeviceCreateShaderModule(ctx.device, &module_desc)};
    if (!module) {
        Log("error", "shader module creation failed");
        return std::nullopt;
    }

    // ── pipeline ──
    const std::string entry{entry_point};
    WGPUComputePipelineDescriptor pipe_desc{};
    pipe_desc.label = Str("p2pgpu-pipeline");
    pipe_desc.compute.module = module.get();
    pipe_desc.compute.entryPoint = Str(entry);

    ComputePipeline pipeline{wgpuDeviceCreateComputePipeline(ctx.device, &pipe_desc)};
    if (!pipeline) {
        Log("error", "compute pipeline creation failed");
        return std::nullopt;
    }

    // ── buffers ──
    WGPUBufferDescriptor in_desc{};
    in_desc.label = Str("input");
    in_desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    in_desc.size = byte_size;
    Buffer in_buf{wgpuDeviceCreateBuffer(ctx.device, &in_desc)};

    WGPUBufferDescriptor out_desc{};
    out_desc.label = Str("output");
    out_desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc;
    out_desc.size = byte_size;
    Buffer out_buf{wgpuDeviceCreateBuffer(ctx.device, &out_desc)};

    // Storage buffers cannot be mapped, so results land in a separate staging
    // buffer that can. On Apple's unified memory the copy is nearly free; on a
    // discrete GPU it is a real PCIe transfer — do not design as if readback is
    // free (docs/RISKS.md §1).
    WGPUBufferDescriptor stage_desc{};
    stage_desc.label = Str("staging");
    stage_desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    stage_desc.size = byte_size;
    Buffer stage_buf{wgpuDeviceCreateBuffer(ctx.device, &stage_desc)};

    if (!in_buf || !out_buf || !stage_buf) {
        Log("error", "buffer allocation failed");
        return std::nullopt;
    }

    wgpuQueueWriteBuffer(ctx.queue, in_buf.get(), 0, input.data(), byte_size);

    // ── bind group ──
    BindGroupLayout layout{wgpuComputePipelineGetBindGroupLayout(pipeline.get(), 0)};
    if (!layout) {
        Log("error", "bind group layout query failed");
        return std::nullopt;
    }

    WGPUBindGroupEntry entries[2]{};
    entries[0].binding = 0;
    entries[0].buffer = in_buf.get();
    entries[0].size = byte_size;
    entries[1].binding = 1;
    entries[1].buffer = out_buf.get();
    entries[1].size = byte_size;

    WGPUBindGroupDescriptor bg_desc{};
    bg_desc.label = Str("p2pgpu-bindgroup");
    bg_desc.layout = layout.get();
    bg_desc.entryCount = 2;
    bg_desc.entries = entries;

    BindGroup bind_group{wgpuDeviceCreateBindGroup(ctx.device, &bg_desc)};
    if (!bind_group) {
        Log("error", "bind group creation failed");
        return std::nullopt;
    }

    // ── encode + dispatch ──
    CommandEncoder encoder{wgpuDeviceCreateCommandEncoder(ctx.device, nullptr)};
    if (!encoder) {
        Log("error", "command encoder creation failed");
        return std::nullopt;
    }

    {
        ComputePass pass{wgpuCommandEncoderBeginComputePass(encoder.get(), nullptr)};
        if (!pass) {
            Log("error", "compute pass creation failed");
            return std::nullopt;
        }
        wgpuComputePassEncoderSetPipeline(pass.get(), pipeline.get());
        wgpuComputePassEncoderSetBindGroup(pass.get(), 0, bind_group.get(), 0, nullptr);

        // Round up so the tail elements get a thread. The kernel bounds-checks
        // the overhang against arrayLength (K1).
        const std::uint32_t groups = (element_count + workgroup_size - 1) / workgroup_size;
        wgpuComputePassEncoderDispatchWorkgroups(pass.get(), groups, 1, 1);
        wgpuComputePassEncoderEnd(pass.get());
    }

    wgpuCommandEncoderCopyBufferToBuffer(encoder.get(), out_buf.get(), 0,
                                         stage_buf.get(), 0, byte_size);

    CommandBuffer commands{wgpuCommandEncoderFinish(encoder.get(), nullptr)};
    if (!commands) {
        Log("error", "command buffer finish failed");
        return std::nullopt;
    }
    WGPUCommandBuffer raw = commands.get();
    wgpuQueueSubmit(ctx.queue, 1, &raw);

    // ── readback ──
    MapResult map_result;
    WGPUBufferMapCallbackInfo map_cb{};
    map_cb.mode = WGPUCallbackMode_AllowProcessEvents;
    map_cb.callback = MapThunk;
    map_cb.userdata1 = &map_result;

    (void)wgpuBufferMapAsync(stage_buf.get(), WGPUMapMode_Read, 0,
                             byte_size, map_cb);

    // Polls natively, yields to the event loop in the browser — the seam
    // decides which, and this line reads the same either way.
    if (!platform::WaitUntil(ctx, [&] { return map_result.done; })) {
        Log("error", "buffer map timed out");
        return std::nullopt;
    }
    if (map_result.status != WGPUMapAsyncStatus_Success) {
        Log("error", "buffer map failed with status " +
                         std::to_string(static_cast<int>(map_result.status)));
        return std::nullopt;
    }

    const void* mapped = wgpuBufferGetConstMappedRange(
        stage_buf.get(), 0, byte_size);
    if (mapped == nullptr) {
        Log("error", "mapped range was null");
        wgpuBufferUnmap(stage_buf.get());
        return std::nullopt;
    }

    // Size is ours (we allocated it), not attacker-supplied, so this is a
    // bounded copy out of our own staging buffer.
    const std::span<const std::byte> mapped_view{static_cast<const std::byte*>(mapped),
                                                 byte_size};
    std::vector<std::byte> result(byte_size);
    std::ranges::copy(mapped_view, result.begin());

    wgpuBufferUnmap(stage_buf.get());
    return result;
}

}  // namespace p2pgpu::worker

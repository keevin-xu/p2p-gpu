// Portable. Compile, allocate against QUERIED limits (K4 — never hardcode),
// dispatch in <=250ms chunks with yields (R4/K1), read back, populate every
// TaskStats field.
// Written once, compiled to both targets — browser and native cannot drift.
//
// RunUnaryKernel (step 0.8) is the smoke shape: one dispatch, one readback.
// RunTask (step 1.19) is the real one. THERE IS NO #ifdef IN THIS FILE AND
// THERE MUST NEVER BE ONE (R2) — every platform difference goes through the
// seam, and tools/check_seam.py fails the build if one appears.
//
// Note what is deliberately absent: acquiring the WGSL. Native reads it from
// disk, the browser fetches it from the coordinator over HTTP (step 1.12), so
// the source text is passed IN. That is what keeps this file portable.

#include "p2pgpu/worker/kernel_host.hpp"

#include <algorithm>
#include <cstring>
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
using QuerySet        = Handle<WGPUQuerySet,        wgpuQuerySetRelease>;
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

namespace {

struct ErrorScopeResult {
    bool done = false;
    bool had_error = false;
    std::string message;
};

void ErrorScopeThunk(WGPUPopErrorScopeStatus, WGPUErrorType type,
                     WGPUStringView message, void* user1, void*) {
    auto* r = static_cast<ErrorScopeResult*>(user1);
    r->had_error = (type != WGPUErrorType_NoError);
    // This file keeps its own tiny string helpers (see `Str` above) rather
    // than pulling in wgpu_util.hpp; `length` may be WGPU_STRLEN, meaning the
    // view happens to be NUL-terminated after all, so both cases need handling.
    if (message.data != nullptr) {
        r->message = (message.length == WGPU_STRLEN)
                         ? std::string(message.data)
                         : std::string(message.data, message.length);
    }
    r->done = true;
}

}  // namespace

bool CompileKernel(const platform::GpuContext& ctx, std::string_view wgsl_source,
                   std::string_view entry_point) {
    if (!ctx.valid()) {
        return false;
    }

    // ── AN ERROR SCOPE, BECAUSE THE HANDLES LIE ─────────────────────────
    // The first version of this function checked the returned handles for null
    // and returned true otherwise. It reported SUCCESS for the literal string
    // "this is not wgsl" — WebGPU surfaces validation failures through the
    // asynchronous error callback and still hands back a non-null object, so
    // there is nothing at the call site to see.
    //
    // Caught by the deliberately-broken case in tests/kernels/test_compile.cpp
    // on its first run. Without that case this would have been a compile check
    // that could not fail, which is exactly D-0067's shape.
    wgpuDevicePushErrorScope(ctx.device, WGPUErrorFilter_Validation);

    WGPUShaderSourceWGSL wgsl{};
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code = Str(wgsl_source);

    WGPUShaderModuleDescriptor module_desc{};
    module_desc.nextInChain = &wgsl.chain;
    module_desc.label = Str("p2pgpu-compile-check");
    ShaderModule module{wgpuDeviceCreateShaderModule(ctx.device, &module_desc)};

    // The PIPELINE too, inside the same scope: a module can compile while its
    // entry point is missing or its bindings do not form a valid layout, and
    // those are the failures that would otherwise surface on a stranger's GPU.
    ComputePipeline pipeline;
    if (module) {
        const std::string entry{entry_point};
        WGPUComputePipelineDescriptor pipe_desc{};
        pipe_desc.label = Str("p2pgpu-compile-check");
        pipe_desc.compute.module = module.get();
        pipe_desc.compute.entryPoint = Str(entry);
        pipeline = ComputePipeline{
            wgpuDeviceCreateComputePipeline(ctx.device, &pipe_desc)};
    }

    ErrorScopeResult scope;
    WGPUPopErrorScopeCallbackInfo cb{};
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = ErrorScopeThunk;
    cb.userdata1 = &scope;
    (void)wgpuDevicePopErrorScope(ctx.device, cb);

    if (!platform::WaitUntil(ctx, [&] { return scope.done; })) {
        Log("error", "compile check: error scope timed out");
        return false;
    }
    if (scope.had_error) {
        Log("error", "kernel failed to compile: " + scope.message);
        return false;
    }
    return module && pipeline;
}

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

    // GUARD, not tidiness (D-0022). On a lost device the create calls return
    // non-null but INVALID objects, so the null checks below all pass, and then
    // wgpuQueueSubmit panics inside wgpu-native and aborts the whole process. A
    // Rust panic cannot be caught from C++, so the only defence is to not make
    // the call. Device loss must degrade to a failed task, never a dead worker.
    if (platform::DeviceIsLost()) {
        Log("warn", "RunUnaryKernel: device is lost; refusing to submit");
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


// ─────────────────────────────────────────────────────────────────────────
// RunTask — step 1.19
// ─────────────────────────────────────────────────────────────────────────
namespace {

/// Uniform buffers must be a multiple of 16 bytes and are bound at 16-byte
/// alignment. The coordinator's params blob is not required to be, so the host
/// rounds up and zero-pads rather than rejecting a 12-byte struct.
constexpr std::size_t kUniformAlign = 16;

constexpr std::size_t RoundUpTo(std::size_t n, std::size_t multiple) noexcept {
    return ((n + multiple - 1) / multiple) * multiple;
}

/// Write the K1 chunk window into the params image (D-0033).
///
/// Bytes 0..3 = start_unit, 4..7 = unit_count, little-endian, both u32. The
/// range is 64-bit at the JOB level but a chunk is bounded by
/// maxComputeWorkgroupsPerDimension * workgroup_size, which is far under 2^32 —
/// so the truncation here is not one, and the caller has already clamped.
void WriteChunkWindow(std::span<std::byte> params, std::uint32_t start_unit,
                      std::uint32_t unit_count) noexcept {
    // Byte-at-a-time rather than a struct overlay: the destination is a byte
    // image with no declared type, and this is endian-explicit so a big-endian
    // host would still produce the little-endian bytes WGSL expects.
    for (std::size_t i = 0; i < 4; ++i) {
        params[i] = static_cast<std::byte>((start_unit >> (8U * i)) & 0xFFU);
        params[4 + i] = static_cast<std::byte>((unit_count >> (8U * i)) & 0xFFU);
    }
}

double MillisBetween(std::chrono::steady_clock::time_point a,
                     std::chrono::steady_clock::time_point b) noexcept {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

}  // namespace

namespace {

struct TimestampPair {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

/// Map the resolved timestamp buffer. Returns false on any failure, and the
/// caller then falls back to submit time — a timing feature must never be able
/// to fail a task (K6).
bool ReadTimestamps(const platform::GpuContext& ctx, const Buffer& ts_read,
                    TimestampPair& out) {
    MapResult map_result;
    WGPUBufferMapCallbackInfo cb{};
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = MapThunk;
    cb.userdata1 = &map_result;
    (void)wgpuBufferMapAsync(ts_read.get(), WGPUMapMode_Read, 0,
                             2 * sizeof(std::uint64_t), cb);
    if (!platform::WaitUntil(ctx, [&] { return map_result.done; })) {
        return false;
    }
    if (map_result.status != WGPUMapAsyncStatus_Success) {
        return false;
    }
    const void* mapped = wgpuBufferGetConstMappedRange(ts_read.get(), 0,
                                                       2 * sizeof(std::uint64_t));
    if (mapped == nullptr) {
        wgpuBufferUnmap(ts_read.get());
        return false;
    }
    const auto* bytes = static_cast<const std::byte*>(mapped);
    const auto read_u64 = [&](std::size_t at) {
        std::array<std::byte, 8> raw{};
        for (std::size_t i = 0; i < 8; ++i) {
            raw[i] = bytes[at + i];
        }
        return std::bit_cast<std::uint64_t>(raw);
    };
    out.begin = read_u64(0);
    out.end = read_u64(8);
    wgpuBufferUnmap(ts_read.get());
    return true;
}

}  // namespace

std::optional<TaskOutcome> RunTask(const platform::GpuContext& ctx,
                                   const TaskRequest& req,
                                   std::uint64_t units_per_chunk,
                                   const ChunkCallback& on_chunk) {
    if (!ctx.valid() || req.unit_count == 0 || req.output_bytes == 0 ||
        req.workgroup_size == 0) {
        Log("error", "RunTask: invalid arguments");
        return std::nullopt;
    }
    // The chunk window lives at bytes 0..7, so there must BE 8 bytes (D-0033).
    if (req.params.size() < 8) {
        Log("error", "RunTask: params smaller than the chunk window");
        return std::nullopt;
    }
    if (!req.output_init.empty() && req.output_init.size() != req.output_bytes) {
        Log("error", "RunTask: output_init size disagrees with output_bytes");
        return std::nullopt;
    }

    // GUARD, not tidiness (D-0022). See RunUnaryKernel above: on a lost device
    // the create calls return non-null but INVALID objects, and the subsequent
    // submit aborts the process from inside Rust where C++ cannot catch it.
    if (platform::DeviceIsLost()) {
        Log("warn", "RunTask: device is lost; refusing to submit");
        return std::nullopt;
    }

    // ── K4: query the real device, never assume ──
    WGPULimits limits{};
    if (wgpuDeviceGetLimits(ctx.device, &limits) != WGPUStatus_Success) {
        Log("error", "RunTask: could not query device limits");
        return std::nullopt;
    }
    if (req.output_bytes > limits.maxStorageBufferBindingSize) {
        Log("error", "output exceeds maxStorageBufferBindingSize (" +
                         std::to_string(limits.maxStorageBufferBindingSize) + ")");
        return std::nullopt;
    }
    const std::size_t params_bytes = RoundUpTo(req.params.size(), kUniformAlign);
    if (params_bytes > limits.maxUniformBufferBindingSize) {
        Log("error", "params exceed maxUniformBufferBindingSize");
        return std::nullopt;
    }

    // A chunk can never dispatch more workgroups than the device allows in one
    // dimension. This is a HARD ceiling on top of whatever the caller asked
    // for — exceeding it is a validation error, not a slow dispatch.
    const std::uint64_t max_units_per_dispatch =
        static_cast<std::uint64_t>(limits.maxComputeWorkgroupsPerDimension) *
        req.workgroup_size;

    std::uint64_t chunk = (units_per_chunk == 0) ? req.unit_count : units_per_chunk;
    chunk = std::min({chunk, req.unit_count, max_units_per_dispatch});
    if (chunk == 0) {
        Log("error", "RunTask: computed a zero-unit chunk");
        return std::nullopt;
    }

    // ── compile ONCE, reuse for every chunk ──
    //
    // Compilation is tens of milliseconds and would otherwise dominate a task
    // split into many short dispatches — the very shape R4 forces on us.
    WGPUShaderSourceWGSL wgsl{};
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code = Str(req.wgsl);

    WGPUShaderModuleDescriptor module_desc{};
    module_desc.nextInChain = &wgsl.chain;
    module_desc.label = Str("p2pgpu-task");

    ShaderModule module{wgpuDeviceCreateShaderModule(ctx.device, &module_desc)};
    if (!module) {
        Log("error", "shader module creation failed");
        return std::nullopt;
    }

    const std::string entry{req.entry_point};
    WGPUComputePipelineDescriptor pipe_desc{};
    pipe_desc.label = Str("p2pgpu-task-pipeline");
    pipe_desc.compute.module = module.get();
    pipe_desc.compute.entryPoint = Str(entry);

    ComputePipeline pipeline{wgpuDeviceCreateComputePipeline(ctx.device, &pipe_desc)};
    if (!pipeline) {
        Log("error", "compute pipeline creation failed");
        return std::nullopt;
    }

    // ── buffers, allocated ONCE for the whole task ──
    WGPUBufferDescriptor params_desc{};
    params_desc.label = Str("params");
    params_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    params_desc.size = params_bytes;
    Buffer params_buf{wgpuDeviceCreateBuffer(ctx.device, &params_desc)};

    WGPUBufferDescriptor out_desc{};
    out_desc.label = Str("result");
    out_desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc |
                     WGPUBufferUsage_CopyDst;
    out_desc.size = req.output_bytes;
    Buffer out_buf{wgpuDeviceCreateBuffer(ctx.device, &out_desc)};

    WGPUBufferDescriptor stage_desc{};
    stage_desc.label = Str("staging");
    stage_desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    stage_desc.size = req.output_bytes;
    Buffer stage_buf{wgpuDeviceCreateBuffer(ctx.device, &stage_desc)};

    if (!params_buf || !out_buf || !stage_buf) {
        Log("error", "buffer allocation failed");
        return std::nullopt;
    }

    // ── initialise the result buffer ONCE PER TASK ──
    //
    // NOT per chunk. brute_search_v1 needs min_match = 0xFFFFFFFF as atomicMin's
    // identity, and chunks ACCUMULATE into this buffer — that is what makes one
    // dispatch of N and four of N/4 produce identical bytes (step 4.2's chunk
    // invariance test). Re-initialising between chunks would look like
    // scheduling nondeterminism, which is the most expensive possible
    // misdiagnosis in a system whose verification story is determinism (R6).
    {
        std::vector<std::byte> init(req.output_bytes, std::byte{0});
        if (!req.output_init.empty()) {
            std::ranges::copy(req.output_init, init.begin());
        }
        wgpuQueueWriteBuffer(ctx.queue, out_buf.get(), 0, init.data(), init.size());
    }

    // ── bind group, built once; only the params CONTENTS change per chunk ──
    BindGroupLayout layout{wgpuComputePipelineGetBindGroupLayout(pipeline.get(), 0)};
    if (!layout) {
        Log("error", "bind group layout query failed");
        return std::nullopt;
    }

    // ── read-only storage inputs, bound at 2, 3, 4… (D-0072) ──
    //
    // Uploaded ONCE per task, before the chunk loop: a BVH is the same for
    // every chunk, and re-uploading megabytes per dispatch would put transfer
    // back into the inner loop that R5's whole argument is about keeping
    // compute-bound.
    //
    // The host attaches no meaning to these. Slicing one fetched asset into
    // this list is the caller's job — see the header.
    std::vector<Buffer> input_buffers;
    input_buffers.reserve(req.inputs.size());
    for (const std::span<const std::byte>& bytes : req.inputs) {
        if (bytes.empty()) {
            Log("error", "an input buffer is empty; refusing to bind it");
            return std::nullopt;
        }
        if (bytes.size() > limits.maxStorageBufferBindingSize) {
            // K4: the device's REAL limit, queried, not a hardcoded guess. This
            // is the check that fires on a stranger's low-end GPU.
            Log("error", "input buffer exceeds maxStorageBufferBindingSize (" +
                             std::to_string(bytes.size()) + " > " +
                             std::to_string(limits.maxStorageBufferBindingSize) + ")");
            return std::nullopt;
        }
        WGPUBufferDescriptor desc{};
        desc.label = Str("p2pgpu-task-input");
        desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
        desc.size = bytes.size();
        Buffer buf{wgpuDeviceCreateBuffer(ctx.device, &desc)};
        if (!buf) {
            Log("error", "input buffer allocation failed");
            return std::nullopt;
        }
        wgpuQueueWriteBuffer(ctx.queue, buf.get(), 0, bytes.data(), bytes.size());
        input_buffers.push_back(std::move(buf));
    }

    std::vector<WGPUBindGroupEntry> entries(2 + input_buffers.size());
    entries[0].binding = 0;
    entries[0].buffer = params_buf.get();
    entries[0].size = params_bytes;
    entries[1].binding = 1;
    entries[1].buffer = out_buf.get();
    entries[1].size = req.output_bytes;
    for (std::size_t i = 0; i < input_buffers.size(); ++i) {
        entries[2 + i].binding = static_cast<std::uint32_t>(2 + i);
        entries[2 + i].buffer = input_buffers[i].get();
        entries[2 + i].size = req.inputs[i].size();
    }

    WGPUBindGroupDescriptor bg_desc{};
    bg_desc.label = Str("p2pgpu-task-bindgroup");
    bg_desc.layout = layout.get();
    bg_desc.entryCount = entries.size();
    bg_desc.entries = entries.data();

    BindGroup bind_group{wgpuDeviceCreateBindGroup(ctx.device, &bg_desc)};
    if (!bind_group) {
        Log("error", "bind group creation failed");
        return std::nullopt;
    }

    // Working copy of the params image. Padded to the uniform alignment; the
    // pad bytes stay zero, and bytes 8.. are the coordinator's, untouched.
    std::vector<std::byte> params_image(params_bytes, std::byte{0});
    std::ranges::copy(req.params, params_image.begin());

    TaskStats stats;
    const auto task_start = platform::Now();

    // ── the chunk loop ──
    // ── readback ──
    //
    // Normally ONCE, after every chunk has accumulated — that single upload per
    // task IS the arithmetic-intensity design (D-0001/R5). `readback_every_chunk`
    // turns it into once per dispatch, which is E2's CONTROL CONDITION (5.22):
    // the same work with accumulation switched off, so the pair of numbers shows
    // what accumulation actually buys rather than asserting it.
    std::vector<std::byte> result;
    // ── GPU-SIDE TIMING (5.21, D-0083) ───────────────────────────────────
    //
    // `wgpuQueueSubmit` is asynchronous, so wall-clock timing around it measures
    // how long SUBMISSION took — ~5 ms regardless of workload — while the real
    // GPU time ends up absorbed into whatever blocks next. D-0082 showed the
    // consequence: `transfer_ms` scaled linearly with WORK at constant output
    // size, which made 5.21's utilization question unanswerable.
    //
    // A timestamp query set records the GPU's OWN clock at the start and end of
    // each compute pass, which is the only way to separate the two.
    //
    // OPTIONAL (K6). The spec makes it optional because precise timers are a
    // side-channel risk and a browser may withhold it, so everything below
    // degrades to the previous behaviour when absent — a worker without it
    // still computes, it just reports a lower bound.
    std::uint32_t gpu_timed_dispatches = 0;
    const bool have_timestamps =
        platform::HasFeature(ctx, WGPUFeatureName_TimestampQuery);
    QuerySet timestamps;
    Buffer ts_resolve;
    Buffer ts_read;
    if (have_timestamps) {
        WGPUQuerySetDescriptor qs_desc{};
        qs_desc.label = Str("p2pgpu-timestamps");
        qs_desc.type = WGPUQueryType_Timestamp;
        qs_desc.count = 2;   // beginning and end of one pass
        timestamps = QuerySet{wgpuDeviceCreateQuerySet(ctx.device, &qs_desc)};

        WGPUBufferDescriptor rd{};
        rd.label = Str("p2pgpu-ts-resolve");
        rd.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
        rd.size = 2 * sizeof(std::uint64_t);
        ts_resolve = Buffer{wgpuDeviceCreateBuffer(ctx.device, &rd)};

        WGPUBufferDescriptor md{};
        md.label = Str("p2pgpu-ts-read");
        md.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        md.size = 2 * sizeof(std::uint64_t);
        ts_read = Buffer{wgpuDeviceCreateBuffer(ctx.device, &md)};
    }
    // Falls back silently rather than failing: a missing optional feature must
    // never stop a worker contributing (K6).
    const bool timing_ready =
        have_timestamps && timestamps && ts_resolve && ts_read;

    const auto ReadBack = [&]() -> bool {
        const auto readback_start = platform::Now();

        CommandEncoder copy_encoder{wgpuDeviceCreateCommandEncoder(ctx.device, nullptr)};
        if (!copy_encoder) {
            Log("error", "readback encoder creation failed");
            return false;
        }
        wgpuCommandEncoderCopyBufferToBuffer(copy_encoder.get(), out_buf.get(), 0,
                                             stage_buf.get(), 0, req.output_bytes);
        CommandBuffer copy_cmds{wgpuCommandEncoderFinish(copy_encoder.get(), nullptr)};
        if (!copy_cmds) {
            Log("error", "readback command buffer finish failed");
            return false;
        }
        WGPUCommandBuffer copy_raw = copy_cmds.get();
        wgpuQueueSubmit(ctx.queue, 1, &copy_raw);

        MapResult map_result;
        WGPUBufferMapCallbackInfo map_cb{};
        map_cb.mode = WGPUCallbackMode_AllowProcessEvents;
        map_cb.callback = MapThunk;
        map_cb.userdata1 = &map_result;

        (void)wgpuBufferMapAsync(stage_buf.get(), WGPUMapMode_Read, 0, req.output_bytes,
                                 map_cb);

        if (!platform::WaitUntil(ctx, [&] { return map_result.done; })) {
            Log("error", "result map timed out");
            return false;
        }
        if (map_result.status != WGPUMapAsyncStatus_Success) {
            Log("error", "result map failed with status " +
                             std::to_string(static_cast<int>(map_result.status)));
            return false;
        }

        const void* mapped = wgpuBufferGetConstMappedRange(stage_buf.get(), 0,
                                                           req.output_bytes);
        if (mapped == nullptr) {
            Log("error", "mapped range was null");
            wgpuBufferUnmap(stage_buf.get());
            return false;
        }

        result.resize(req.output_bytes);
        const std::span<const std::byte> mapped_view{static_cast<const std::byte*>(mapped),
                                                     req.output_bytes};
        std::ranges::copy(mapped_view, result.begin());
        wgpuBufferUnmap(stage_buf.get());

        stats.transfer_ms += MillisBetween(readback_start, platform::Now());

        return true;
    };

    for (std::uint64_t done = 0; done < req.unit_count;) {
        // Re-check every iteration, not just at entry. A task is many
        // dispatches over seconds and the device can go away between any two of
        // them; submitting to a dead device aborts the process (D-0022).
        if (platform::DeviceIsLost()) {
            Log("warn", "RunTask: device lost mid-task; abandoning");
            return std::nullopt;
        }

        const std::uint64_t units = std::min(chunk, req.unit_count - done);
        const std::uint64_t start = req.start_unit + done;

        // Both fit in u32: `units` is bounded by max_units_per_dispatch above,
        // and `start` is a candidate index within the task's own 32-bit window
        // (the 64-bit half of the keyspace travels in the kernel's own params).
        WriteChunkWindow(params_image, static_cast<std::uint32_t>(start),
                         static_cast<std::uint32_t>(units));

        const auto upload_start = platform::Now();
        wgpuQueueWriteBuffer(ctx.queue, params_buf.get(), 0, params_image.data(),
                             params_image.size());
        stats.transfer_ms += MillisBetween(upload_start, platform::Now());

        CommandEncoder encoder{wgpuDeviceCreateCommandEncoder(ctx.device, nullptr)};
        if (!encoder) {
            Log("error", "command encoder creation failed");
            return std::nullopt;
        }
        {
            // Timestamps bracket the PASS, so what is measured is the GPU
            // executing this dispatch — not the CPU submitting it.
            WGPUPassTimestampWrites ts_writes{};
            WGPUComputePassDescriptor pass_desc{};
            if (timing_ready) {
                ts_writes.querySet = timestamps.get();
                ts_writes.beginningOfPassWriteIndex = 0;
                ts_writes.endOfPassWriteIndex = 1;
                pass_desc.timestampWrites = &ts_writes;
            }
            ComputePass pass{wgpuCommandEncoderBeginComputePass(
                encoder.get(), timing_ready ? &pass_desc : nullptr)};
            if (!pass) {
                Log("error", "compute pass creation failed");
                return std::nullopt;
            }
            wgpuComputePassEncoderSetPipeline(pass.get(), pipeline.get());
            wgpuComputePassEncoderSetBindGroup(pass.get(), 0, bind_group.get(), 0, nullptr);
            // Round up; the kernel bounds-checks the overhang against
            // unit_count rather than the dispatch size (K1).
            //
            // WHICH COUNT drives the grid is kernel-dependent, and assuming it
            // was always `units` was wrong — see TaskRequest::invocations_x. A
            // zero means the one-invocation-per-unit model; anything else is a
            // fixed grid that does not shrink with the chunk.
            const std::uint64_t span_x =
                req.invocations_x != 0 ? req.invocations_x : units;
            const std::uint64_t groups_x =
                (span_x + req.workgroup_size - 1) / req.workgroup_size;
            const std::uint64_t groups_y =
                (req.invocations_y + req.workgroup_size_y - 1) / req.workgroup_size_y;
            wgpuComputePassEncoderDispatchWorkgroups(
                pass.get(), static_cast<std::uint32_t>(groups_x),
                static_cast<std::uint32_t>(groups_y), 1);
            wgpuComputePassEncoderEnd(pass.get());
        }

        // Resolve into a GPU buffer, then stage for mapping. Both must be
        // encoded into THIS command buffer, after the pass has ended — a
        // resolve issued before the pass closes reads values that have not been
        // written.
        if (timing_ready) {
            wgpuCommandEncoderResolveQuerySet(encoder.get(), timestamps.get(), 0, 2,
                                              ts_resolve.get(), 0);
            wgpuCommandEncoderCopyBufferToBuffer(encoder.get(), ts_resolve.get(), 0,
                                                 ts_read.get(), 0,
                                                 2 * sizeof(std::uint64_t));
        }

        CommandBuffer commands{wgpuCommandEncoderFinish(encoder.get(), nullptr)};
        if (!commands) {
            Log("error", "command buffer finish failed");
            return std::nullopt;
        }

        const auto gpu_start = platform::Now();
        WGPUCommandBuffer raw = commands.get();
        wgpuQueueSubmit(ctx.queue, 1, &raw);
        // SUBMIT time, which is not GPU time — asynchronous by definition. Kept
        // as the fallback and superseded below when timestamps are available.
        const double submit_ms = MillisBetween(gpu_start, platform::Now());

        if (timing_ready) {
            // Map the two timestamps. This BLOCKS, which is exactly what the
            // wall clock could not do: the dispatch is now waited for here, in
            // the GPU column, instead of leaking into the readback.
            TimestampPair ts{};
            if (ReadTimestamps(ctx, ts_read, ts)) {
                // Nanoseconds on both implementations we target. `end < begin`
                // is possible if the GPU's counter wrapped or the queue was
                // reset; treated as no sample rather than as a negative
                // duration, which would silently deflate the total.
                if (ts.end > ts.begin) {
                    stats.gpu_ms += static_cast<double>(ts.end - ts.begin) / 1.0e6;
                    ++gpu_timed_dispatches;
                } else {
                    stats.gpu_ms += submit_ms;
                }
            } else {
                stats.gpu_ms += submit_ms;
            }
        } else {
            stats.gpu_ms += submit_ms;
        }

        ++stats.dispatches;
        stats.iterations += units;
        done += units;

        // Hand the host back its thread between dispatches (R4/K1). Native this
        // is a no-op; in the browser it returns to the event loop, which is the
        // whole reason a long task is many short dispatches rather than one
        // long kernel. Counted as idle because it is time this worker is not
        // computing — EVALUATION.md E2 needs that split honest.
        const auto yield_start = platform::Now();
        platform::Yield();
        stats.idle_ms += MillisBetween(yield_start, platform::Now());

        // E2's CONTROL (5.22): read the accumulator back after EVERY dispatch.
        // This is what "accumulation disabled" costs, measured rather than
        // asserted — and the readback is the whole of it, which is why it is
        // timed into transfer_ms exactly as the final one is.
        if (req.readback_every_chunk && !ReadBack()) {
            return std::nullopt;
        }

        // Proof of life for a long task. The whole task runs inside one Poll(),
        // so without this the worker is silent for its entire duration and gets
        // declared lost while working perfectly.
        if (on_chunk) {
            on_chunk(done, req.unit_count);
        }
    }


    if (!ReadBack()) {
        return std::nullopt;
    }

    // gpu_ms above is SUBMIT time, not GPU execution time — wgpuQueueSubmit is
    // asynchronous, so it measures how long submission took, not how long the
    // GPU ran. D-0020 measured that gap at ~106 us natively and ~5 ms in the
    // browser. Real GPU timing needs timestamp queries (an optional feature,
    // K6), which is step 2.11's problem. Until then the honest statement is
    // that gpu_ms is a lower bound, and the coordinator must not treat it as
    // anything else — it is untrusted telemetry regardless (invariant 8).
    const double wall_ms = MillisBetween(task_start, platform::Now());
    // ONLY when gpu_ms is the submit-time fallback. With real timestamps the
    // unattributed remainder is genuinely NOT GPU execution — it is driver and
    // host overhead — and folding it in would recreate exactly the conflation
    // D-0082 diagnosed, in the field that was supposed to fix it.
    if (gpu_timed_dispatches == 0 &&
        wall_ms > stats.gpu_ms + stats.transfer_ms + stats.idle_ms) {
        // The remainder is time inside the driver we did not attribute. Fold it
        // into gpu_ms rather than leaving the fields summing to less than the
        // wall clock, which would look like a measurement bug downstream.
        stats.gpu_ms = wall_ms - stats.transfer_ms - stats.idle_ms;
    }

    TaskOutcome outcome;
    outcome.output = std::move(result);
    outcome.stats = stats;
    return outcome;
}

}  // namespace p2pgpu::worker

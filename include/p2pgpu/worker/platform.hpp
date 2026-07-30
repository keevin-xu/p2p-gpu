#pragma once
//
// THE PLATFORM SEAM (rule R2).
//
// worker-core is compiled twice — native and WebAssembly — from identical
// source. This header declares the only interface that differs between them.
// Two implementations satisfy it:
//
//     src/worker-core/platform/gpu_native.cpp    #if !__EMSCRIPTEN__
//     src/worker-core/platform/gpu_wasm.cpp      #if  __EMSCRIPTEN__
//     src/worker-core/platform/timer_native.cpp
//     src/worker-core/platform/timer_wasm.cpp
//
// CMake picks the right pair per target; nothing else in worker-core knows
// which it got.
//
// ── WHY THIS SEAM IS NARROW (D-0016) ─────────────────────────────────────
// Everything past device acquisition — buffers, pipelines, dispatch, readback
// — is called directly against webgpu.h from PORTABLE code, with no seam. That
// is only sound because wgpu-native and emdawnwebgpu were measured to be the
// same header revision: 199/202 shared functions, and all 86 shared structs
// identical field-for-field. The only structural divergence is surface and
// presentation types, which headless compute never touches.
//
// Two things genuinely differ, and they are exactly what lives here:
//   1. HOW you get a device (native: instance + adapter + device; browser: the
//      host already has one, reached through Emscripten).
//   2. HOW you wait for an async result. Native can block. The browser CANNOT
//      — blocking the main thread freezes the tab — so it yields instead.
//
// ── REVIEW RULE ──────────────────────────────────────────────────────────
// A `#ifdef __EMSCRIPTEN__` outside src/worker-core/platform/ is a defect.
// If a portable file appears to need one, THIS INTERFACE IS WRONG — widen it
// here rather than forking the caller. Forking is exactly what R2 exists to
// prevent, and it is how the two targets silently drift apart.
//
// Note transport is NOT in this seam. libdatachannel (native) and
// datachannel-wasm (browser) expose the same API, so transport.cpp is
// genuinely portable. That is the property the whole single-language
// architecture rests on — see docs/DECISIONS.md D-0008.

#include <webgpu/webgpu.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace p2pgpu::worker::platform {

/// A live GPU device and the handles needed to use it. Acquired through the
/// seam; consumed by portable code.
struct GpuContext {
    WGPUInstance instance = nullptr;
    WGPUAdapter  adapter  = nullptr;
    WGPUDevice   device   = nullptr;
    WGPUQueue    queue    = nullptr;

    [[nodiscard]] bool valid() const noexcept { return device != nullptr && queue != nullptr; }
};

/// Human-readable adapter description, for the E7 table and capability
/// negotiation at join (docs/PROTOCOL.md `AdapterInfo` / `GpuLimits`).
///
/// NOTE: the identity strings are populated DIFFERENTLY per target on the same
/// GPU — native fills `device`, Chrome fills `vendor`/`architecture`, each
/// leaving the other blank (docs/RISKS.md §1). Never key logic on one field
/// being non-empty.
struct AdapterDescription {
    std::string vendor;
    std::string architecture;
    std::string device;
    std::string description;
    std::string backend;

    /// Optional WebGPU features actually granted on the device, e.g.
    /// "timestamp-query", "shader-f16", "subgroups". K6: every one of these
    /// must have a fallback path — a worker lacking a feature still contributes.
    std::vector<std::string> features;

    /// Queried limits. K4 forbids hardcoding these; tiling must respect the
    /// real device's numbers, not the spec defaults.
    std::uint64_t max_storage_buffer_binding_size = 0;
    std::uint64_t max_buffer_size = 0;
    std::uint32_t max_compute_workgroups_per_dim = 0;
    std::uint32_t max_compute_invocations_per_workgroup = 0;
    std::uint32_t max_compute_workgroup_storage_size = 0;
};

/// Acquire a GPU device. Returns false if unavailable — a blocklisted driver
/// or unsupported browser is a *capability*, not a crash (docs/RISKS.md §1).
/// Callers report it and decline gracefully.
[[nodiscard]] bool AcquireDevice(GpuContext& out);

/// Release everything in `ctx` and null it out. Safe on a partially-built
/// context, so it doubles as the cleanup path for a failed acquisition.
void ReleaseDevice(GpuContext& ctx);

/// Read adapter identity, granted features, and queried limits. Requires an
/// acquired context.
[[nodiscard]] AdapterDescription DescribeAdapter(const GpuContext& ctx);

/// Was `feature` granted on this device? Optional features must be *requested*
/// at device creation, so adapter support alone is not enough — this reports
/// what the device actually has (K6).
[[nodiscard]] bool HasFeature(const GpuContext& ctx, WGPUFeatureName feature);

/// Drive the GPU until `done()` reports true, or `timeout_ms` elapses.
///
/// THE load-bearing difference between the two targets, and the reason this is
/// a predicate rather than a `WGPUFuture`:
///
///   - Native pumps `wgpuInstanceProcessEvents` in a poll loop. It CANNOT use
///     `wgpuInstanceWaitAny` — wgpu-native v29 declares that function in the
///     header but aborts with "not implemented" when called (D-0017).
///   - The browser cannot block at all; blocking the main thread freezes the
///     tab, so it returns to the event loop and resumes when the promise
///     settles.
///
/// A predicate is the one shape that expresses both honestly. Callers set a
/// flag in their WebGPU callback and wait on it, and portable code reads the
/// same either way.
///
/// Returns false on timeout.
[[nodiscard]] bool WaitUntil(const GpuContext& ctx,
                             const std::function<bool()>& done,
                             std::uint32_t timeout_ms = 5000);

/// Register a device-loss callback. Windows TDR kills work blocking ~2 s and
/// resets the driver (rule R4); the browser surfaces this as a lost device.
/// The handler must release leases, re-acquire, and re-register.
void OnDeviceLost(std::function<void()> handler);

/// Has the current device been lost or destroyed?
///
/// MUST be checked before submitting GPU work. Discovered in step 0.14:
/// `wgpuQueueSubmit` on a lost device does not return an error — it panics
/// inside wgpu-native and **aborts the process**. A Rust panic cannot be caught
/// from C++, so the only defence is not making the call. Worse, the create
/// functions return non-null but *invalid* objects on a lost device, so
/// ordinary null checks sail straight past (D-0022).
///
/// Cleared by a successful AcquireDevice. One device per process is assumed.
[[nodiscard]] bool DeviceIsLost();

/// Record that the device is gone. Called by the seam's own loss callback;
/// also callable by anything that learns of the loss first — the callback is
/// asynchronous and may not have run yet.
void MarkDeviceLost();

/// Yield control so the host stays responsive between dispatches (R4/K1).
/// Native: a no-op or short sleep. WASM: returns to the Emscripten main loop.
/// NEVER busy-wait — on WASM that freezes the tab.
void Yield();

/// Monotonic clock for local elapsed-time measurement only.
///
/// This is NEVER used to decide lease expiry. Lease expiry is decided solely
/// by the coordinator on its own clock; the worker only measures how much of
/// its granted window it has consumed (docs/PROTOCOL.md §5).
[[nodiscard]] std::chrono::steady_clock::time_point Now();

/// Structured log line, routed to spdlog natively and to the browser console
/// under WASM. Field names are identical on both so correlation IDs work
/// across targets (docs/CONVENTIONS.md §6).
void Log(std::string_view level, std::string_view message);

}  // namespace p2pgpu::worker::platform

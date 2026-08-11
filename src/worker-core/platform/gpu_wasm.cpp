// PLATFORM SEAM (browser). Implements include/p2pgpu/worker/platform.hpp
// against the emdawnwebgpu port (D-0014).
//
// The native half is gpu_native.cpp. Both satisfy the same header, and nothing
// else in worker-core knows which one it linked against (R2) — kernel_host.cpp
// compiles unchanged for both.
//
// ── THE ONE REAL DIFFERENCE ──────────────────────────────────────────────
// Native can block a thread. THIS CANNOT. Blocking the browser's main thread
// freezes the tab, stalls the event loop, and guarantees the WebGPU promises
// we are waiting on never resolve — a self-inflicted deadlock. So WaitUntil
// yields to the event loop via emscripten_sleep (ASYNCIFY) instead of
// sleeping a thread. That asymmetry is the entire reason this seam exists.
//
// Implemented in step 0.6.

#if defined(__EMSCRIPTEN__)

#include <chrono>

#include "p2pgpu/worker/platform.hpp"
#include "p2pgpu/worker/wgpu_util.hpp"

#include <emscripten/emscripten.h>
#include <emscripten/console.h>

#include <string>
#include <utility>
#include <vector>

namespace p2pgpu::worker::platform {
namespace {

std::function<void()>& LostHandler() {
    static std::function<void()> handler;
    return handler;
}

/// Set for ANY loss reason, including our own destroy. Guards the submit path
/// (D-0022). One device per process is assumed.
bool& LostFlag() {
    static bool lost = false;
    return lost;
}

void DeviceLostThunk(WGPUDevice const*, WGPUDeviceLostReason reason,
                     WGPUStringView message, void*, void*) {
    // CRITICAL: separate OUR teardown from a GENUINE loss.
    //
    // wgpuDeviceRelease fires this callback with reason `Destroyed`. Observed
    // in step 0.6, where a clean run logged "device lost (2): Device was
    // destroyed" immediately before reporting PASS. Treating that as loss would
    // make the R4 recovery path run during our own shutdown.
    //
    // In the browser this same callback IS how a real TDR-style driver reset
    // arrives, which is exactly why the two cases must not be conflated.
    // Flag EVERY reason. The intentional/genuine distinction below decides
    // whether to RECOVER; it does not change the fact that this device can no
    // longer accept work, and submitting to it would abort the process.
    LostFlag() = true;

    switch (reason) {
        case WGPUDeviceLostReason_Destroyed:
        case WGPUDeviceLostReason_CallbackCancelled:
            Log("debug", "device released (expected): " + wgpu::FromStr(message));
            return;
        case WGPUDeviceLostReason_Unknown:
        case WGPUDeviceLostReason_FailedCreation:
        default:
            break;
    }

    Log("error", "device lost (" + std::to_string(static_cast<int>(reason)) +
                     "): " + wgpu::FromStr(message));
    if (LostHandler()) {
        LostHandler()();
    }
}

void UncapturedErrorThunk(WGPUDevice const*, WGPUErrorType type,
                          WGPUStringView message, void*, void*) {
    Log("error", "uncaptured wgpu error (" + std::to_string(static_cast<int>(type)) +
                     "): " + wgpu::FromStr(message));
}

struct AdapterResult {
    WGPUAdapter adapter = nullptr;
    bool done = false;
};

struct DeviceResult {
    WGPUDevice device = nullptr;
    bool done = false;
};

void AdapterThunk(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                  WGPUStringView message, void* ud, void*) {
    auto* r = static_cast<AdapterResult*>(ud);
    if (status == WGPURequestAdapterStatus_Success) {
        r->adapter = adapter;
    } else {
        Log("error", "requestAdapter failed: " + wgpu::FromStr(message));
    }
    r->done = true;
}

void DeviceThunk(WGPURequestDeviceStatus status, WGPUDevice device,
                 WGPUStringView message, void* ud, void*) {
    auto* r = static_cast<DeviceResult*>(ud);
    if (status == WGPURequestDeviceStatus_Success) {
        r->device = device;
    } else {
        Log("error", "requestDevice failed: " + wgpu::FromStr(message));
    }
    r->done = true;
}

}  // namespace

bool AcquireDevice(GpuContext& out, std::uint32_t timeout_ms) {
    out = GpuContext{};

    out.instance = wgpuCreateInstance(nullptr);
    if (out.instance == nullptr) {
        Log("error", "wgpuCreateInstance failed — no WebGPU in this browser");
        return false;
    }

    // ── adapter ──
    AdapterResult adapter_result;
    WGPURequestAdapterCallbackInfo adapter_cb{};
    adapter_cb.mode = WGPUCallbackMode_AllowProcessEvents;
    adapter_cb.callback = AdapterThunk;
    adapter_cb.userdata1 = &adapter_result;

    // ── ASK FOR THE DISCRETE GPU ─────────────────────────────────────────
    // Passing nullptr here means "no preference", and on a laptop with both an
    // integrated and a discrete GPU that reliably yields the INTEGRATED one —
    // which is the wrong device for a compute contributor and, worse, silently
    // so: everything works, just several times slower, and the benchmark
    // dutifully reports the small number as this machine's throughput.
    //
    // R7 makes this defensible. Nothing runs without an affirmative click, the
    // throttle is the user's, and stop is instant — so a volunteer who opted in
    // is opting in with the hardware they meant. A background page quietly
    // spinning up a discrete GPU would be a different question.
    WGPURequestAdapterOptions adapter_opts{};
    adapter_opts.powerPreference = WGPUPowerPreference_HighPerformance;

    (void)wgpuInstanceRequestAdapter(out.instance, &adapter_opts, adapter_cb);
    if (!WaitUntil(out, [&] { return adapter_result.done; }, timeout_ms) ||
        adapter_result.adapter == nullptr) {
        // requestAdapter returning null is the documented outcome on a
        // blocklisted driver/GPU pair. A capability, not a crash — report it
        // and decline gracefully (docs/RISKS.md §1).
        Log("warn", "no WebGPU adapter — browser or driver declined");
        ReleaseDevice(out);
        return false;
    }
    out.adapter = adapter_result.adapter;

    // ── device ──
    DeviceResult device_result;
    WGPUDeviceDescriptor desc{};
    desc.label = wgpu::Str("p2pgpu-device");
    desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    desc.deviceLostCallbackInfo.callback = DeviceLostThunk;
    desc.uncapturedErrorCallbackInfo.callback = UncapturedErrorThunk;

    // Ask for the optional features this adapter can actually provide (K6).
    // Requesting one the adapter lacks fails device creation outright, so the
    // list is filtered against the adapter first. Every feature here must have
    // a fallback path — none may become a requirement to participate.
    const std::vector<WGPUFeatureName> features =
        wgpu::SupportedOptionalFeatures(out.adapter);
    desc.requiredFeatureCount = features.size();
    desc.requiredFeatures = features.empty() ? nullptr : features.data();

    WGPURequestDeviceCallbackInfo device_cb{};
    device_cb.mode = WGPUCallbackMode_AllowProcessEvents;
    device_cb.callback = DeviceThunk;
    device_cb.userdata1 = &device_result;

    (void)wgpuAdapterRequestDevice(out.adapter, &desc, device_cb);
    if (!WaitUntil(out, [&] { return device_result.done; }, timeout_ms) ||
        device_result.device == nullptr) {
        Log("error", "device request failed");
        ReleaseDevice(out);
        return false;
    }
    out.device = device_result.device;
    out.queue = wgpuDeviceGetQueue(out.device);
    LostFlag() = false;

    return out.valid();
}

void ReleaseDevice(GpuContext& ctx) {
    if (ctx.queue != nullptr)    { wgpuQueueRelease(ctx.queue);       ctx.queue = nullptr; }
    if (ctx.device != nullptr)   { wgpuDeviceRelease(ctx.device);     ctx.device = nullptr; }
    if (ctx.adapter != nullptr)  { wgpuAdapterRelease(ctx.adapter);   ctx.adapter = nullptr; }
    if (ctx.instance != nullptr) { wgpuInstanceRelease(ctx.instance); ctx.instance = nullptr; }
}

bool WaitUntil(const GpuContext& ctx, const std::function<bool()>& done,
               std::uint32_t timeout_ms) {
    if (ctx.instance == nullptr) {
        return false;
    }

    // NEVER busy-wait here. emscripten_sleep unwinds the stack back to the JS
    // event loop (ASYNCIFY) and resumes when the timer fires — which is what
    // lets the browser actually run the promise callbacks we are waiting on.
    // A spin loop would hang the tab forever waiting on work it is preventing.
    //
    // MEASURED AGAINST A CLOCK, not by accumulating the sleeps we asked for.
    // The previous version did `waited_ms += step_ms` with `step_ms = 1`, but
    // browsers clamp nested setTimeout to ~4 ms, so 5000 iterations billed
    // themselves 5 s and burned ~20 s of wall clock. Every timeout in this
    // layer was therefore ~4x longer than it claimed — and the native
    // WaitUntil already used a real deadline, so the same call meant different
    // things on the two targets (R2). Found chasing a TDR recovery that looked
    // hung and was merely slow (D-0051).
    const auto deadline = Now() + std::chrono::milliseconds(timeout_ms);
    constexpr std::uint32_t kStepMs = 1;

    while (!done()) {
        wgpuInstanceProcessEvents(ctx.instance);
        if (done()) {
            break;
        }
        if (Now() >= deadline) {
            return false;
        }
        emscripten_sleep(kStepMs);
    }
    return true;
}

void OnDeviceLost(std::function<void()> handler) {
    LostHandler() = std::move(handler);
}

bool DeviceIsLost() {
    return LostFlag();
}

void MarkDeviceLost() {
    LostFlag() = true;
}

}  // namespace p2pgpu::worker::platform

#endif  // __EMSCRIPTEN__

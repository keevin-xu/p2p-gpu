// PLATFORM SEAM (native). Implements include/p2pgpu/worker/platform.hpp.
// wgpu-native device acquisition, headless — no surface, no swapchain.
//
// The browser half is gpu_wasm.cpp. Both satisfy the same header, and nothing
// else in worker-core knows which one it linked against (R2).
//
// Implemented in step 0.8.

#if !defined(__EMSCRIPTEN__)

#include "p2pgpu/worker/platform.hpp"
#include "p2pgpu/worker/wgpu_util.hpp"

// wgpu-native's extension header, on top of the standard webgpu.h. Native-only
// by definition, which is why it may be included HERE and nowhere else (R2).
#include <webgpu/wgpu.h>

#include <string>
#include <thread>
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
    // wgpuDeviceRelease fires this callback with reason `Destroyed`. Treating
    // that as device loss would make the R4 recovery path run during our own
    // shutdown — releasing leases we already released and re-acquiring a device
    // we deliberately threw away. Only unexpected losses (TDR, driver reset,
    // browser reclaim) may trigger recovery.
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
        Log("error", "wgpuCreateInstance failed");
        return false;
    }

    // ── adapter ──
    AdapterResult adapter_result;
    WGPURequestAdapterCallbackInfo adapter_cb{};
    // AllowProcessEvents, not WaitAnyOnly — the callback has to be able to fire
    // from inside wgpuInstanceProcessEvents, since WaitAny is unimplemented.
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
        // No adapter is a capability, not a crash — a blocklisted driver or an
        // unsupported machine lands here (docs/RISKS.md §1).
        Log("warn", "no WebGPU adapter available on this machine");
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
    // Reverse acquisition order. Each is null-checked, so this doubles as the
    // cleanup path for a partially-built context.
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

    // NOT wgpuInstanceWaitAny. That function is declared in webgpu.h and
    // exported by libwgpu_native, but its body is `unimplemented!()` — calling
    // it aborts the process with a non-unwinding Rust panic (D-0017). Found the
    // hard way in step 0.8; the header gives no hint.
    //
    // So: pump events and poll. wgpuInstanceProcessEvents fires instance-level
    // callbacks (adapter/device requests); wgpuDevicePoll is what advances
    // queue work, which is how a buffer map completes after a submit.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);

    // Two-phase wait. A fixed sleep between polls QUANTIZES every wait shorter
    // than the sleep: measuring submit-to-completion round trips with a flat
    // 100 us sleep reported ~134 us, most of which was this loop rather than
    // the GPU. Short waits now spin (poll-only), long ones fall back to
    // sleeping so a multi-second dispatch does not burn a core.
    //
    // The threshold is deliberately small — long enough to resolve a fast
    // dispatch, short enough that the spin is bounded and cheap.
    constexpr int kSpinPolls = 2000;
    int polls = 0;

    while (!done()) {
        wgpuInstanceProcessEvents(ctx.instance);
        if (ctx.device != nullptr) {
            (void)wgpuDevicePoll(ctx.device, /*wait=*/0U, nullptr);
        }
        if (done()) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        // Native-only. The browser seam must NEVER sleep or spin like this —
        // it has to return to the event loop instead, or the tab freezes.
        if (++polls > kSpinPolls) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
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

#endif  // !__EMSCRIPTEN__

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

#include "p2pgpu/worker/platform.hpp"

#include <emscripten/emscripten.h>
#include <emscripten/console.h>

#include <string>
#include <utility>

namespace p2pgpu::worker::platform {
namespace {

constexpr WGPUStringView Str(const char* s) noexcept {
    return WGPUStringView{s, WGPU_STRLEN};
}

std::string FromStr(WGPUStringView v) {
    if (v.data == nullptr) {
        return {};
    }
    if (v.length == WGPU_STRLEN) {
        return std::string{v.data};
    }
    return std::string{v.data, v.length};
}

const char* BackendName(WGPUBackendType b) noexcept {
    switch (b) {
        case WGPUBackendType_Undefined: return "undefined";
        case WGPUBackendType_Null:      return "null";
        case WGPUBackendType_WebGPU:    return "webgpu";
        case WGPUBackendType_D3D11:     return "d3d11";
        case WGPUBackendType_D3D12:     return "d3d12";
        case WGPUBackendType_Metal:     return "metal";
        case WGPUBackendType_Vulkan:    return "vulkan";
        case WGPUBackendType_OpenGL:    return "opengl";
        case WGPUBackendType_OpenGLES:  return "opengles";
        default:                        return "unknown";
    }
}

std::function<void()>& LostHandler() {
    static std::function<void()> handler;
    return handler;
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
    switch (reason) {
        case WGPUDeviceLostReason_Destroyed:
        case WGPUDeviceLostReason_CallbackCancelled:
            Log("debug", "device released (expected): " + FromStr(message));
            return;
        case WGPUDeviceLostReason_Unknown:
        case WGPUDeviceLostReason_FailedCreation:
        default:
            break;
    }

    Log("error", "device lost (" + std::to_string(static_cast<int>(reason)) +
                     "): " + FromStr(message));
    if (LostHandler()) {
        LostHandler()();
    }
}

void UncapturedErrorThunk(WGPUDevice const*, WGPUErrorType type,
                          WGPUStringView message, void*, void*) {
    Log("error", "uncaptured wgpu error (" + std::to_string(static_cast<int>(type)) +
                     "): " + FromStr(message));
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
        Log("error", "requestAdapter failed: " + FromStr(message));
    }
    r->done = true;
}

void DeviceThunk(WGPURequestDeviceStatus status, WGPUDevice device,
                 WGPUStringView message, void* ud, void*) {
    auto* r = static_cast<DeviceResult*>(ud);
    if (status == WGPURequestDeviceStatus_Success) {
        r->device = device;
    } else {
        Log("error", "requestDevice failed: " + FromStr(message));
    }
    r->done = true;
}

}  // namespace

bool AcquireDevice(GpuContext& out) {
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

    (void)wgpuInstanceRequestAdapter(out.instance, nullptr, adapter_cb);
    if (!WaitUntil(out, [&] { return adapter_result.done; }) ||
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
    desc.label = Str("p2pgpu-device");
    desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    desc.deviceLostCallbackInfo.callback = DeviceLostThunk;
    desc.uncapturedErrorCallbackInfo.callback = UncapturedErrorThunk;

    WGPURequestDeviceCallbackInfo device_cb{};
    device_cb.mode = WGPUCallbackMode_AllowProcessEvents;
    device_cb.callback = DeviceThunk;
    device_cb.userdata1 = &device_result;

    (void)wgpuAdapterRequestDevice(out.adapter, &desc, device_cb);
    if (!WaitUntil(out, [&] { return device_result.done; }) ||
        device_result.device == nullptr) {
        Log("error", "device request failed");
        ReleaseDevice(out);
        return false;
    }
    out.device = device_result.device;
    out.queue = wgpuDeviceGetQueue(out.device);

    return out.valid();
}

void ReleaseDevice(GpuContext& ctx) {
    if (ctx.queue != nullptr)    { wgpuQueueRelease(ctx.queue);       ctx.queue = nullptr; }
    if (ctx.device != nullptr)   { wgpuDeviceRelease(ctx.device);     ctx.device = nullptr; }
    if (ctx.adapter != nullptr)  { wgpuAdapterRelease(ctx.adapter);   ctx.adapter = nullptr; }
    if (ctx.instance != nullptr) { wgpuInstanceRelease(ctx.instance); ctx.instance = nullptr; }
}

AdapterDescription DescribeAdapter(const GpuContext& ctx) {
    AdapterDescription out;
    if (ctx.adapter == nullptr) {
        return out;
    }
    WGPUAdapterInfo info{};
    if (wgpuAdapterGetInfo(ctx.adapter, &info) != WGPUStatus_Success) {
        return out;
    }
    out.vendor       = FromStr(info.vendor);
    out.architecture = FromStr(info.architecture);
    out.device       = FromStr(info.device);
    out.description  = FromStr(info.description);
    out.backend      = BackendName(info.backendType);
    wgpuAdapterInfoFreeMembers(info);
    return out;
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
    std::uint32_t waited_ms = 0;
    const std::uint32_t step_ms = 1;

    while (!done()) {
        wgpuInstanceProcessEvents(ctx.instance);
        if (done()) {
            break;
        }
        if (waited_ms >= timeout_ms) {
            return false;
        }
        emscripten_sleep(step_ms);
        waited_ms += step_ms;
    }
    return true;
}

void OnDeviceLost(std::function<void()> handler) {
    LostHandler() = std::move(handler);
}

void Yield() {
    // Return to the event loop between dispatches so the tab stays responsive
    // and the browser's own watchdogs never fire (R4/K1).
    emscripten_sleep(0);
}

std::chrono::steady_clock::time_point Now() {
    return std::chrono::steady_clock::now();
}

void Log(std::string_view level, std::string_view message) {
    // Same field names as the native sink so correlation IDs line up across
    // targets once they exist (docs/CONVENTIONS.md §6).
    const std::string line = "[" + std::string{level} + "] " + std::string{message};
    if (level == "error") {
        emscripten_console_error(line.c_str());
    } else if (level == "warn") {
        emscripten_console_warn(line.c_str());
    } else {
        emscripten_console_log(line.c_str());
    }
}

}  // namespace p2pgpu::worker::platform

#endif  // __EMSCRIPTEN__

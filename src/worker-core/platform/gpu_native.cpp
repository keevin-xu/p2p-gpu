// PLATFORM SEAM (native). Implements include/p2pgpu/worker/platform.hpp.
// wgpu-native device acquisition, headless — no surface, no swapchain.
//
// The browser half is gpu_wasm.cpp. Both satisfy the same header, and nothing
// else in worker-core knows which one it linked against (R2).
//
// Implemented in step 0.8.

#if !defined(__EMSCRIPTEN__)

#include "p2pgpu/worker/platform.hpp"

// wgpu-native's extension header, on top of the standard webgpu.h. Native-only
// by definition, which is why it may be included HERE and nowhere else (R2).
#include <webgpu/wgpu.h>

#include <cstdio>
#include <string>
#include <thread>
#include <utility>

namespace p2pgpu::worker::platform {
namespace {

/// webgpu.h takes (pointer, length) rather than NUL-terminated strings.
/// WGPU_STRLEN means "call strlen for me".
constexpr WGPUStringView Str(const char* s) noexcept {
    return WGPUStringView{s, WGPU_STRLEN};
}

/// WGPUStringView -> std::string. The view is not guaranteed NUL-terminated,
/// but `length` may also be WGPU_STRLEN meaning "it is after all", so both
/// cases need handling rather than assuming either.
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
    // wgpuDeviceRelease fires this callback with reason `Destroyed`. Treating
    // that as device loss would make the R4 recovery path run during our own
    // shutdown — releasing leases we already released and re-acquiring a device
    // we deliberately threw away. Only unexpected losses (TDR, driver reset,
    // browser reclaim) may trigger recovery.
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

    (void)wgpuInstanceRequestAdapter(out.instance, nullptr, adapter_cb);
    if (!WaitUntil(out, [&] { return adapter_result.done; }) ||
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
    // Reverse acquisition order. Each is null-checked, so this doubles as the
    // cleanup path for a partially-built context.
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
        // Yield the core rather than spinning. This is a native-only wait; the
        // browser seam must never sleep like this (it would freeze the tab).
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return true;
}

void OnDeviceLost(std::function<void()> handler) {
    LostHandler() = std::move(handler);
}

void Yield() {
    // Native has no event loop to return to, and dispatch pacing is the
    // caller's business. Deliberately NOT a sleep — that would inflate the
    // idle_ms/gpu_ms split that EVALUATION.md E2 depends on.
}

std::chrono::steady_clock::time_point Now() {
    return std::chrono::steady_clock::now();
}

void Log(std::string_view level, std::string_view message) {
    // spdlog with the CONVENTIONS.md §6 correlation fields arrives in step
    // 1.11; until the coordinator exists there are no IDs to correlate.
    std::fprintf(stderr, "[%.*s] %.*s\n", static_cast<int>(level.size()), level.data(),
                 static_cast<int>(message.size()), message.data());
}

}  // namespace p2pgpu::worker::platform

#endif  // !__EMSCRIPTEN__

#pragma once
//
// Small shared helpers over webgpu.h. INTERNAL to worker-core.
//
// These are portable by construction: D-0016 measured wgpu-native and
// emdawnwebgpu to expose identical declarations for everything here. Keeping
// them in one header stops the two platform seams from growing near-duplicate
// copies that can silently drift — which is the failure mode R2 exists to
// prevent, expressed at the level of helper functions rather than whole files.

#include <webgpu/webgpu.h>

#include <string>
#include <vector>

namespace p2pgpu::worker::wgpu {

/// webgpu.h takes (pointer, length) rather than NUL-terminated strings.
/// WGPU_STRLEN means "call strlen for me".
constexpr WGPUStringView Str(const char* s) noexcept {
    return WGPUStringView{s, WGPU_STRLEN};
}

/// WGPUStringView -> std::string. The view is not guaranteed NUL-terminated,
/// but `length` may also be WGPU_STRLEN meaning "it is after all", so both
/// cases need handling rather than assuming either.
inline std::string FromStr(WGPUStringView v) {
    if (v.data == nullptr) {
        return {};
    }
    if (v.length == WGPU_STRLEN) {
        return std::string{v.data};
    }
    return std::string{v.data, v.length};
}

inline const char* BackendName(WGPUBackendType b) noexcept {
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

/// The optional features worth asking for, and their spec names.
///
/// K6: every one of these MUST have a fallback path. A worker without
/// `subgroups` or `shader-f16` still contributes — it is simply slower. Nothing
/// here may become a requirement for participation.
struct OptionalFeature {
    WGPUFeatureName id;
    const char* name;
};

inline const std::vector<OptionalFeature>& OptionalFeatures() {
    static const std::vector<OptionalFeature> kFeatures{
        // Needed for GPU-side dispatch timing (step 0.11). Optional in the spec
        // because precise timers are a side-channel risk, so browsers may
        // withhold it — hence the wall-clock fallback.
        {WGPUFeatureName_TimestampQuery, "timestamp-query"},
        // ~2x throughput on many devices where supported.
        {WGPUFeatureName_ShaderF16, "shader-f16"},
        // SIMD ops within a workgroup; Google measured 2.3-2.9x on
        // matrix-vector shaders.
        {WGPUFeatureName_Subgroups, "subgroups"},
    };
    return kFeatures;
}

/// Which of the optional features this adapter can provide. Requesting a
/// feature the adapter lacks fails device creation outright, so the list must
/// be filtered against the adapter before it is requested.
inline std::vector<WGPUFeatureName> SupportedOptionalFeatures(WGPUAdapter adapter) {
    std::vector<WGPUFeatureName> out;
    if (adapter == nullptr) {
        return out;
    }
    for (const OptionalFeature& f : OptionalFeatures()) {
        if (wgpuAdapterHasFeature(adapter, f.id) != 0) {
            out.push_back(f.id);
        }
    }
    return out;
}

}  // namespace p2pgpu::worker::wgpu

// Adapter identity, granted features, and queried limits.
//
// PORTABLE — deliberately NOT in the platform seam, even though it implements
// functions declared in platform.hpp. D-0016 measured wgpu-native and
// emdawnwebgpu to declare all of this identically, so both seams were carrying
// near-identical copies. Duplicated code that is supposed to stay in lockstep
// is exactly what R2 forbids; one portable implementation cannot drift.
//
// What remains genuinely seam-specific is only device ACQUISITION and WAITING.

#include "p2pgpu/worker/platform.hpp"
#include "p2pgpu/worker/wgpu_util.hpp"

namespace p2pgpu::worker::platform {

AdapterDescription DescribeAdapter(const GpuContext& ctx) {
    AdapterDescription out;
    if (ctx.adapter == nullptr) {
        return out;
    }

    WGPUAdapterInfo info{};
    if (wgpuAdapterGetInfo(ctx.adapter, &info) == WGPUStatus_Success) {
        out.vendor       = wgpu::FromStr(info.vendor);
        out.architecture = wgpu::FromStr(info.architecture);
        out.device       = wgpu::FromStr(info.device);
        out.description  = wgpu::FromStr(info.description);
        out.backend      = wgpu::BackendName(info.backendType);
        wgpuAdapterInfoFreeMembers(info);
    }

    // Report what the DEVICE actually got, not what the adapter could offer.
    // Optional features must be requested at creation, so the two differ
    // whenever we chose not to ask for something (K6).
    if (ctx.device != nullptr) {
        for (const wgpu::OptionalFeature& f : wgpu::OptionalFeatures()) {
            if (wgpuDeviceHasFeature(ctx.device, f.id) != 0) {
                out.features.emplace_back(f.name);
            }
        }

        WGPULimits limits{};
        if (wgpuDeviceGetLimits(ctx.device, &limits) == WGPUStatus_Success) {
            out.max_storage_buffer_binding_size = limits.maxStorageBufferBindingSize;
            out.max_buffer_size = limits.maxBufferSize;
            out.max_compute_workgroups_per_dim = limits.maxComputeWorkgroupsPerDimension;
            out.max_compute_invocations_per_workgroup =
                limits.maxComputeInvocationsPerWorkgroup;
            out.max_compute_workgroup_storage_size = limits.maxComputeWorkgroupStorageSize;
        }
    }

    return out;
}

bool HasFeature(const GpuContext& ctx, WGPUFeatureName feature) {
    return ctx.device != nullptr && wgpuDeviceHasFeature(ctx.device, feature) != 0;
}

}  // namespace p2pgpu::worker::platform

#pragma once
//
// Kernel execution host. PORTABLE — compiled for both native and WASM from
// this one source (R2). No platform conditionals here; anything that differs
// goes through include/p2pgpu/worker/platform.hpp.
//
// Note what is deliberately NOT here: loading the WGSL. Native reads it from
// disk, the browser fetches it over HTTP from the coordinator (step 1.12), so
// the source text is passed IN. Keeping acquisition out of worker-core is what
// stops a `#ifdef __EMSCRIPTEN__` appearing in portable code.

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "p2pgpu/worker/platform.hpp"

namespace p2pgpu::worker {

/// Run a single-dispatch f32 -> f32 compute kernel and read the result back.
///
/// Step 0.8's smoke test: the narrowest thing that proves a WGSL compute
/// pipeline works end to end on a target — module, pipeline, bind group,
/// dispatch, copy, map, readback. `worker-native` and `worker-browser` call
/// this with the SAME WGSL file, which is what makes step 0.9's cross-target
/// output comparison meaningful.
///
/// Expects the kernel to bind `input` at @binding(0) and `output` at
/// @binding(1) in @group(0), with the given 1-D workgroup size.
///
/// Returns std::nullopt on any failure; failures are logged through the seam.
[[nodiscard]] std::optional<std::vector<float>> RunUnaryF32Kernel(
    const platform::GpuContext& ctx,
    std::string_view wgsl_source,
    std::string_view entry_point,
    std::span<const float> input,
    std::uint32_t workgroup_size);

}  // namespace p2pgpu::worker

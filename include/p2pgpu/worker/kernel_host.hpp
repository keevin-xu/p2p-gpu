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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "p2pgpu/worker/platform.hpp"

namespace p2pgpu::worker {

/// Run a single-dispatch element-wise compute kernel and read the result back.
///
/// Deliberately **untyped** — it moves bytes. The kernel's WGSL declares what
/// those bytes mean, and `OutputSpec.dtype` carries it on the wire
/// (`PROTOCOL.md`). Baking f32 in here would mean a near-duplicate function per
/// dtype, and `DType` already lists five.
///
/// Step 0.8/0.9's smoke path: the narrowest thing that proves a WGSL compute
/// pipeline works end to end on a target — module, pipeline, bind group,
/// dispatch, copy, map, readback. `worker-native` and `worker-browser` call
/// this with the SAME WGSL bytes, which is what makes step 0.9's cross-target
/// comparison mean anything.
///
/// Expects the kernel to bind `input` at @binding(0) and `output` at
/// @binding(1) in @group(0), with the given 1-D workgroup size. Output is the
/// same byte length as input. `element_count` drives dispatch sizing and is the
/// kernel's own element count, not a byte count.
///
/// Returns std::nullopt on any failure; failures are logged through the seam.
/// Compile WGSL and build a compute pipeline, WITHOUT running anything.
///
/// Exists so every kernel in `kernels/` can be validated on every commit —
/// including in CI against the software adapter (4.4) — rather than only when
/// something happens to execute it. A kernel that no test exercises yet is
/// exactly the one whose WGSL rots.
///
/// Building the PIPELINE and not merely the module is the point: a shader
/// module can compile while its entry point is missing, its workgroup size
/// exceeds the device, or its bindings do not form a valid layout. Those are
/// the failures that would otherwise surface on a stranger's GPU.
[[nodiscard]] bool CompileKernel(const platform::GpuContext& ctx,
                                 std::string_view wgsl_source,
                                 std::string_view entry_point);

[[nodiscard]] std::optional<std::vector<std::byte>> RunUnaryKernel(
    const platform::GpuContext& ctx,
    std::string_view wgsl_source,
    std::string_view entry_point,
    std::span<const std::byte> input,
    std::uint32_t element_count,
    std::uint32_t workgroup_size);

// ─────────────────────────────────────────────────────────────────────────
// Task execution — step 1.19
// ─────────────────────────────────────────────────────────────────────────

/// One task, as the coordinator described it. Everything here comes off the
/// wire in `TaskEnvelope`; nothing is chosen locally (R1).
struct TaskRequest {
    std::string_view wgsl;
    std::string_view entry_point;

    /// Uniform buffer bytes, built by the COORDINATOR (it owns sizing).
    ///
    /// Bytes 0..7 are the chunk window and are OVERWRITTEN per dispatch with
    /// `(start_unit, unit_count)` — K1 / D-0033. Whatever the coordinator put
    /// there is ignored; everything past byte 8 is copied verbatim and never
    /// interpreted.
    std::span<const std::byte> params;

    std::uint64_t start_unit = 0;
    std::uint64_t unit_count = 0;
    std::uint32_t output_bytes = 0;

    /// Workgroup dimensions, matching the kernel's `@workgroup_size`.
    std::uint32_t workgroup_size = 64;    ///< x
    std::uint32_t workgroup_size_y = 1;   ///< y

    /// ── INVOCATION GRID, WHEN UNITS ARE NOT INVOCATIONS ─────────────────
    ///
    /// Zero (the default) means the `brute_search_v1` model: **one invocation
    /// per unit**, so the grid shrinks with the chunk. That mapping was implicit
    /// in the host from 1.19 until 5.12, because it was true of the only kernel
    /// that existed.
    ///
    /// It is not true in general. For `pathtrace_tile_v1` a unit is one SAMPLE
    /// of the whole tile while the invocation grid is PIXELS — fixed, and
    /// completely independent of how the sample range was chunked. Deriving the
    /// grid from the chunk there dispatched a fraction of the tile and left the
    /// rest of the accumulator untouched; the kernel bounds-checks its own
    /// overhang, so nothing failed, it simply rendered part of the image.
    ///
    /// Set these to state the grid explicitly. The host still applies K4's
    /// `maxComputeWorkgroupsPerDimension` clamp to whatever it is given.
    std::uint32_t invocations_x = 0;
    std::uint32_t invocations_y = 1;

    /// Initial bytes of the result buffer, written ONCE PER TASK.
    ///
    /// Not once per chunk. `brute_search_v1` needs `min_match = 0xFFFFFFFF` as
    /// the identity for `atomicMin`, and re-initialising between chunks is the
    /// "state leaks between chunks" bug — it would present as scheduling
    /// nondeterminism, which is the most expensive thing to misdiagnose in a
    /// system whose whole verification story is determinism (R6).
    ///
    /// Empty means zero-filled.
    std::span<const std::byte> output_init;

    /// Read-only storage inputs, bound at bindings 2, 3, 4… IN ORDER (D-0072).
    ///
    /// The host attaches NO meaning to these — it uploads each span to a buffer
    /// and binds it. Turning one fetched asset into this list is the caller's
    /// job, because that is the only place that knows the asset's shape, and
    /// keeping the knowledge out of here is what preserves D-0033's
    /// kernel-agnostic host.
    ///
    /// Binding 0 is params and binding 1 is the output; inputs therefore start
    /// at 2, and a kernel's WGSL must agree. Nothing checks that agreement at
    /// runtime — a mismatch is a pipeline creation failure, which is why
    /// `CompileKernel` builds the pipeline rather than just the module.
    std::span<const std::span<const std::byte>> inputs;
};

/// Facts about an execution. Reported to the coordinator; NEVER used to decide
/// anything locally, and the coordinator treats them as untrusted telemetry
/// (invariant 8) because a worker can lie about every field.
struct TaskStats {
    double gpu_ms = 0.0;
    double transfer_ms = 0.0;
    double idle_ms = 0.0;
    std::uint32_t dispatches = 0;
    std::uint64_t iterations = 0;
};

struct TaskOutcome {
    std::vector<std::byte> output;
    TaskStats stats;
};

/// Compile once, then dispatch the range in chunks of <=250 ms expected work
/// with a yield between each (R4/K1), accumulating into one result buffer.
///
/// `units_per_chunk` bounds a single dispatch. It is a LOCAL EXECUTION DETAIL,
/// not scheduling: R4 is a hardware-safety property (Windows TDR resets the
/// driver at ~2 s), so the worker must satisfy it whatever it was told. What
/// work to do, and how much of it, remains entirely the coordinator's (R1) —
/// and because every chunkable kernel's reductions are partition-independent,
/// the subdivision cannot change the answer.
///
/// Zero means "one dispatch", which is what the golden tests want.
///
/// Returns nullopt on any failure, including device loss mid-task. Failures are
/// logged through the seam; the caller reports them and moves on (R8).
/// Called after each chunk, with (units_done, units_total).
///
/// Exists so a long task can prove it is alive. `Execute` runs a whole task
/// inside one `Poll()`, so without this the worker sends nothing for the task's
/// entire duration and the coordinator declares it lost — chunking already
/// exists to yield between dispatches (R4), and this is the natural place to
/// say so out loud.
using ChunkCallback = std::function<void(std::uint64_t, std::uint64_t)>;

[[nodiscard]] std::optional<TaskOutcome> RunTask(const platform::GpuContext& ctx,
                                                 const TaskRequest& req,
                                                 std::uint64_t units_per_chunk = 0,
                                                 const ChunkCallback& on_chunk = {});

}  // namespace p2pgpu::worker

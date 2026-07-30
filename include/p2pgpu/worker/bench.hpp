#pragma once
//
// Throughput and dispatch-overhead measurement (steps 0.10-0.12).
//
// PORTABLE — lives in worker-core so the browser measures with identical code
// (step 0.11 wants the same numbers from both targets, and E7 compares them).
// Step 2.11 re-uses RunCalibration as the join-time benchmark whose score
// drives the adaptive sizer, so this is not throwaway measurement code.
//
// MEASURE UNDER native-release, NEVER under sanitizers. ASan's ~2x slowdown
// corrupts every timing number here (CONVENTIONS.md §8).

#include <cstdint>
#include <string>
#include <vector>

#include "p2pgpu/worker/platform.hpp"

namespace p2pgpu::worker {

/// C++ mirror of the WGSL `Params` struct in kernels/calibrate.wgsl.
/// Field order and size must match exactly — see the static_assert in bench.cpp
/// (CONVENTIONS.md §5). Silent layout drift produces garbage that looks like a
/// kernel bug and costs hours.
struct CalibrateParams {
    std::uint32_t iterations;
    std::uint32_t seed;
};

struct BenchSample {
    std::uint32_t invocations = 0;
    std::uint32_t iterations = 0;
    std::uint32_t dispatches = 0;
    double wall_ms = 0.0;      ///< host-observed, submit through completion
    double gpu_ms = -1.0;      ///< from timestamp-query; <0 when unavailable
    std::uint64_t flops = 0;   ///< exact, from the D-0018 derivation
    double gflops = 0.0;
};

/// Sweep problem sizes and measure achieved GFLOP/s (step 0.11).
[[nodiscard]] std::vector<BenchSample> RunCalibration(
    const platform::GpuContext& ctx,
    std::string_view wgsl,
    const std::vector<std::uint32_t>& invocation_counts,
    std::uint32_t iterations);

/// Per-dispatch overhead in microseconds (step 0.12).
///
/// MARGINAL cost of one more dispatch inside a single command buffer: many
/// trivial dispatches, one submit. This is what bounds R4 chunking, where a
/// task is split into many short dispatches that are encoded together.
[[nodiscard]] double MeasureDispatchOverheadUs(const platform::GpuContext& ctx,
                                               std::string_view wgsl,
                                               std::uint32_t repetitions);

/// Per-SUBMISSION overhead in microseconds (step 0.12).
///
/// Full round trip: encode, submit, wait for completion — one dispatch each
/// time. Strictly larger than the marginal figure above, and the two must not
/// be confused. This one sets the FLOOR on useful task size, because every
/// task pays it at least once; it is the number the Phase 2 sizer needs when
/// deciding that 1-3 s tasks amortise overhead but 10 ms tasks do not.
///
/// Published WebGPU figures of 24-71 us are this measurement, not the other.
[[nodiscard]] double MeasureSubmitOverheadUs(const platform::GpuContext& ctx,
                                             std::string_view wgsl,
                                             std::uint32_t repetitions);

struct ChunkSample {
    std::uint32_t chunks = 0;
    std::uint32_t iterations_per_chunk = 0;
    double wall_ms = 0.0;         ///< total for all chunks, including yields
    double max_chunk_ms = 0.0;    ///< slowest single chunk — the R4 number
    double overhead_pct = 0.0;    ///< vs. the 1-chunk baseline
};

/// Chunking spike (step 0.15) — the empirical test of rule R4.
///
/// R4 says no single dispatch may represent more than ~250 ms of work, because
/// Windows kills GPU work that blocks ~2 s and resets the driver. So every long
/// task MUST be many short dispatches. That is only viable if splitting is
/// cheap, and this measures how cheap.
///
/// Holds total work constant and varies how many pieces it is cut into. Each
/// chunk is submitted SEPARATELY with a Yield() between, which is the real R4
/// pattern — batching them into one command buffer would leave no opportunity
/// to yield, and on the browser a non-yielding worker freezes the tab.
///
/// So each chunk pays the full submit round trip, not the cheaper marginal
/// dispatch cost. That is the honest cost of being interruptible.
[[nodiscard]] std::vector<ChunkSample> RunChunkingSpike(
    const platform::GpuContext& ctx,
    std::string_view wgsl,
    std::uint32_t invocations,
    std::uint32_t total_iterations,
    const std::vector<std::uint32_t>& chunk_counts);

}  // namespace p2pgpu::worker

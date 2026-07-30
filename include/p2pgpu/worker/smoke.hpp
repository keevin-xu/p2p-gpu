#pragma once
//
// The step 0.6 / 0.8 / 0.9 smoke suite. PORTABLE — lives in worker-core so
// that `worker-native` and `worker-browser` run byte-identical verification
// logic (R1: the thin wrappers decide nothing).
//
// That placement is the entire point of step 0.9. If each `main()` carried its
// own checking code, "native and browser agree" would be a statement about two
// hand-written verifiers agreeing, not about the GPU path. Here, the only
// difference between the two runs is which platform/ translation unit linked.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "p2pgpu/worker/platform.hpp"

namespace p2pgpu::worker {

/// WGSL handed in by the caller — native reads a file, the browser reads an
/// embedded copy, step 1.12 will fetch it from the coordinator. worker-core
/// never learns which (R2).
struct KernelSource {
    std::string_view name;
    std::string_view wgsl;
};

struct SmokeReport {
    bool passed = false;
    /// Canonical, deterministic text: one digest line per kernel followed by a
    /// full hex dump. Byte-comparable across targets — this IS the 0.9
    /// artifact, so nothing machine- or run-dependent may appear in it.
    std::string report;
};

/// Run every kernel, verify each against a CPU reference, and emit the report.
///
/// Verifying against a CPU reference rather than only cross-checking targets
/// matters: two targets can agree and both still be wrong. Agreement proves
/// host-code symmetry; the reference proves correctness. 0.9 wants both.
[[nodiscard]] SmokeReport RunSmokeSuite(const platform::GpuContext& ctx,
                                        const std::vector<KernelSource>& kernels);

}  // namespace p2pgpu::worker

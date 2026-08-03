#pragma once
//
// Result checking against the CPU reference — step 1.26's harness.
//
// ┌─ THIS IS A TEST HARNESS, NOT A VALIDATION STRATEGY ────────────────────┐
// │ If the coordinator could simply compute every answer itself, there     │
// │ would be no reason to distribute the work at all — the entire project  │
// │ premise is that it cannot. Recomputing results here is affordable ONLY │
// │ because 1.26 deliberately uses tiny tasks; at the sizer's real         │
// │ operating point (~1.25e10 candidates) it would take longer than the    │
// │ whole grid.                                                            │
// │                                                                        │
// │ REAL validation is replication across workers plus reputation, and it  │
// │ is Phase 3. Nothing here is a step toward it, and this must never be   │
// │ enabled by default or quoted as evidence that results are validated.   │
// └────────────────────────────────────────────────────────────────────────┘
//
// What it IS good for: proving end to end, once, on real hardware, that the
// numbers arriving over the wire are the numbers the work was supposed to
// produce — through the sizer, the params builder, the wire encoding, the GPU,
// the chunk loop, the checksum and the ingestion path. That is exactly step
// 1.26's requirement, and nothing cheaper demonstrates it.

#include <cstddef>
#include <cstdint>
#include <span>

#include "p2pgpu/coordinator/job.hpp"
#include "p2pgpu/coordinator/kernel_registry.hpp"

namespace p2pgpu::coordinator {

/// Running tally, printed at shutdown. A count of zero checks is reported
/// distinctly from a count of zero mismatches — "nothing was verified" and
/// "everything verified clean" must never look alike in a log.
struct ReferenceStats {
    std::uint64_t checked = 0;
    std::uint64_t matched = 0;
    std::uint64_t mismatched = 0;
    std::uint64_t unsupported = 0;  ///< kernel has no reference implementation
};

/// Recompute `task` on the CPU and compare against `payload` byte for byte.
///
/// Returns false only on a genuine MISMATCH. A kernel with no reference
/// implementation counts as `unsupported` and returns true — refusing results
/// for `calibrate_v1` because this harness cannot check them would be a harness
/// deciding policy, which it has no business doing.
[[nodiscard]] bool CheckAgainstReference(const KernelSpec& spec, const Job& job,
                                         const Task& task,
                                         std::span<const std::byte> payload,
                                         ReferenceStats& stats);

}  // namespace p2pgpu::coordinator

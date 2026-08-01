#pragma once
//
// Building a task's uniform-buffer params — step 1.20's coordinator half.
//
// ── WHY THIS IS THE COORDINATOR'S JOB ────────────────────────────────────
// R1: the worker executes and reports facts, it makes no decisions. Which
// keyspace a task searches, what difficulty mask it uses, what seed it runs
// under — all of that is *what work to do*, and the worker must never invent
// any of it. So the coordinator builds the bytes and the worker uploads them.
//
// The one exception is the chunk window at bytes 0..7, which the worker's
// kernel host overwrites per dispatch (K1 / D-0033). That is not a decision
// about what work to do — the union of the chunks is exactly the range the
// coordinator granted — it is a local execution detail forced by R4.
//
// ── WHY IT IS KEYED ON param_layout ──────────────────────────────────────
// Each kernel's params struct is its own; there is no generic shape. The
// manifest declares `param_layout` for exactly this dispatch, and an unknown
// layout is an error rather than a zero-filled default: a task whose params are
// silently all zero would run happily and return a well-formed wrong answer.

#include <cstddef>
#include <vector>

#include "p2pgpu/coordinator/job.hpp"
#include "p2pgpu/coordinator/kernel_registry.hpp"
#include "p2pgpu/protocol/error.hpp"

namespace p2pgpu::coordinator {

/// Build the uniform-buffer bytes for one task.
///
/// Fails if the kernel's `param_layout` is not one this coordinator knows how
/// to build. That is a manifest/coordinator mismatch and must be loud.
[[nodiscard]] protocol::Result<std::vector<std::byte>> BuildParams(
    const KernelSpec& spec, const Job& job, const Task& task);

}  // namespace p2pgpu::coordinator

// Step 1.26's harness. See reference_check.hpp for why this is NOT validation.

#include "p2pgpu/coordinator/reference_check.hpp"

#include <spdlog/spdlog.h>

#include <cstring>
#include <vector>

#include "p2pgpu/coordinator/params.hpp"
#include "p2pgpu/kernels/reference.hpp"

namespace p2pgpu::coordinator {

bool CheckAgainstReference(const KernelSpec& spec, const Job& job, const Task& task,
                           std::span<const std::byte> payload, ReferenceStats& stats) {
    if (spec.param_layout != "BruteSearchParams") {
        // No reference for this kernel. NOT a failure — see the header: a
        // harness that rejected results it cannot check would be making policy.
        ++stats.unsupported;
        return true;
    }
    if (payload.size() != sizeof(kernels::BruteSearchResult)) {
        spdlog::error("reference_check task={} payload is {} bytes, expected {}",
                      task.id.lo(), payload.size(), sizeof(kernels::BruteSearchResult));
        ++stats.mismatched;
        return false;
    }

    // Rebuild the EXACT params the worker was sent. Going through BuildParams
    // rather than reconstructing them here is the point: if the params builder
    // is wrong, both the GPU and this check are wrong in the same way and agree
    // — so this is a check on the pipeline, not on BuildParams.
    const auto params = BuildParams(spec, job, task);
    if (!params) {
        spdlog::error("reference_check task={} could not rebuild params: {}",
                      task.id.lo(), params.error().message);
        ++stats.mismatched;
        return false;
    }

    kernels::BruteSearchParams p{};
    if (params->size() != sizeof(p)) {
        spdlog::error("reference_check task={} params size changed under us", task.id.lo());
        ++stats.mismatched;
        return false;
    }
    // memcpy out of a buffer WE just built, into a type whose layout params.hpp
    // asserts field by field. Not network input; R11's ban does not reach here.
    std::memcpy(&p, params->data(), sizeof(p));

    // The worker's kernel host overwrites the chunk window per dispatch, and
    // the union of its chunks is exactly this range (D-0033). The reference
    // runs the whole range in one pass — which is what makes this a check on
    // the chunk loop and not merely a repeat of it.
    const auto expected = kernels::BruteSearchReference(p);

    kernels::BruteSearchResult actual{};
    std::memcpy(&actual, payload.data(), sizeof(actual));

    ++stats.checked;
    const bool ok = actual.found_count == expected.found_count &&
                    actual.min_match == expected.min_match &&
                    actual.match_xor == expected.match_xor;
    if (ok) {
        ++stats.matched;
        return true;
    }

    ++stats.mismatched;
    // Every field, both sides. A mismatch here is the most serious thing this
    // system can report — it means the distributed answer is WRONG, not late or
    // missing — so the log line must contain enough to start bisecting without
    // reproducing the run.
    spdlog::error("REFERENCE MISMATCH task={} start_lo={} units={} base_hi={} seed={} "
                  "mask={:08x} target={:08x} rounds={} | got count={} min={} xor={:08x} "
                  "| want count={} min={} xor={:08x}",
                  task.id.lo(), p.start_lo, p.unit_count, p.base_hi, p.seed, p.mask,
                  p.target_bits, p.rounds, actual.found_count, actual.min_match,
                  actual.match_xor, expected.found_count, expected.min_match,
                  expected.match_xor);
    return false;
}

}  // namespace p2pgpu::coordinator

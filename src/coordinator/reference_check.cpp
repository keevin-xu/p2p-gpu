// Step 1.26's harness. See reference_check.hpp for why this is NOT validation.

#include "p2pgpu/coordinator/reference_check.hpp"
#include "p2pgpu/coordinator/validator.hpp"
#include "p2pgpu/kernels/pathtrace_reference.hpp"
#include <algorithm>
#include <bit>
#include <array>

#include <spdlog/spdlog.h>

#include <cstring>
#include <vector>

#include "p2pgpu/coordinator/params.hpp"
#include "p2pgpu/protocol/verify.hpp"
#include "p2pgpu/kernels/reference.hpp"

namespace p2pgpu::coordinator {

bool CheckAgainstReference(const KernelSpec& spec, const Job& job, const Task& task,
                           std::span<const std::byte> payload, ReferenceStats& stats) {
    // ── PATH TRACER: a STATISTICAL check on a SUB-RECT (5.20) ────────────
    //
    // Not the whole tile. The reference is single-threaded and brute-forces
    // every primitive by design (5.19), so a 64x64 tile at a useful sample count
    // is minutes of CPU. A 16x16 corner at the same sample count is seconds and
    // asks the same question — the kernel does not know which pixels are being
    // watched.
    //
    // The verdict comes from 5.14's comparator rather than a pixel threshold,
    // because the distributed image and the reference are two independent Monte
    // Carlo estimates and a fixed threshold cannot separate their honest
    // disagreement from a real one (D-0075/D-0080).
    if (spec.param_layout == "PathTraceParams") {
        if (!job.render || !job.render->reference_bvh) {
            ++stats.unsupported;
            return true;
        }
        const RenderConfig& rc = *job.render;
        const std::uint64_t tile_index = task.start_unit / rc.samples_per_tile;
        const auto tile = rc.grid.TileAt(static_cast<std::uint32_t>(tile_index));
        if (!tile) {
            ++stats.unsupported;
            return true;
        }

        constexpr std::uint32_t kProbe = 16;
        const std::uint32_t pw = std::min(kProbe, tile->w);
        const std::uint32_t ph = std::min(kProbe, tile->h);
        if (payload.size() != static_cast<std::size_t>(tile->pixels()) * 16) {
            ++stats.unsupported;
            return true;
        }

        kernels::ReferenceRequest rr;
        std::copy_n(rc.cam_origin, 3, rr.camera.origin);
        std::copy_n(rc.cam_lower_left, 3, rr.camera.lower_left);
        std::copy_n(rc.cam_horizontal, 3, rr.camera.horizontal);
        std::copy_n(rc.cam_vertical, 3, rr.camera.vertical);
        rr.image_w = rc.grid.image_w;
        rr.image_h = rc.grid.image_h;
        rr.tile_x = tile->x;
        rr.tile_y = tile->y;
        rr.tile_w = pw;
        rr.tile_h = ph;
        rr.samples = task.unit_count;
        // A DIFFERENT seed from the job's. Two independent estimates of one
        // integral: agreement cannot then come from drawing the same numbers.
        rr.seed = static_cast<std::uint32_t>(job.seed ^ 0x5BD1E995u) + 1u;
        rr.max_bounces = rc.max_bounces;
        rr.rr_start_bounce = rc.rr_start_bounce;

        const auto expected = kernels::PathTraceReference(*rc.reference_bvh, rr);

        // Extract the same sub-rect from the worker's tile — row-major within
        // the tile, so a sub-rect is a set of row segments, not a prefix.
        std::vector<float> actual(static_cast<std::size_t>(pw) * ph * 4);
        for (std::uint32_t y = 0; y < ph; ++y) {
            for (std::uint32_t x = 0; x < pw; ++x) {
                const std::size_t src = (static_cast<std::size_t>(y) * tile->w + x) * 16;
                const std::size_t dst = (static_cast<std::size_t>(y) * pw + x) * 4;
                for (std::size_t k = 0; k < 4; ++k) {
                    std::array<std::byte, 4> raw{};
                    for (std::size_t j = 0; j < 4; ++j) {
                        raw[j] = payload[src + k * 4 + j];
                    }
                    actual[dst + k] = std::bit_cast<float>(raw);
                }
            }
        }

        KernelSpec stat_spec = spec;
        stat_spec.determinism = Determinism::Statistical;
        const auto cmp = Compare(
            stat_spec,
            std::as_bytes(std::span<const float>(actual)),
            std::as_bytes(std::span<const float>(expected)));

        ++stats.checked;
        if (cmp.verdict == Verdict::Match) {
            ++stats.matched;
            return true;
        }
        if (cmp.verdict == Verdict::Unsupported) {
            ++stats.unsupported;
            return true;
        }
        ++stats.mismatched;
        spdlog::warn("reference_mismatch task={} tile={} detail=\"{}\"",
                     task.id.lo(), tile_index, Describe(cmp));
        return false;
    }

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

    // R11: `payload` is the WORKER'S submitted result — network input. A
    // memcpy here reads sizeof(actual) bytes whether or not they arrived, which
    // is the Heartbleed shape; `ReadStruct` checks the length first. The audit
    // in 4.16 found this line unjustified while the memcpy above it (a buffer
    // we built ourselves) was correctly annotated.
    const auto actual_opt =
        protocol::ReadStruct<kernels::BruteSearchResult>(payload);
    if (!actual_opt) {
        spdlog::error("reference_check task={} payload shorter than the result type",
                      task.id.lo());
        ++stats.mismatched;
        return false;
    }
    const kernels::BruteSearchResult actual = *actual_opt;

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

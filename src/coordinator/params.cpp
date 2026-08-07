// Task params, built by the coordinator. See params.hpp for why it is the
// coordinator that builds them (R1).

#include "p2pgpu/coordinator/params.hpp"

#include <cstring>

#include "p2pgpu/kernels/params.hpp"
#include "p2pgpu/kernels/pathtrace_params.hpp"

namespace p2pgpu::coordinator {
namespace {

using protocol::ErrorCode;
using protocol::MakeError;

/// Byte image of a params struct.
///
/// memcpy of a LOCAL, fully-owned struct into a buffer we allocated. R11 bans
/// memcpy in code that touches *network bytes*; this is the opposite direction
/// — we are producing bytes, from a type whose layout `params.hpp` asserts
/// field by field. Hand-rolling a field serialiser here would be more code with
/// one more place to disagree with those assertions.
template <typename T>
std::vector<std::byte> ToBytes(const T& v) {
    std::vector<std::byte> out(sizeof(T));
    // OUTBOUND serialization: copying a struct WE built into a buffer we own,
    // to hand to the worker. Not network input — R11's ban is about reading
    // attacker-controlled bytes, and nothing here has been off the wire
    // (4.16 audit).
    std::memcpy(out.data(), &v, sizeof(T));
    return out;
}

/// Derive a per-task 32-bit value from the job seed and the task's position.
///
/// Splitmix64's finalizer. Not for cryptography — it exists so two tasks in one
/// job do not search identical keyspace just because they share a seed, and so
/// the mapping is reproducible from `(job seed, start_unit)` alone, which K2
/// requires for replication to be sound.
std::uint32_t Mix32(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return static_cast<std::uint32_t>((x ^ (x >> 31)) >> 32);
}

}  // namespace

protocol::Result<std::vector<std::byte>> BuildParams(const KernelSpec& spec,
                                                     const Job& job,
                                                     const Task& task) {
    if (spec.param_layout == "BruteSearchParams") {
        kernels::BruteSearchParams p{};

        // The chunk window. The worker's kernel host overwrites bytes 0..7 per
        // dispatch (D-0033), so these values are what a SINGLE-dispatch run
        // would use — set honestly rather than left zero, because a zeroed
        // window is indistinguishable from a host that forgot to write one.
        //
        // start_lo is the task's offset within the job's 32-bit low window; the
        // high half travels separately in base_hi, since WGSL has no u64.
        p.start_lo = static_cast<std::uint32_t>(task.start_unit & 0xFFFFFFFFULL);

        // REFUSE, never narrow (D-0063). This cast was unguarded until 4.17,
        // where a 5e9-unit task arrived as 705,032,704 and every consumer of
        // this blob silently described the wrong range. The sizer now caps at
        // kMaxUnitsPerTask, so reaching this is a bug elsewhere — which is
        // exactly when a silent truncation is most expensive.
        if (task.unit_count > 0xFFFFFFFFULL) {
            return protocol::MakeError(
                protocol::ErrorCode::Internal,
                "task of " + std::to_string(task.unit_count) +
                    " units exceeds what BruteSearchParams can express (2^32-1)");
        }
        p.unit_count = static_cast<std::uint32_t>(task.unit_count);
        p.base_hi = static_cast<std::uint32_t>(task.start_unit >> 32);

        // Per-task, derived from the job seed. Deterministic given
        // (job.seed, task.start_unit), which is what lets a replica of this
        // task be issued to a different worker and compared bitwise (K2/R6).
        p.seed = Mix32(job.seed ^ task.start_unit);

        // Difficulty. Fixed for now — tuning it per job is the submission API's
        // business (2.16), and a hardcoded value here is at least visible.
        // 0xFFF gives roughly one match per 4096 candidates.
        p.mask = 0x00000FFF;
        p.target_bits = Mix32(job.seed) & p.mask;
        p.rounds = 8;  // must match the manifest's `rounds`

        return ToBytes(p);
    }

    if (spec.param_layout == "PathTraceParams") {
        if (!job.render) {
            // LOUD. A render kernel on a job with no render config would
            // otherwise produce a params blob of zeros — a 0x0 tile, a camera
            // at the origin looking nowhere — and the worker would dutifully
            // render nothing and report success.
            return MakeError(ErrorCode::Internal,
                             "pathtrace task on a job with no render config");
        }
        const RenderConfig& r = *job.render;
        if (r.samples_per_tile == 0) {
            return MakeError(ErrorCode::Internal, "render job has 0 samples per tile");
        }

        // The linearization: unit = tile_index * samples_per_tile + sample.
        // `Job::units_per_group` guarantees a task never crosses a tile
        // boundary, so this division is exact for the whole range.
        const std::uint64_t tile_index = task.start_unit / r.samples_per_tile;
        const std::uint64_t sample_offset = task.start_unit % r.samples_per_tile;
        const auto tile = r.grid.TileAt(static_cast<std::uint32_t>(tile_index));
        if (!tile) {
            return MakeError(ErrorCode::Internal,
                             "task maps to a tile outside the render grid");
        }
        if (sample_offset + task.unit_count > r.samples_per_tile) {
            // Would mean the carve let a task cross a tile. Checked rather than
            // assumed: the failure renders half a range into the wrong tile and
            // still produces a well-formed accumulator of the right size, which
            // nothing downstream could detect (the D-0040 shape).
            return MakeError(ErrorCode::Internal,
                             "task spans more than one tile");
        }

        kernels::PathTraceParams p{};
        p.start_unit = static_cast<std::uint32_t>(sample_offset);
        p.unit_count = static_cast<std::uint32_t>(task.unit_count);
        p.tile_x = tile->x;
        p.tile_y = tile->y;
        p.tile_w = tile->w;
        p.tile_h = tile->h;
        p.image_w = r.grid.image_w;
        p.image_h = r.grid.image_h;
        for (int i = 0; i < 3; ++i) {
            p.cam_origin[i] = r.cam_origin[i];
            p.cam_lower_left[i] = r.cam_lower_left[i];
            p.cam_horizontal[i] = r.cam_horizontal[i];
            p.cam_vertical[i] = r.cam_vertical[i];
        }
        p.max_bounces = r.max_bounces;
        p.rr_start_bounce = r.rr_start_bounce;
        p.node_count = r.node_count;
        p.prim_count = r.prim_count;
        p.material_count = r.material_count;

        // THE SEED IS PER-JOB, NOT PER-TASK — the opposite of brute_search.
        //
        // The kernel already keys its RNG on (seed, image pixel, absolute
        // sample index), so two tasks covering different sample ranges of the
        // same tile draw different numbers from one seed. Mixing the seed per
        // task would instead make the samples depend on HOW THE RANGE WAS
        // CARVED, and a replica sized differently would legitimately disagree —
        // destroying K2 and with it the comparator's premise.
        p.seed = Mix32(job.seed);
        return ToBytes(p);
    }

    if (spec.param_layout == "CalibrateParams") {
        kernels::CalibrateParams p{};
        p.iterations = 2048;
        p.seed = Mix32(job.seed ^ task.start_unit);
        return ToBytes(p);
    }

    // LOUD, not zero-filled. A task whose params were silently all zero would
    // execute happily and return a well-formed wrong answer — and every replica
    // would agree with it, so validation would confirm the bug rather than
    // catch it.
    return MakeError(ErrorCode::Internal,
                     "no params builder for layout: " + spec.param_layout);
}

std::vector<std::byte> BuildOutputInit(const KernelSpec& spec) {
    if (spec.param_layout == "BruteSearchParams") {
        // min_match must start at atomicMin's identity. Zero would make "no
        // match" indistinguishable from "matched candidate 0" — and worse,
        // atomicMin(0, x) is 0 forever, which pinned the field at zero for
        // every task until D-0040.
        return ToBytes(kernels::BruteSearchResult{});
    }
    // Everything else reduces from zero, which is what an empty vector means.
    return {};
}

}  // namespace p2pgpu::coordinator

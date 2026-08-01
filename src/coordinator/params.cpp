// Task params, built by the coordinator. See params.hpp for why it is the
// coordinator that builds them (R1).

#include "p2pgpu/coordinator/params.hpp"

#include <cstring>

#include "p2pgpu/kernels/params.hpp"

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

}  // namespace p2pgpu::coordinator

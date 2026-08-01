// T3 — kernel tests on a REAL GPU. Step 1.19's verification.
//
// Separate binary from `tests` on purpose: these need a working adapter, and a
// unit suite that silently skips itself on a machine without one is worse than
// no suite at all. Run with `ctest -R kernel_` or directly.
//
// What is being proved here is narrow but load-bearing: RunTask's chunk loop
// must not change the answer. D-0033 lets the host rewrite the chunk window
// with no knowledge of the kernel, and R4 forces it to split a task into many
// dispatches — so if subdivision perturbed the result, every long task would
// return something a replica computed differently, and the validator would call
// two honest workers liars (R6, the D-0003 disaster).

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "p2pgpu/kernels/params.hpp"
#include "p2pgpu/worker/kernel_host.hpp"
#include "p2pgpu/worker/platform.hpp"

using namespace p2pgpu;
using namespace p2pgpu::worker;

namespace {

std::string ReadKernel(const char* name) {
    const std::filesystem::path path = std::filesystem::path(P2PGPU_KERNEL_DIR) / name;
    std::ifstream in(path);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Byte image of a params struct. Goes through memcpy on a LOCAL, fully-owned
/// struct — this is not network input, so R11's ban does not reach here, and
/// the alternative (a hand-rolled field serialiser) would be more code with
/// more ways to disagree with the static_asserts in params.hpp.
template <typename T>
std::vector<std::byte> AsBytes(const T& v) {
    std::vector<std::byte> out(sizeof(T));
    std::memcpy(out.data(), &v, sizeof(T));
    return out;
}

kernels::BruteSearchResult ParseResult(std::span<const std::byte> bytes) {
    REQUIRE(bytes.size() == sizeof(kernels::BruteSearchResult));
    kernels::BruteSearchResult r{};
    std::memcpy(&r, bytes.data(), sizeof(r));
    return r;
}

/// Acquire once for the whole binary. Device acquisition is slow and the point
/// here is the kernel, not the seam.
struct Gpu {
    platform::GpuContext ctx{};
    Gpu() { REQUIRE(platform::AcquireDevice(ctx)); }
    ~Gpu() { platform::ReleaseDevice(ctx); }
};

Gpu& TheGpu() {
    static Gpu gpu;
    return gpu;
}

/// Run brute_search_v1 over [0, units) with the given chunk size.
kernels::BruteSearchResult RunSearch(std::uint64_t units, std::uint64_t units_per_chunk) {
    static const std::string wgsl = ReadKernel("brute_search.wgsl");

    kernels::BruteSearchParams p{};
    // Deliberately garbage in the chunk window: RunTask must overwrite bytes
    // 0..7 (D-0033), and seeding them with values that would produce a WRONG
    // answer is what makes this test able to fail.
    p.start_lo = 0xDEADBEEF;
    p.unit_count = 0xDEADBEEF;
    p.base_hi = 7;
    p.seed = 12345;
    p.mask = 0x00000FFF;    // ~1 match per 4096 candidates
    p.target_bits = 0x00000ABC;
    p.rounds = 8;

    const auto params = AsBytes(p);
    const auto init = AsBytes(kernels::BruteSearchResult{});

    TaskRequest req;
    req.wgsl = wgsl;
    req.entry_point = "main";
    req.params = params;
    req.start_unit = 0;
    req.unit_count = units;
    req.output_bytes = sizeof(kernels::BruteSearchResult);
    req.workgroup_size = 256;
    req.output_init = init;

    const auto outcome = RunTask(TheGpu().ctx, req, units_per_chunk);
    REQUIRE(outcome.has_value());
    return ParseResult(outcome->output);
}

}  // namespace

TEST_CASE("brute_search finds matches and the reductions agree", "[kernel][brute_search]") {
    constexpr std::uint64_t kUnits = 1u << 20;  // ~256 expected matches at mask 0xFFF
    const auto r = RunSearch(kUnits, 0);

    // A run that finds nothing would make every other assertion here vacuous.
    CHECK(r.found_count > 0);
    CHECK(r.min_match < kUnits);
    CHECK_FALSE(r.empty());

    // reserved is written by NOBODY — not by the kernel (K8 forbids non-atomic
    // writes) and not by the host after init.
    for (std::uint32_t word : r.reserved) {
        CHECK(word == 0);
    }
}

TEST_CASE("RunTask overwrites the chunk window regardless of what it was given",
          "[kernel][brute_search]") {
    // The params handed in carry 0xDEADBEEF in bytes 0..7. If RunTask trusted
    // them, unit_count would be nonsense and start_lo would be off in the
    // keyspace — either a zero result or a wildly different one.
    const auto r = RunSearch(1u << 20, 0);
    CHECK(r.found_count > 0);
}

TEST_CASE("chunking does not change the answer", "[kernel][brute_search]") {
    constexpr std::uint64_t kUnits = 1u << 20;

    const auto whole = RunSearch(kUnits, 0);              // one dispatch
    const auto quarters = RunSearch(kUnits, kUnits / 4);  // four
    const auto small = RunSearch(kUnits, 4096);           // 256

    // NOT VACUOUS. Three runs that all found nothing would agree perfectly and
    // prove nothing — the exact way an invariance test rots into decoration.
    REQUIRE(whole.found_count > 0);

    // BITWISE identical, and legitimately so: this kernel is `Exact` and all
    // three outputs are order-independent reductions (add/min/xor), so they do
    // not care how the range was partitioned or in what order the GPU got to
    // it. If this ever fails, suspect the host's accumulation — the result
    // buffer must be initialised ONCE PER TASK, never per chunk.
    CHECK(quarters.found_count == whole.found_count);
    CHECK(quarters.min_match == whole.min_match);
    CHECK(quarters.match_xor == whole.match_xor);

    CHECK(small.found_count == whole.found_count);
    CHECK(small.min_match == whole.min_match);
    CHECK(small.match_xor == whole.match_xor);
}

TEST_CASE("a chunked run reports more dispatches but the same work", "[kernel][stats]") {
    static const std::string wgsl = ReadKernel("brute_search.wgsl");
    constexpr std::uint64_t kUnits = 1u << 20;

    kernels::BruteSearchParams p{};
    p.base_hi = 1;
    p.seed = 999;
    p.mask = 0x00000FFF;
    p.target_bits = 0x00000111;
    p.rounds = 8;
    const auto params = AsBytes(p);
    const auto init = AsBytes(kernels::BruteSearchResult{});

    TaskRequest req;
    req.wgsl = wgsl;
    req.entry_point = "main";
    req.params = params;
    req.unit_count = kUnits;
    req.output_bytes = sizeof(kernels::BruteSearchResult);
    req.workgroup_size = 256;
    req.output_init = init;

    const auto one = RunTask(TheGpu().ctx, req, 0);
    const auto many = RunTask(TheGpu().ctx, req, kUnits / 8);
    REQUIRE(one.has_value());
    REQUIRE(many.has_value());

    CHECK(one->stats.dispatches == 1);
    CHECK(many->stats.dispatches == 8);
    // `iterations` counts units actually dispatched, so the split must not
    // change it — a mismatch means chunks overlapped or dropped a range, which
    // is precisely the bug that would break the identity above while leaving
    // found_count plausible.
    CHECK(one->stats.iterations == kUnits);
    CHECK(many->stats.iterations == kUnits);
    CHECK(one->output == many->output);
}

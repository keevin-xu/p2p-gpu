// The step 0.6 / 0.8 / 0.9 smoke suite. Portable; see smoke.hpp for why it
// lives in worker-core rather than in each main().
//
// NO #ifdef IN THIS FILE, EVER (R2).

#include "p2pgpu/worker/smoke.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <span>
#include <vector>

#include "p2pgpu/worker/kernel_host.hpp"

namespace p2pgpu::worker {
namespace {

constexpr std::uint32_t kElementCount = 1000;   // NOT a multiple of 256, so the
constexpr std::uint32_t kWorkgroupSize = 256;   // dispatch overhangs (K1)

/// CPU twin of pcg_hash in kernels/smoke_hash.wgsl. Must stay bit-identical to
/// the WGSL; both are u32 wrapping arithmetic, which is well-defined in WGSL
/// and, for unsigned types, in C++ too.
std::uint32_t PcgHash(std::uint32_t v) noexcept {
    const std::uint32_t state = v * 747796405U + 2891336453U;
    const std::uint32_t word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return (word >> 22U) ^ word;
}

/// FNV-1a 64. Not for security — a short, stable fingerprint so a human can
/// compare two runs at a glance instead of diffing 8000 hex characters.
std::uint64_t Fingerprint(std::span<const std::byte> bytes) noexcept {
    std::uint64_t h = 1469598103934665603ULL;
    for (const std::byte b : bytes) {
        h ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(b));
        h *= 1099511628211ULL;
    }
    return h;
}

std::string ToHex(std::span<const std::byte> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2 + bytes.size() / 32 + 2);
    std::size_t col = 0;
    for (const std::byte b : bytes) {
        const auto v = std::to_integer<std::uint8_t>(b);
        out.push_back(kDigits[v >> 4U]);
        out.push_back(kDigits[v & 0x0FU]);
        if (++col % 32 == 0) {
            out.push_back('\n');
        }
    }
    if (col % 32 != 0) {
        out.push_back('\n');
    }
    return out;
}

template <typename T>
std::span<const std::byte> AsBytes(const std::vector<T>& v) noexcept {
    return std::as_bytes(std::span<const T>{v});
}

/// Read the i-th 4-byte element out of a byte span.
///
/// std::bit_cast rather than memcpy — this is not network-facing code so R11
/// does not strictly bind, but keeping memcpy out of the codebase entirely
/// means a reviewer grepping for it (step 4.16's audit) gets a clean result
/// instead of hits that each need justifying.
template <typename T>
T LoadElement(std::span<const std::byte> bytes, std::uint32_t i) noexcept {
    static_assert(sizeof(T) == 4);
    const std::size_t off = static_cast<std::size_t>(i) * 4U;
    const std::array<std::byte, 4> raw{bytes[off], bytes[off + 1],
                                       bytes[off + 2], bytes[off + 3]};
    return std::bit_cast<T>(raw);
}

std::string Line(std::string_view kernel, std::uint64_t fp, bool ok) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "kernel=%.*s elements=%u fnv1a=%016llx match=%s\n",
                  static_cast<int>(kernel.size()), kernel.data(), kElementCount,
                  static_cast<unsigned long long>(fp), ok ? "yes" : "NO");
    return std::string{buf};
}

}  // namespace

SmokeReport RunSmokeSuite(const platform::GpuContext& ctx,
                          const std::vector<KernelSource>& kernels) {
    SmokeReport out;
    out.passed = true;

    for (const KernelSource& k : kernels) {
        // Build the input for this kernel's dtype. Both are 4-byte element
        // types, so the byte length matches either way.
        std::vector<float> f_in(kElementCount);
        std::vector<std::uint32_t> u_in(kElementCount);
        std::iota(f_in.begin(), f_in.end(), 1.0F);
        std::iota(u_in.begin(), u_in.end(), 1U);

        const bool is_hash = k.name.find("hash") != std::string_view::npos;
        const std::span<const std::byte> input = is_hash ? AsBytes(u_in) : AsBytes(f_in);

        const auto result =
            RunUnaryKernel(ctx, k.wgsl, "main", input, kElementCount, kWorkgroupSize);
        if (!result) {
            out.report += Line(k.name, 0, false);
            out.report += "  ERROR: kernel execution failed\n";
            out.passed = false;
            continue;
        }
        if (result->size() != input.size_bytes()) {
            out.report += Line(k.name, 0, false);
            out.report += "  ERROR: output size mismatch\n";
            out.passed = false;
            continue;
        }

        // Verify against the CPU reference, element by element. Both kernels
        // are exactly reproducible on a CPU — `*2.0` only shifts a float
        // exponent, and the hash is integer — so this is an EXACT check with no
        // tolerance. That is only legitimate because both are
        // DeterminismClass::Exact; a float kernel doing real arithmetic would
        // need R6 tolerance handling instead.
        const std::span<const std::byte> out_bytes{*result};
        std::size_t mismatches = 0;
        for (std::uint32_t i = 0; i < kElementCount; ++i) {
            const bool ok_elem =
                is_hash ? LoadElement<std::uint32_t>(out_bytes, i) == PcgHash(u_in[i])
                        : LoadElement<float>(out_bytes, i) == f_in[i] * 2.0F;
            if (!ok_elem) {
                ++mismatches;
            }
        }

        const std::uint64_t fp = Fingerprint(*result);
        const bool ok = (mismatches == 0);
        out.passed = out.passed && ok;

        out.report += Line(k.name, fp, ok);
        if (!ok) {
            out.report += "  mismatches=" + std::to_string(mismatches) + "\n";
        }
        out.report += ToHex(*result);
    }

    // Verdict goes IN the report, not printed alongside it, so both targets
    // emit byte-identical artifacts. These files are the step 0.9 evidence;
    // a formatting asymmetry between them is noise in a comparison whose whole
    // purpose is detecting differences.
    out.report += out.passed ? "verdict=PASS\n" : "verdict=FAIL\n";
    return out;
}

}  // namespace p2pgpu::worker

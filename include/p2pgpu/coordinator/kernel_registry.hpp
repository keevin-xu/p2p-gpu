#pragma once
//
// Kernel registry — step 1.12.
//
// Loads kernels/manifest.toml at startup, serves WGSL by id over HTTP, and
// supplies the descriptors that go into `Welcome`.
//
// ── ON THE MANIFEST PARSER ───────────────────────────────────────────────
// This reads a deliberately restricted TOML subset with a small hand-written
// parser rather than taking a TOML library as a dependency (D-0030).
//
// **The manifest is a trusted, in-repo, developer-authored file read once at
// startup. It is NOT network input, so R11 does not apply to it.** That is the
// entire justification, and it stops being true the moment anyone points this
// parser at something a peer supplied — at which point it must be replaced, not
// hardened.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "p2pgpu/protocol/error.hpp"

namespace p2pgpu::coordinator {

/// How the validator may compare two replicas of this kernel's output (R6).
/// Misdeclaring this blacklists honest workers, so it is parsed strictly:
/// an unrecognised value is an error, never a default.
enum class Determinism { Exact, Tolerant, Statistical };

struct KernelSpec {
    std::string id;
    std::string file;            ///< relative to the kernels directory
    std::string entry_point;
    std::string wgsl;            ///< source, loaded at startup and held in memory

    std::uint32_t workgroup_size[3] = {1, 1, 1};
    Determinism determinism = Determinism::Statistical;  ///< weakest by default (K3)
    float rel_eps = 0.0F;
    float abs_eps = 0.0F;

    std::uint32_t output_bytes = 0;
    std::string output_dtype;
    std::string param_layout;

    bool accumulates = false;
    std::uint64_t min_iterations = 0;
    std::uint64_t flop_per_unit = 0;

    /// R5 floor. The sizer must never grant fewer units than this, even when
    /// clamping for a very slow worker — below it the kernel is transfer-bound
    /// and adding nodes cannot help (D-0029(b)).
    std::uint64_t r5_min_units = 0;
    double r5_ratio = 0.0;
};

class KernelRegistry {
public:
    /// Parse the manifest and load every referenced WGSL file.
    ///
    /// Fails loudly rather than skipping a bad entry: a kernel that silently
    /// failed to load would surface much later as an UnknownKernel error from a
    /// worker, and the cause would be nowhere near the symptom.
    [[nodiscard]] static protocol::Result<KernelRegistry> Load(
        const std::filesystem::path& manifest, const std::filesystem::path& kernel_dir);

    [[nodiscard]] const KernelSpec* Find(std::string_view id) const noexcept;
    [[nodiscard]] std::vector<const KernelSpec*> All() const;
    [[nodiscard]] std::size_t size() const noexcept { return kernels_.size(); }

private:
    std::map<std::string, KernelSpec, std::less<>> kernels_;
};

}  // namespace p2pgpu::coordinator

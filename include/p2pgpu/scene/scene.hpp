#pragma once
//
// Scene description — step 5.2's format, as C++ reads it.
//
// ── TRUSTED INPUT, AND ONLY THIS FILE IS ─────────────────────────────────
// A `.scene` text is in-repo input read at coordinator startup, exactly like
// `kernels/manifest.toml` (D-0030). It never crosses the network, so this
// parser may be lenient and may report errors as strings.
//
// The BVH built FROM it is a different story entirely: those bytes arrive from
// an arbitrary peer in Phase 6, and `bvh.hpp`'s loader treats them as hostile
// (D-0069). Do not reach for this parser when handling asset bytes, and do not
// let its leniency drift into that one.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "p2pgpu/protocol/error.hpp"

namespace p2pgpu::scene {

struct Vec3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

/// Kept out of the BVH asset ON PURPOSE. The camera is render configuration,
/// not geometry, so one content-addressed scene asset serves every viewpoint —
/// which is what lets a demo orbit the camera without invalidating the cache
/// every worker just filled (2.16 affinity, Phase 6 P2P).
struct Camera {
    Vec3 origin{0.0F, 1.0F, 4.0F};
    Vec3 target{};
    Vec3 up{0.0F, 1.0F, 0.0F};
    float vfov_deg = 40.0F;
};

enum class MaterialKind : std::uint32_t {
    Lambertian = 0,
    Metal = 1,
    Emissive = 2,
};

struct Material {
    MaterialKind kind = MaterialKind::Lambertian;
    Vec3 albedo{0.5F, 0.5F, 0.5F};
    /// Metal only. 0 is a perfect mirror.
    float fuzz = 0.0F;
};

struct Sphere {
    Vec3 center;
    float radius = 1.0F;
    std::uint32_t material = 0;
};

struct Triangle {
    Vec3 a;
    Vec3 b;
    Vec3 c;
    std::uint32_t material = 0;
};

struct Scene {
    Camera camera;
    std::vector<Material> materials;
    std::vector<Sphere> spheres;
    std::vector<Triangle> triangles;

    [[nodiscard]] std::size_t primitive_count() const noexcept {
        return spheres.size() + triangles.size();
    }
};

/// Parse `.scene` text. `version` must be the first non-comment directive so a
/// future v2 file is REJECTED rather than misread — a v1 parser that silently
/// ignores unknown directives would render a scene missing half its geometry
/// and report success.
///
/// `error_line`, if given, receives the 1-based line of a failure. It is an
/// out-param rather than part of the message because `protocol::Error::message`
/// is a NON-OWNING `string_view` documented "static text only" — composing
/// `"line " + std::to_string(n)` into it hands it a view of a temporary that
/// dies immediately. `kernels.cpp` sets the same precedent for trusted-file
/// parsers: literal messages only.
[[nodiscard]] protocol::Result<Scene> ParseScene(std::string_view text,
                                                 std::size_t* error_line = nullptr);

/// Convenience for the coordinator's startup path.
[[nodiscard]] protocol::Result<Scene> LoadSceneFile(const std::string& path,
                                                    std::size_t* error_line = nullptr);

}  // namespace p2pgpu::scene

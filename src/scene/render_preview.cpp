// Dev tool: render a scene single-machine and write a PPM — step 5.15's visual
// proof, and the skeleton of 5.19's reference render.
//
// NOT a worker and NOT a coordinator. It wires the real pieces together —
// scene -> BVH -> asset round-trip -> GPU kernel -> compositor — so that an
// image can be LOOKED AT. Every automated check in this project compares
// numbers; a rendering bug that satisfies all of them still shows up instantly
// to an eye.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <span>
#include <string>
#include <vector>

#include "p2pgpu/coordinator/composite.hpp"
#include "p2pgpu/kernels/pathtrace_params.hpp"
#include "p2pgpu/scene/bvh.hpp"
#include "p2pgpu/scene/scene.hpp"
#include "p2pgpu/worker/kernel_host.hpp"
#include "p2pgpu/worker/platform.hpp"

namespace platform = p2pgpu::worker::platform;
using namespace p2pgpu::scene;
using namespace p2pgpu::kernels;
using namespace p2pgpu::coordinator;

namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
    const std::string scene_path = argc > 1 ? argv[1] : "scenes/default.scene";
    const std::uint32_t width = argc > 2 ? std::stoul(argv[2]) : 192;
    const std::uint32_t height = argc > 3 ? std::stoul(argv[3]) : 128;
    const std::uint64_t spp = argc > 4 ? std::stoull(argv[4]) : 64;
    const std::string out_path = argc > 5 ? argv[5] : "render.ppm";

    std::size_t err_line = 0;
    auto scene = LoadSceneFile(scene_path, &err_line);
    if (!scene) {
        std::fprintf(stderr, "scene: %s (line %zu)\n",
                     std::string(scene.error().message).c_str(), err_line);
        return 1;
    }

    // Through the ASSET, exactly as a worker would: serialize, then validate on
    // the way back in. Rendering straight from the builder's output would skip
    // the one path that carries an attacker's bytes in production.
    const auto blob = SerializeBvh(BuildBvh(*scene));
    auto loaded = LoadBvh(blob);
    if (!loaded) {
        std::fprintf(stderr, "bvh: %s\n", std::string(loaded.error().message).c_str());
        return 1;
    }
    const Bvh& bvh = *loaded;
    std::printf("scene=%s prims=%zu nodes=%zu depth=%u address=%s\n",
                scene_path.c_str(), bvh.prims.size(), bvh.nodes.size(),
                bvh.max_depth, ContentAddress(blob).c_str());

    platform::GpuContext ctx;
    if (!platform::AcquireDevice(ctx)) {
        std::fprintf(stderr, "no WebGPU device available\n");
        return 1;
    }
    const std::string wgsl = ReadFile(std::string(P2PGPU_KERNEL_DIR) + "/pathtrace_tile.wgsl");
    if (wgsl.empty()) {
        std::fprintf(stderr, "could not read pathtrace_tile.wgsl\n");
        return 1;
    }

    const auto bytes_of = [](const auto& v) {
        const auto* p = reinterpret_cast<const std::byte*>(v.data());
        return std::vector<std::byte>(p, p + v.size() * sizeof(v[0]));
    };
    const auto nodes = bytes_of(bvh.nodes);
    const auto prims = bytes_of(bvh.prims);
    const auto mats = bytes_of(bvh.materials);
    const std::vector<std::span<const std::byte>> inputs{nodes, prims, mats};

    const TileGrid grid{width, height, 64, 64};
    Compositor comp(grid);
    // Explicit, not automatic: auto-exposure would make the preview's
    // brightness depend on which tiles have arrived, so the image would breathe
    // as it converges and nobody could tell that from the render changing.
    comp.SetExposure(0.45F);
    std::printf("image=%ux%u tiles=%u spp=%llu\n", width, height, grid.tile_count(),
                static_cast<unsigned long long>(spp));

    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    for (std::uint32_t i = 0; i < grid.tile_count(); ++i) {
        const auto tile = grid.TileAt(i);
        PathTraceParams p{};
        p.tile_x = tile->x;
        p.tile_y = tile->y;
        p.tile_w = tile->w;
        p.tile_h = tile->h;
        p.image_w = width;
        p.image_h = height;
        p.cam_origin[0] = 0.0F; p.cam_origin[1] = 1.4F; p.cam_origin[2] = 4.0F;
        p.cam_lower_left[0] = -aspect; p.cam_lower_left[1] = 0.35F; p.cam_lower_left[2] = 2.6F;
        p.cam_horizontal[0] = 2.0F * aspect;
        p.cam_vertical[1] = 2.0F;
        p.seed = 20260807U;
        p.max_bounces = 8;
        p.rr_start_bounce = 3;
        p.node_count = static_cast<std::uint32_t>(bvh.nodes.size());
        p.prim_count = static_cast<std::uint32_t>(bvh.prims.size());
        p.material_count = static_cast<std::uint32_t>(bvh.materials.size());

        const auto* pb = reinterpret_cast<const std::byte*>(&p);
        p2pgpu::worker::TaskRequest req{};
        req.wgsl = wgsl;
        req.entry_point = "main";
        req.params = std::span<const std::byte>(pb, sizeof(p));
        req.start_unit = 0;
        req.unit_count = spp;
        req.output_bytes = TileOutputBytes(tile->w, tile->h);
        req.workgroup_size = 8;
        req.workgroup_size_y = 8;
        req.invocations_x = tile->w;
        req.invocations_y = tile->h;
        req.inputs = inputs;

        // Chunked, so R4's ceiling is exercised rather than bypassed.
        const auto outcome = p2pgpu::worker::RunTask(ctx, req, std::max<std::uint64_t>(spp / 4, 1));
        if (!outcome) {
            std::fprintf(stderr, "tile %u failed to render\n", i);
            return 1;
        }
        if (!comp.AcceptTile(i, outcome->output)) {
            std::fprintf(stderr, "tile %u rejected by the compositor\n", i);
            return 1;
        }
    }
    platform::ReleaseDevice(ctx);

    const auto rgba = comp.RenderRgba();
    std::ofstream out(out_path, std::ios::binary);
    out << "P6\n" << width << " " << height << "\n255\n";
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        out.put(static_cast<char>(rgba[i]));
        out.put(static_cast<char>(rgba[i + 1]));
        out.put(static_cast<char>(rgba[i + 2]));
    }
    std::printf("wrote %s  min_samples=%.0f max_samples=%.0f tiles_with_data=%u\n",
                out_path.c_str(), comp.MinSamples(), comp.MaxSamples(),
                comp.TilesWithData());
    return 0;
}

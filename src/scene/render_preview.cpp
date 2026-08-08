// Dev tool: render a scene single-machine and write a PPM — step 5.15's visual
// proof, and the skeleton of 5.19's reference render.
//
// NOT a worker and NOT a coordinator. It wires the real pieces together —
// scene -> BVH -> asset round-trip -> GPU kernel -> compositor — so that an
// image can be LOOKED AT. Every automated check in this project compares
// numbers; a rendering bug that satisfies all of them still shows up instantly
// to an eye.

#include <chrono>
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
    // E2's control switch (5.22). Same work either way; only the upload cadence
    // changes.
    const bool no_accum = argc > 6 && std::string(argv[6]) == "--no-accumulation";

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

    double total_gpu_ms = 0.0;
    double total_transfer_ms = 0.0;
    double total_idle_ms = 0.0;
    std::uint64_t total_dispatches = 0;
    const auto wall_start = std::chrono::steady_clock::now();

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
        req.readback_every_chunk = no_accum;

        // Chunked, so R4's ceiling is exercised rather than bypassed.
        // 16 chunks, so the control has something to pay for per chunk and the
        // treatment has something to accumulate across. Identical in both runs:
        // the ONLY difference between the two conditions is the upload cadence.
        const auto outcome =
            p2pgpu::worker::RunTask(ctx, req, std::max<std::uint64_t>(spp / 16, 1));
        if (!outcome) {
            std::fprintf(stderr, "tile %u failed to render\n", i);
            return 1;
        }
        total_gpu_ms += outcome->stats.gpu_ms;
        total_transfer_ms += outcome->stats.transfer_ms;
        total_idle_ms += outcome->stats.idle_ms;
        total_dispatches += outcome->stats.dispatches;
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
    const double wall_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - wall_start).count();
    const double accounted = total_gpu_ms + total_transfer_ms + total_idle_ms;
    std::printf("wrote %s  min_samples=%.0f max_samples=%.0f tiles_with_data=%u\n",
                out_path.c_str(), comp.MinSamples(), comp.MaxSamples(),
                comp.TilesWithData());

    // ── E2 (5.21/5.22) ───────────────────────────────────────────────────
    //
    // `gpu_ms` is SUBMIT time, not GPU execution time — timestamp queries are
    // not in yet, so it is a LOWER BOUND. That does not weaken the comparison:
    // both conditions are measured the same way, and the quantity of interest
    // is the RATIO between them.
    std::printf("E2 accumulation=%s  wall=%.0fms  gpu=%.0fms (%.1f%%)  "
                "transfer=%.0fms (%.1f%%)  idle=%.0fms (%.1f%%)  dispatches=%llu\n",
                no_accum ? "OFF (control)" : "ON",
                wall_ms,
                total_gpu_ms, 100.0 * total_gpu_ms / accounted,
                total_transfer_ms, 100.0 * total_transfer_ms / accounted,
                total_idle_ms, 100.0 * total_idle_ms / accounted,
                static_cast<unsigned long long>(total_dispatches));
    return 0;
}

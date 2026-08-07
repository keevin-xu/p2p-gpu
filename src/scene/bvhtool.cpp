#include <cstdio>
#include <string>
#include <fstream>
#include <sstream>
#include "p2pgpu/scene/scene.hpp"
#include "p2pgpu/scene/bvh.hpp"
int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: bvhtool <scene>\n"); return 2; }
    auto sc = p2pgpu::scene::LoadSceneFile(argv[1]);
    if (!sc) { std::fprintf(stderr, "scene: %s\n", std::string(sc.error().message).c_str()); return 1; }
    const auto bvh = p2pgpu::scene::BuildBvh(*sc);
    const auto blob = p2pgpu::scene::SerializeBvh(bvh);
    const auto addr = p2pgpu::scene::ContentAddress(blob);
    std::printf("prims=%zu nodes=%zu materials=%zu depth=%u max_leaf=%u bytes=%zu\n",
                bvh.prims.size(), bvh.nodes.size(), bvh.materials.size(),
                bvh.max_depth, bvh.max_leaf_prims, blob.size());
    std::printf("address=%s\n", addr.c_str());
    auto back = p2pgpu::scene::LoadBvh(blob);
    if (!back) { std::fprintf(stderr, "ROUNDTRIP FAILED: %s\n", std::string(back.error().message).c_str()); return 1; }
    std::printf("roundtrip ok: nodes=%zu depth=%u\n", back->nodes.size(), back->max_depth);
    return 0;
}

// CPU path tracer — step 5.19. Ground truth; see the header for why a
// single-machine GPU render is not one.
//
// Written from the rendering equation, NOT transliterated from
// kernels/pathtrace_tile.wgsl. Different RNG, different traversal order, same
// integral. An implementation that accidentally mirrors the kernel's mistakes
// is not a reference.

#include "p2pgpu/kernels/pathtrace_reference.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace p2pgpu::kernels {
namespace {

struct V3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
V3 operator*(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
V3 Norm(V3 a) {
    const double len = std::sqrt(Dot(a, a));
    return len > 0.0 ? a * (1.0 / len) : a;
}
V3 FromArray(const float* p) { return {p[0], p[1], p[2]}; }

// DOUBLE precision throughout, unlike the kernel's f32 (K5 forbids fp64 on the
// GPU, and nothing forbids it here). A reference carrying the same rounding as
// the thing it checks would agree with it about rounding errors too.
constexpr double kTMin = 1e-4;
constexpr double kTMax = 1e30;

struct Hit {
    double t = kTMax;
    V3 position;
    V3 normal;
    std::uint32_t material = 0;
    bool hit = false;
};

bool HitSphere(const scene::BvhPrim& p, V3 o, V3 d, double t_max, Hit& out) {
    const V3 centre = FromArray(p.a);
    const V3 oc = o - centre;
    const double a = Dot(d, d);
    const double half_b = Dot(oc, d);
    const double c = Dot(oc, oc) - static_cast<double>(p.radius) * p.radius;
    const double disc = half_b * half_b - a * c;
    if (disc < 0.0) {
        return false;
    }
    const double sq = std::sqrt(disc);
    double root = (-half_b - sq) / a;
    if (root < kTMin || root > t_max) {
        root = (-half_b + sq) / a;
        if (root < kTMin || root > t_max) {
            return false;
        }
    }
    out.t = root;
    out.position = o + d * root;
    out.normal = (out.position - centre) * (1.0 / p.radius);
    out.material = p.material;
    out.hit = true;
    return true;
}

bool HitTriangle(const scene::BvhPrim& p, V3 o, V3 d, double t_max, Hit& out) {
    const V3 va = FromArray(p.a);
    const V3 e1 = FromArray(p.b) - va;
    const V3 e2 = FromArray(p.c) - va;
    const V3 h = Cross(d, e2);
    const double det = Dot(e1, h);
    if (std::abs(det) < 1e-12) {
        return false;
    }
    const double inv = 1.0 / det;
    const V3 s = o - va;
    const double u = inv * Dot(s, h);
    if (u < 0.0 || u > 1.0) {
        return false;
    }
    const V3 q = Cross(s, e1);
    const double v = inv * Dot(d, q);
    if (v < 0.0 || u + v > 1.0) {
        return false;
    }
    const double t = inv * Dot(e2, q);
    if (t < kTMin || t > t_max) {
        return false;
    }
    out.t = t;
    out.position = o + d * t;
    V3 n = Norm(Cross(e1, e2));
    if (Dot(n, d) > 0.0) {
        n = n * -1.0;   // two-sided, matching the kernel's choice
    }
    out.normal = n;
    out.material = p.material;
    out.hit = true;
    return true;
}

/// BRUTE FORCE over every primitive — the BVH is deliberately NOT traversed.
///
/// The tree is the single most likely thing to be wrong (D-0070 found two
/// defects in its loader alone), and a reference that walks the same tree would
/// inherit the same mistake and agree with the GPU about a scene neither is
/// rendering. Testing every primitive is O(n) and cannot be wrong about which
/// ones exist.
Hit Trace(const scene::Bvh& bvh, V3 o, V3 d) {
    Hit best;
    for (const auto& p : bvh.prims) {
        Hit candidate;
        bool got = false;
        if (p.kind == scene::kPrimSphere) {
            got = HitSphere(p, o, d, best.t, candidate);
        } else {
            got = HitTriangle(p, o, d, best.t, candidate);
        }
        if (got && candidate.t < best.t) {
            best = candidate;
        }
    }
    return best;
}

V3 Sky(V3 d) {
    const double t = 0.5 * (Norm(d).y + 1.0);
    return V3{1.0, 1.0, 1.0} * (1.0 - t) + V3{0.5, 0.7, 1.0} * t;
}

/// Cosine-weighted hemisphere sample. Same distribution as the kernel, arrived
/// at independently — a different basis construction, and `std::mt19937` rather
/// than PCG, so agreement cannot come from drawing identical numbers.
V3 CosineHemisphere(V3 n, double r1, double r2) {
    V3 up = std::abs(n.x) > 0.9 ? V3{0.0, 1.0, 0.0} : V3{1.0, 0.0, 0.0};
    const V3 tangent = Norm(Cross(up, n));
    const V3 bitangent = Cross(n, tangent);
    const double phi = 2.0 * 3.14159265358979323846 * r1;
    const double cos_theta = std::sqrt(1.0 - r2);
    const double sin_theta = std::sqrt(r2);
    return Norm(tangent * (std::cos(phi) * sin_theta) +
                bitangent * (std::sin(phi) * sin_theta) + n * cos_theta);
}

V3 Radiance(const scene::Bvh& bvh, const ReferenceRequest& req, V3 o, V3 d,
            std::mt19937& rng, std::uniform_real_distribution<double>& uni) {
    V3 throughput{1.0, 1.0, 1.0};
    V3 out{0.0, 0.0, 0.0};

    for (std::uint32_t bounce = 0; bounce < req.max_bounces; ++bounce) {
        const Hit h = Trace(bvh, o, d);
        if (!h.hit) {
            out = out + throughput * Sky(d);
            break;
        }
        const std::uint32_t mi = std::min<std::uint32_t>(
            h.material, static_cast<std::uint32_t>(bvh.materials.size()) - 1);
        const auto& m = bvh.materials[mi];
        const V3 albedo{m.albedo[0], m.albedo[1], m.albedo[2]};

        if (m.kind == 2) {   // emissive
            out = out + throughput * albedo;
            break;
        }
        if (m.kind == 1) {   // metal
            const V3 nd = Norm(d);
            const V3 reflected = nd - h.normal * (2.0 * Dot(nd, h.normal));
            const V3 jitter = CosineHemisphere(h.normal, uni(rng), uni(rng)) * m.fuzz;
            d = Norm(reflected + jitter);
            if (Dot(d, h.normal) <= 0.0) {
                break;
            }
        } else {
            d = CosineHemisphere(h.normal, uni(rng), uni(rng));
        }
        throughput = throughput * albedo;
        o = h.position;

        // Russian roulette, WITH the survival division. Without it the estimator
        // is biased dark, which would make the reference disagree with a correct
        // kernel — and the natural conclusion would be that the kernel is wrong.
        if (bounce >= req.rr_start_bounce) {
            const double p = std::clamp(
                std::max({throughput.x, throughput.y, throughput.z}), 0.05, 0.95);
            if (uni(rng) > p) {
                break;
            }
            throughput = throughput * (1.0 / p);
        }
    }
    return out;
}

}  // namespace

std::vector<float> PathTraceReference(const scene::Bvh& bvh,
                                      const ReferenceRequest& req) {
    std::vector<float> out(static_cast<std::size_t>(req.tile_w) * req.tile_h * 4, 0.0F);
    if (bvh.prims.empty() || bvh.materials.empty()) {
        return out;
    }

    const V3 origin = FromArray(req.camera.origin);
    const V3 lower_left = FromArray(req.camera.lower_left);
    const V3 horizontal = FromArray(req.camera.horizontal);
    const V3 vertical = FromArray(req.camera.vertical);

    for (std::uint32_t ty = 0; ty < req.tile_h; ++ty) {
        for (std::uint32_t tx = 0; tx < req.tile_w; ++tx) {
            const std::uint32_t px = req.tile_x + tx;
            const std::uint32_t py = req.tile_y + ty;

            // Seeded per pixel so a partial render is reproducible and two runs
            // of the same request agree — the reference has to be stable or a
            // disagreement cannot be attributed.
            std::mt19937 rng(req.seed * 2654435761u + py * 73856093u + px * 19349663u);
            std::uniform_real_distribution<double> uni(0.0, 1.0);

            V3 sum;
            for (std::uint64_t s = 0; s < req.samples; ++s) {
                const double u = (px + uni(rng)) / static_cast<double>(req.image_w);
                const double v = (py + uni(rng)) / static_cast<double>(req.image_h);
                const V3 dir =
                    Norm(lower_left + horizontal * u + vertical * v - origin);
                sum = sum + Radiance(bvh, req, origin, dir, rng, uni);
            }
            const std::size_t at = (static_cast<std::size_t>(ty) * req.tile_w + tx) * 4;
            out[at + 0] = static_cast<float>(sum.x);
            out[at + 1] = static_cast<float>(sum.y);
            out[at + 2] = static_cast<float>(sum.z);
            out[at + 3] = static_cast<float>(req.samples);
        }
    }
    return out;
}

}  // namespace p2pgpu::kernels

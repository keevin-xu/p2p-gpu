// pathtrace_tile_v1 — step 5.6. Workload A.
//
// R5 gate: D-0068. `min_iterations` is derived there, not guessed here.
// Asset layout: D-0069. Every struct below MIRRORS a C++ struct byte for byte;
// `include/p2pgpu/kernels/pathtrace_params.hpp` static_asserts the sizes and
// every offset (5.11, CONVENTIONS.md §5). Drift does not crash — it misreads
// the tree and renders a plausible wrong picture.
//
// ── K1: THE CHUNK WINDOW IS THE FIRST 8 BYTES ────────────────────────────
// `start_unit` at byte 0, `unit_count` at byte 4, always (D-0033). The host is
// kernel-agnostic: it rewrites exactly those two words before each dispatch and
// copies the rest verbatim. A uniform struct is 16-byte aligned as a whole, so
// the vec3 camera fields below are PADDED into place rather than reordered —
// putting a vec3 first would push the window off byte 0 and the host would
// silently overwrite camera data with chunk bounds. No runtime check can catch
// that; the bytes are valid either way.
//
// ── K2: DETERMINISTIC GIVEN (seed, unit_range) ───────────────────────────
// A WORK UNIT IS ONE SAMPLE OF THE WHOLE TILE, so `unit_count` is a count of
// samples per pixel and `start_unit` is the first sample index. The RNG is
// counter-based, seeded from `(seed, pixel_index, sample_index)` and nothing
// else — no state carried between samples, dispatches or invocations. So any
// worker asked for the same range produces the same estimate, which is what
// makes replication and speculative re-execution sound.
//
// ── K3: `statistical`, and what that does NOT mean ───────────────────────
// Two workers given DIFFERENT sample ranges produce different images, which is
// why the manifest declares `statistical`. Two workers given the SAME range
// agree to within float ULP, by K2 above. The 5.14 comparator must not treat
// this class as licence for loose agreement on identical work (D-0068).
//
// ── K5: no fp64 anywhere. WebGPU has none. ───────────────────────────────

// ── The BVH asset, exactly as D-0069 lays it out ─────────────────────────
// vec3<f32> has align 16 / size 12 in WGSL, so the u32 after each one fills the
// padding the alignment would otherwise waste. That is why these come out at
// 32 / 64 / 32 bytes with no explicit padding words beyond the named ones.

struct BvhNode {
    bmin: vec3<f32>,           // offset 0
    left_or_first: u32,        // offset 12
    bmax: vec3<f32>,           // offset 16
    count_and_leaf: u32,       // offset 28  (bit 31 = leaf)
};                             // size 32

struct BvhPrim {
    kind: u32,                 // offset 0   (0 = sphere, 1 = triangle)
    material: u32,             // offset 4
    pad0: u32,                 // offset 8
    pad1: u32,                 // offset 12
    a: vec3<f32>,              // offset 16  sphere centre / triangle vertex A
    radius: f32,               // offset 28
    b: vec3<f32>,              // offset 32
    pad2: f32,                 // offset 44
    c: vec3<f32>,              // offset 48
    pad3: f32,                 // offset 60
};                             // size 64

struct BvhMaterial {
    albedo: vec3<f32>,         // offset 0
    fuzz: f32,                 // offset 12
    kind: u32,                 // offset 16  (0 lambertian, 1 metal, 2 emissive)
    pad0: u32,                 // offset 20
    pad1: u32,                 // offset 24
    pad2: u32,                 // offset 28
};                             // size 32

struct Params {
    // THE CHUNK WINDOW. Bytes 0-7. Do not move (K1 / D-0033).
    start_unit: u32,           // offset 0   first sample index
    unit_count: u32,           // offset 4   samples in this dispatch

    tile_x: u32,               // offset 8   tile origin in image pixels
    tile_y: u32,               // offset 12
    tile_w: u32,               // offset 16
    tile_h: u32,               // offset 20
    image_w: u32,              // offset 24
    image_h: u32,              // offset 28

    // vec3 forces 16-byte alignment; the trailing scalar in each pair is a real
    // field rather than dead padding wherever one was available to place.
    cam_origin: vec3<f32>,     // offset 32
    seed: u32,                 // offset 44

    cam_lower_left: vec3<f32>, // offset 48
    max_bounces: u32,          // offset 60

    cam_horizontal: vec3<f32>, // offset 64
    node_count: u32,           // offset 76

    cam_vertical: vec3<f32>,   // offset 80
    prim_count: u32,           // offset 92

    material_count: u32,       // offset 96
    rr_start_bounce: u32,      // offset 100  Russian roulette begins here
    pad0: u32,                 // offset 104
    pad1: u32,                 // offset 108
};                             // size 112

@group(0) @binding(0) var<uniform> params: Params;
// The accumulator (5.8). read_write because a dispatch ADDS to what previous
// dispatches left. Zeroed once per TASK by the host, never between chunks —
// re-initialising mid-task is the "state leaks between chunks" bug, and it
// would present as scheduling nondeterminism (the most expensive thing to
// misdiagnose in a system whose verification story is determinism).
@group(0) @binding(1) var<storage, read_write> accum: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read> nodes: array<BvhNode>;
@group(0) @binding(3) var<storage, read> prims: array<BvhPrim>;
@group(0) @binding(4) var<storage, read> materials: array<BvhMaterial>;

const LEAF_FLAG: u32 = 0x80000000u;
const PRIM_SPHERE: u32 = 0u;
const PRIM_TRIANGLE: u32 = 1u;
const MAT_LAMBERTIAN: u32 = 0u;
const MAT_METAL: u32 = 1u;
const MAT_EMISSIVE: u32 = 2u;

const T_MIN: f32 = 0.001;      // shadow-acne epsilon
const T_MAX: f32 = 1.0e30;
const PI: f32 = 3.14159265358979;
// Traversal stack. 64 is deeper than any tree the builder can produce for a
// scene that fits our K4 buffer limits (measured depth 13 at 3006 primitives),
// and it is a FIXED bound on purpose: a stack that could overflow is a hang,
// and R4's chunking cannot interrupt a running shader.
const STACK_SIZE: u32 = 64u;

// ── K2: counter-based RNG ────────────────────────────────────────────────
// PCG hash. Integer-only, so it is bit-identical on every vendor — the same
// reason `brute_search_v1` could be declared `exact`. A stateful/sequential RNG
// would make a sample depend on how many samples preceded it in THIS dispatch,
// which is precisely what chunking changes, so chunk invariance would fail.
fn pcg_hash(input: u32) -> u32 {
    let state = input * 747796405u + 2891336453u;
    let word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// One stream per (pixel, sample, purpose). `purpose` keeps the two numbers a
// hemisphere sample needs from being correlated, which shows up as visible
// structure in the noise rather than as an obvious bug.
fn rand_u32(pixel_index: u32, sample_index: u32, purpose: u32) -> u32 {
    var h = pcg_hash(params.seed ^ pixel_index);
    h = pcg_hash(h ^ sample_index);
    return pcg_hash(h ^ purpose);
}

fn rand_f32(pixel_index: u32, sample_index: u32, purpose: u32) -> f32 {
    // 24 bits into [0,1). Dividing by 2^24 rather than 2^32 keeps the result
    // exactly representable in f32, so the value is identical across vendors
    // instead of differing in the last ULP by rounding mode.
    return f32(rand_u32(pixel_index, sample_index, purpose) >> 8u) *
           (1.0 / 16777216.0);
}

struct Hit {
    t: f32,
    position: vec3<f32>,
    normal: vec3<f32>,
    material: u32,
    hit: bool,
};

fn ray_aabb(origin: vec3<f32>, inv_dir: vec3<f32>, bmin: vec3<f32>,
            bmax: vec3<f32>, t_max: f32) -> bool {
    let t0 = (bmin - origin) * inv_dir;
    let t1 = (bmax - origin) * inv_dir;
    let tsmall = min(t0, t1);
    let tbig = max(t0, t1);
    let t_enter = max(max(tsmall.x, tsmall.y), max(tsmall.z, T_MIN));
    let t_exit = min(min(tbig.x, tbig.y), min(tbig.z, t_max));
    return t_enter <= t_exit;
}

fn hit_sphere(p: BvhPrim, origin: vec3<f32>, dir: vec3<f32>, t_max: f32,
              out: ptr<function, Hit>) -> bool {
    let oc = origin - p.a;
    let a = dot(dir, dir);
    let half_b = dot(oc, dir);
    let c = dot(oc, oc) - p.radius * p.radius;
    let disc = half_b * half_b - a * c;
    if (disc < 0.0) {
        return false;
    }
    let sqrtd = sqrt(disc);
    var root = (-half_b - sqrtd) / a;
    if (root < T_MIN || root > t_max) {
        root = (-half_b + sqrtd) / a;
        if (root < T_MIN || root > t_max) {
            return false;
        }
    }
    (*out).t = root;
    (*out).position = origin + dir * root;
    (*out).normal = ((*out).position - p.a) / p.radius;
    (*out).material = p.material;
    (*out).hit = true;
    return true;
}

// Moller-Trumbore.
fn hit_triangle(p: BvhPrim, origin: vec3<f32>, dir: vec3<f32>, t_max: f32,
                out: ptr<function, Hit>) -> bool {
    let e1 = p.b - p.a;
    let e2 = p.c - p.a;
    let h = cross(dir, e2);
    let det = dot(e1, h);
    if (abs(det) < 1.0e-8) {
        return false;   // ray parallel to the triangle
    }
    let inv_det = 1.0 / det;
    let s = origin - p.a;
    let u = inv_det * dot(s, h);
    if (u < 0.0 || u > 1.0) {
        return false;
    }
    let q = cross(s, e1);
    let v = inv_det * dot(dir, q);
    if (v < 0.0 || u + v > 1.0) {
        return false;
    }
    let t = inv_det * dot(e2, q);
    if (t < T_MIN || t > t_max) {
        return false;
    }
    (*out).t = t;
    (*out).position = origin + dir * t;
    // Two-sided: face the normal against the ray so a triangle lights from
    // whichever side it is seen. A one-sided normal makes the ground plane
    // black from below, which reads as a shading bug rather than a choice.
    let n = normalize(cross(e1, e2));
    (*out).normal = select(-n, n, dot(n, dir) < 0.0);
    (*out).material = p.material;
    (*out).hit = true;
    return true;
}

fn trace(origin: vec3<f32>, dir: vec3<f32>) -> Hit {
    var best: Hit;
    best.hit = false;
    best.t = T_MAX;

    // Guard against a division that would produce inf/NaN and poison the slab
    // test. `1/0` is +inf in f32 and the comparisons still behave, but a
    // literal zero component is worth pinning rather than relying on that.
    let safe_dir = select(dir, vec3<f32>(1.0e-20), abs(dir) < vec3<f32>(1.0e-20));
    let inv_dir = vec3<f32>(1.0) / safe_dir;

    var stack: array<u32, 64>;
    var sp: u32 = 0u;
    stack[0] = 0u;
    sp = 1u;

    loop {
        if (sp == 0u) {
            break;
        }
        sp = sp - 1u;
        let node_index = stack[sp];
        // Bounds guard. `LoadBvh` already proved every index is in range
        // (D-0069/D-0070) — this is the belt to that braces, because WGSL
        // CLAMPS an out-of-range index rather than faulting, so a validator
        // regression would silently render the wrong image instead of failing.
        if (node_index >= params.node_count) {
            continue;
        }
        let node = nodes[node_index];
        if (!ray_aabb(origin, inv_dir, node.bmin, node.bmax, best.t)) {
            continue;
        }

        if ((node.count_and_leaf & LEAF_FLAG) != 0u) {
            let first = node.left_or_first;
            let count = node.count_and_leaf & ~LEAF_FLAG;
            for (var i: u32 = 0u; i < count; i = i + 1u) {
                let pi = first + i;
                if (pi >= params.prim_count) {
                    continue;
                }
                let p = prims[pi];
                var candidate: Hit;
                candidate.hit = false;
                var got = false;
                if (p.kind == PRIM_SPHERE) {
                    got = hit_sphere(p, origin, dir, best.t, &candidate);
                } else {
                    got = hit_triangle(p, origin, dir, best.t, &candidate);
                }
                if (got && candidate.t < best.t) {
                    best = candidate;
                }
            }
        } else {
            // Both children, unordered. Front-to-back ordering would prune
            // more, and is deliberately not done yet: it changes the order of
            // floating-point comparisons and therefore which of two coincident
            // hits wins, which is a determinism question (R6) that belongs with
            // 5.14's comparator rather than smuggled in as an optimisation.
            if (sp + 2u <= STACK_SIZE) {
                stack[sp] = node.left_or_first;
                stack[sp + 1u] = node.left_or_first + 1u;
                sp = sp + 2u;
            }
        }
    }
    return best;
}

// Cosine-weighted hemisphere sample, built on an orthonormal basis around n.
// Cosine-weighted rather than uniform because the Lambert BRDF's cos(theta)
// then cancels against the pdf, leaving throughput *= albedo with no division
// — fewer operations AND no chance of a near-zero pdf producing a fireflies
// artefact that looks like a Byzantine worker to the 5.14 comparator.
fn cosine_hemisphere(n: vec3<f32>, r1: f32, r2: f32) -> vec3<f32> {
    // Building the basis by branching on the smallest component avoids the
    // degenerate cross product when n is near-parallel to the chosen axis.
    var up = vec3<f32>(1.0, 0.0, 0.0);
    if (abs(n.x) > 0.9) {
        up = vec3<f32>(0.0, 1.0, 0.0);
    }
    let tangent = normalize(cross(up, n));
    let bitangent = cross(n, tangent);

    let phi = 2.0 * PI * r1;
    let cos_theta = sqrt(1.0 - r2);
    let sin_theta = sqrt(r2);
    return normalize(tangent * cos(phi) * sin_theta +
                     bitangent * sin(phi) * sin_theta +
                     n * cos_theta);
}

fn sky(dir: vec3<f32>) -> vec3<f32> {
    let t = 0.5 * (normalize(dir).y + 1.0);
    return mix(vec3<f32>(1.0, 1.0, 1.0), vec3<f32>(0.5, 0.7, 1.0), t);
}

fn radiance(pixel_index: u32, sample_index: u32, origin_in: vec3<f32>,
            dir_in: vec3<f32>) -> vec3<f32> {
    var origin = origin_in;
    var dir = dir_in;
    var throughput = vec3<f32>(1.0);
    var radiance_out = vec3<f32>(0.0);

    for (var bounce: u32 = 0u; bounce < params.max_bounces; bounce = bounce + 1u) {
        let h = trace(origin, dir);
        if (!h.hit) {
            radiance_out = radiance_out + throughput * sky(dir);
            break;
        }

        let mat_index = min(h.material, params.material_count - 1u);
        let m = materials[mat_index];

        if (m.kind == MAT_EMISSIVE) {
            radiance_out = radiance_out + throughput * m.albedo;
            break;
        }

        // `purpose` is derived from the bounce so two bounces of one sample draw
        // from different streams. Reusing one stream across bounces correlates
        // successive scatter directions and produces structured noise.
        let base = bounce * 4u;
        if (m.kind == MAT_METAL) {
            let reflected = reflect(normalize(dir), h.normal);
            let f1 = rand_f32(pixel_index, sample_index, base + 0u);
            let f2 = rand_f32(pixel_index, sample_index, base + 1u);
            let jitter = cosine_hemisphere(h.normal, f1, f2) * m.fuzz;
            dir = normalize(reflected + jitter);
            if (dot(dir, h.normal) <= 0.0) {
                break;   // scattered below the surface: absorbed
            }
        } else {
            let r1 = rand_f32(pixel_index, sample_index, base + 0u);
            let r2 = rand_f32(pixel_index, sample_index, base + 1u);
            dir = cosine_hemisphere(h.normal, r1, r2);
        }
        throughput = throughput * m.albedo;
        origin = h.position;

        // Russian roulette, and it is UNBIASED only because survivors are
        // divided by their survival probability. Terminating without that
        // division would darken the image in a way that looks like a shading
        // bug and would make honest workers disagree with the reference.
        if (bounce >= params.rr_start_bounce) {
            let p = clamp(max(throughput.x, max(throughput.y, throughput.z)),
                          0.05, 0.95);
            let r = rand_f32(pixel_index, sample_index, base + 2u);
            if (r > p) {
                break;
            }
            throughput = throughput / p;
        }
    }
    return radiance_out;
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= params.tile_w || gid.y >= params.tile_h) {
        return;
    }
    let pixel_local = gid.y * params.tile_w + gid.x;

    // The RNG stream is keyed on the pixel's position in the IMAGE, not in the
    // tile. Otherwise every tile would draw the identical noise and the seams
    // between them would be visible as repeated speckle.
    let px = params.tile_x + gid.x;
    let py = params.tile_y + gid.y;
    let pixel_global = py * params.image_w + px;

    var sum = vec3<f32>(0.0);
    for (var s: u32 = 0u; s < params.unit_count; s = s + 1u) {
        let sample_index = params.start_unit + s;

        // Jitter within the pixel for antialiasing. Keyed on the sample index,
        // so the same sample of the same pixel lands identically on every
        // device (K2) regardless of how the range was chunked.
        let jx = rand_f32(pixel_global, sample_index, 0xF00u);
        let jy = rand_f32(pixel_global, sample_index, 0xF01u);
        let u = (f32(px) + jx) / f32(params.image_w);
        // ROW 0 IS THE TOP OF THE IMAGE; v=0 IS `cam_lower_left`, THE BOTTOM OF
        // THE VIEW. The flip is what reconciles the two, and leaving it out
        // renders the whole scene upside down (D-0099).
        let v = 1.0 - (f32(py) + jy) / f32(params.image_h);

        let dir = params.cam_lower_left + params.cam_horizontal * u +
                  params.cam_vertical * v - params.cam_origin;
        sum = sum + radiance(pixel_global, sample_index, params.cam_origin,
                             normalize(dir));
    }

    // ACCUMULATE (5.8). `+=`, never `=`: previous dispatches of this task left
    // their samples here, and the host zeroes the buffer once per TASK rather
    // than per chunk. The `.w` lane carries the sample count so the coordinator
    // can composite tiles that have received different amounts of work (5.15)
    // — a running SUM plus a count, not a running average, because averaging on
    // the worker would lose the weight needed to merge two partial results.
    let prev = accum[pixel_local];
    accum[pixel_local] = vec4<f32>(prev.xyz + sum,
                                   prev.w + f32(params.unit_count));
}

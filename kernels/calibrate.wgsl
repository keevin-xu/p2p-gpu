// calibrate_v1 — throughput benchmark and join-time calibration kernel.
//
// Design and the R5 calculation: docs/DECISIONS.md D-0018 (written first, R9).
// Manifest entry: kernels/manifest.toml.
//
// Unlike the smoke_* kernels this one IS scheduled — step 2.11 sends it to
// every worker on join and turns its runtime into the throughput score that
// drives adaptive task sizing. So its FLOP count must be exact, and it must
// measure ALU throughput rather than anything else.
//
// ── WHY FOUR ACCUMULATORS ────────────────────────────────────────────────
// A single dependent FMA chain measures LATENCY: every op waits for the one
// before it, and the ALUs sit mostly idle. Four independent chains give the
// scheduler enough instruction-level parallelism to actually saturate them.
// This is the single most common reason a benchmark reports ~2% of peak and
// the hardware gets blamed (step 0.11 expects 10-20%).
//
// ── WHY THE WORKGROUP REDUCTION ──────────────────────────────────────────
// Every invocation's result MUST be observable. An earlier draft had only
// invocation 0 write its own accumulators, which lets the compiler sink the
// whole loop into that branch — 255 of 256 invocations would do no work, and
// the measured throughput would be inflated by up to 256x with nothing
// obviously wrong. The shared-memory reduction makes each invocation's output
// load-bearing, so none of it can be eliminated.
//
// R5: ratio = 512 x iterations, so iterations = 2048 gives 1.05e6. Intensity
// here is a TUNING KNOB, not a property of the workload — the whole thesis of
// D-0001 in miniature.

struct Params {
    iterations : u32,   // loop passes per invocation; 4 FMA each
    seed       : u32,   // keeps starting values off exact 0/1 (see below)
};

@group(0) @binding(0) var<uniform>             params : Params;
@group(0) @binding(1) var<storage, read_write> output : array<f32>;

// Per-invocation partial sums. Sized to match @workgroup_size below; WGSL
// needs a constant here, so the two must be kept in step by hand.
const kWorkgroupSize : u32 = 256u;
var<workgroup> partial : array<f32, 256>;

@compute @workgroup_size(256)
fn main(@builtin(local_invocation_id) lid : vec3<u32>,
        @builtin(global_invocation_id) gid : vec3<u32>,
        @builtin(workgroup_id) wid : vec3<u32>) {

    // Derive starting values from the invocation index and seed. Deliberately
    // NOT exact 0.0 or 1.0: denormals and exact-zero operands can take
    // different hardware paths on some GPUs, which would make the measurement
    // depend on constant folding rather than on ALU throughput.
    let base = f32((gid.x ^ params.seed) & 0xFFFFu) * 0.0001 + 1.0001;

    var a0 = base;
    var a1 = base * 1.0003;
    var a2 = base * 1.0007;
    var a3 = base * 1.0011;

    let m = 1.0000001;
    let c = 0.0000001;

    // 4 FMAs per pass, 2 FLOP each => 8 FLOP per pass per invocation.
    // The chains are mutually independent, which is the point.
    for (var i = 0u; i < params.iterations; i = i + 1u) {
        a0 = fma(a0, m, c);
        a1 = fma(a1, m, c);
        a2 = fma(a2, m, c);
        a3 = fma(a3, m, c);
    }

    partial[lid.x] = a0 + a1 + a2 + a3;

    // Protects `partial`: every invocation must have written its slot before
    // invocation 0 reads any of them (K8).
    workgroupBarrier();

    // ONE writer per workgroup, so there is no race on `output`. The reduction
    // costs 256 adds against ~4.2e6 FLOP of real work per workgroup (~0.006%),
    // which is well below measurement noise and is excluded from the FLOP count.
    if (lid.x == 0u) {
        var sum = 0.0;
        for (var k = 0u; k < kWorkgroupSize; k = k + 1u) {
            sum = sum + partial[k];
        }
        output[wid.x] = sum;
    }
}

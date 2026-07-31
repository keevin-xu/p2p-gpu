// R6 evidence generator — step 0.16. NOT a workload kernel (no manifest entry,
// no R5 calculation: it is never scheduled, sized, or replicated).
//
// ── WHAT THIS IS FOR ─────────────────────────────────────────────────────
// R6 asserts that two HONEST GPUs from different vendors disagree in the last
// few ULPs, which is why replication cannot compare floats bitwise. That is
// currently an assumption inherited from the literature. This kernel turns it
// into a measurement, and the measured spread is what sets the `Tolerant`
// epsilons in step 3.2 — otherwise those numbers are guesses.
//
// ── WHY THE OTHER KERNELS CANNOT DO THIS ─────────────────────────────────
// smoke_double computes `x * 2.0`, which only adjusts a float exponent: exactly
// representable, so every implementation agrees and it would "prove" R6 false.
// smoke_hash is integer. Neither can expose divergence. This kernel is built
// specifically to maximise it, by stacking the operations that vendors
// implement differently:
//
//   1. TRANSCENDENTALS (sin/exp/log/pow). Not exactly specified by IEEE-754;
//      each vendor ships its own polynomial approximation. The single largest
//      source of honest disagreement.
//   2. FMA CONTRACTION. `a*b + c` may be fused (one rounding) or split (two).
//      Compilers choose differently, and the results differ in the last ulp.
//   3. SUMMATION ORDER. Float addition is not associative, so a reduction in a
//      different order gives a different answer.
//   4. DIVISION AND inverseSqrt, both commonly approximated.
//
// A kernel this hostile to reproducibility is exactly the point: if two vendors
// agree here, they agree anywhere. If they disagree, the magnitude tells us
// what tolerance honest workers actually need.
//
// Output is raw f32 BIT PATTERNS so the comparison can be exact and the
// difference expressed in ULPs rather than a relative error that hides scale.

@group(0) @binding(0) var<storage, read>       input  : array<f32>;
@group(0) @binding(1) var<storage, read_write> output : array<f32>;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let i = gid.x;
    if (i >= arrayLength(&input)) {
        return;
    }

    let x = input[i];

    // Spread values across a wide dynamic range: divergence behaves
    // differently near 1.0, near zero, and out where exponents are large.
    let a = x * 0.001 + 0.5;
    let b = x * 7.0 + 1.0;
    let c = x * 1e-4 + 1e-3;

    // 1. Transcendentals — the biggest source of vendor disagreement.
    var acc = sin(a) * cos(b);
    acc = acc + exp(c) - log(b);
    acc = acc + pow(a, 1.7);
    acc = acc + tanh(a) * atan(b);

    // 2. FMA contraction: written so a compiler MAY fuse it. Whether it does is
    //    implementation-defined, and the two spellings round differently.
    acc = acc * 1.0000001 + 0.0000001;
    acc = fma(acc, 0.9999999, 0.0000002);

    // 3. Division and inverse square root — commonly approximated in hardware.
    acc = acc / (b + 1e-6);
    acc = acc + inverseSqrt(abs(a) + 1e-6);

    // 4. Order-dependent accumulation. Float addition is not associative, so a
    //    reordering optimisation shows up here.
    var sum = 0.0;
    for (var k = 0u; k < 16u; k = k + 1u) {
        let t = f32(k) * 0.1 + a;
        sum = sum + sin(t) / (t + 1.0);
    }

    output[i] = acc + sum;
}

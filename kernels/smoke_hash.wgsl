// Toolchain smoke test — step 0.9. out[i] = pcg_hash(in[i])
//
// ── NOT A WORKLOAD KERNEL ────────────────────────────────────────────────
// Deliberately absent from kernels/manifest.toml and deliberately has no R5
// calculation, for the same reason as smoke_double.wgsl: R5 gates kernels that
// get SCHEDULED. This one is never queued, sized, or replicated.
//
// ── WHY INTEGER, AND WHY A HASH ──────────────────────────────────────────
// This is the kernel step 0.9 compares across targets, and it is integer-only
// on purpose. `DeterminismClass::Exact` — bitwise comparison is VALID here, so
// a single differing bit is a genuine failure rather than a tolerance question.
//
// smoke_double.wgsl would be a weak test: multiplying by 2.0 only adjusts a
// float exponent, so it is exactly representable and would agree across two
// targets even if something subtle were wrong. A hash avalanches — every input
// bit affects roughly half the output bits — so ANY host-side asymmetry
// (a wrong buffer offset, a stride mistake, a truncated size, byte-order
// confusion) turns into a loud, obvious mismatch instead of a silent pass.
//
// PCG-style output permutation. This is also the shape K2 mandates for real
// kernels: derive values from a counter, never from stateful sequential RNG,
// so a given (seed, range) reproduces identically on any device. That property
// is what makes replication and speculative re-execution sound later.

@group(0) @binding(0) var<storage, read>       input  : array<u32>;
@group(0) @binding(1) var<storage, read_write> output : array<u32>;

fn pcg_hash(v: u32) -> u32 {
    let state = v * 747796405u + 2891336453u;
    let word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let i = gid.x;
    // Bounds check against the real element count, not the dispatch size: the
    // grid is rounded up to a whole workgroup and will overhang (K1).
    if (i >= arrayLength(&input)) {
        return;
    }
    output[i] = pcg_hash(input[i]);
}

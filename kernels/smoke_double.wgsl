// Toolchain smoke test — step 0.6 / 0.8. out[i] = in[i] * 2.0
//
// ── NOT A WORKLOAD KERNEL ────────────────────────────────────────────────
// Deliberately absent from kernels/manifest.toml, and deliberately has no R5
// arithmetic-intensity calculation. R5 gates kernels that get SCHEDULED — its
// purpose is to reject transfer-bound workloads before they are built. This
// one is never queued, never sized, never replicated; it exists only to prove
// a WGSL compute pipeline runs end to end on a given target. Adding it to the
// manifest would make the registry lie about what the system can execute.
//
// (For the record it would fail R5 spectacularly: 1 FLOP per 4 output bytes,
// i.e. 0.25 FLOP/byte against a 1e6 bar. That is precisely why it is a smoke
// test and not a workload.)
//
// Loaded VERBATIM by both worker targets. It is the same bytes on native and
// in the browser — which is what makes step 0.9's output comparison meaningful.

@group(0) @binding(0) var<storage, read>       input  : array<f32>;
@group(0) @binding(1) var<storage, read_write> output : array<f32>;

// K7 wants >= 64 to avoid wasting occupancy. 256 is the 1D default.
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let i = gid.x;
    // Bounds check against the real element count, not the dispatch size:
    // the grid is rounded up to a whole workgroup and will overhang (K1).
    if (i >= arrayLength(&input)) {
        return;
    }
    output[i] = input[i] * 2.0;
}

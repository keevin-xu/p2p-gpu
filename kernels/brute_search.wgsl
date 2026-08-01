// brute_search_v1 — Workload B. THE `Exact` VERIFICATION CASE.
//
// Design and the R5 calculation: docs/DECISIONS.md D-0029 (written first, R9).
// Manifest entry: kernels/manifest.toml.
//
// Searches a 64-bit keyspace range for candidates whose iterated hash matches a
// masked target. Integer-only, so bitwise replication comparison is genuinely
// valid rather than accidentally valid.
//
// ── NOT A SCIENTIFIC WORKLOAD ────────────────────────────────────────────
// This exists to debug the pipeline end to end without the path tracer's
// complexity confounding it, and to be the clean `Exact` case the validator is
// built against. It is not cryptanalysis, not proof-of-work, not a research
// result. See PROJECT_OVERVIEW.md §5 on framing.
//
// ── THE TRAP THIS KERNEL AVOIDS ──────────────────────────────────────────
// The obvious design — atomically append each match to an array — produces an
// array whose ORDER DEPENDS ON GPU SCHEDULING. Two *honest* workers would then
// return different bytes, bitwise `Exact` comparison would fail, and the
// validator would flag them as liars. That is precisely the R6 / D-0003
// disaster the determinism-class system exists to prevent, occurring inside the
// very kernel meant to be the clean case.
//
// So every output field is an ORDER-INDEPENDENT REDUCTION. Add, min, and xor
// are each commutative and associative, so the result is identical regardless
// of execution order, workgroup count, or how the range was partitioned.

// K1 / D-0033: `start_lo` and `unit_count` are the CHUNK WINDOW and MUST be the
// first two fields, at bytes 0 and 4. The kernel host is kernel-agnostic — it
// rewrites exactly those 8 bytes before every dispatch and copies the rest
// verbatim. Moving them makes the host write chunk bounds over another field,
// which produces a well-formed result computed over a garbage keyspace and no
// runtime check can catch it.
struct BruteSearchParams {
    start_lo   : u32,   // K1's (start_unit, unit_count) — the chunkable range
    unit_count : u32,
    base_hi    : u32,   // high half of the 64-bit keyspace cursor
    seed       : u32,
    target_bits: u32,   // match iff (H(x) & mask) == target_bits
                        //   NOT `target` — that is a RESERVED KEYWORD in WGSL
    mask       : u32,   // difficulty knob; tuned for ~1-10 matches per task
    rounds     : u32,   // pcg_hash iterations per candidate (manifest: 8)
    _pad       : u32,   // keeps the struct at 32 bytes; see the C++ static_assert
};

// 32 bytes, FIXED and independent of N. Growing this weakens the R5 ratio
// proportionally — see D-0029(d) before touching it.
struct BruteSearchResult {
    found_count : atomic<u32>,   // atomicAdd — |set| is order-independent
    min_match   : atomic<u32>,   // atomicMin — min(set); host inits 0xFFFFFFFFu
    match_xor   : atomic<u32>,   // atomicXor — fingerprints the SET itself
    // Zero-initialised by the host, never written here. No non-atomic field may
    // be written by the kernel: two invocations writing one address is a race
    // even when every write carries the same value (K8).
    reserved    : array<u32, 5>,
};

@group(0) @binding(0) var<uniform>             params : BruteSearchParams;
@group(0) @binding(1) var<storage, read_write> result : BruteSearchResult;

// Verbatim from kernels/smoke_hash.wgsl — do NOT write a new primitive.
//
// That function is already proven BITWISE-IDENTICAL across naga, Tint, and
// WebKit by the step 0.9 artifacts. Reusing it means that if `Exact` ever
// disagrees across vendors, the hash is eliminated as a suspect immediately and
// the bug is in the host. Inventing a new one throws away evidence we have
// already paid for.
fn pcg_hash(v: u32) -> u32 {
    let state = v * 747796405u + 2891336453u;
    let word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let i = gid.x;
    // Bounds-check against unit_count, NOT the dispatch size: the grid rounds up
    // to a whole workgroup and will overhang (K1).
    if (i >= params.unit_count) {
        return;
    }

    // The candidate's low half. A 1-second task is ~1.25e10 candidates, which
    // exceeds 2^32, and WGSL has no u64 — so the job owns a 64-bit keyspace
    // while each task carries base_hi plus a 32-bit (start_lo, unit_count)
    // window. Full 2^64 range, no 64-bit arithmetic in the shader.
    let lo = params.start_lo + i;

    // Pair mix: fold both halves and the seed into one 32-bit state, so the
    // high half actually influences the result. Without this, every task
    // sharing a start_lo window would search identical values.
    var h = lo ^ params.seed ^ (params.base_hi * 2654435769u);

    for (var r = 0u; r < params.rounds; r = r + 1u) {
        h = pcg_hash(h);
    }

    if ((h & params.mask) == params.target_bits) {
        // ALL THREE REDUCTIONS OPERATE ON `lo`, NEVER ON h.
        //
        // A candidate `lo` is visited exactly once per task, so atomicXor cannot
        // self-cancel. Hash values carry no such guarantee: two distinct
        // candidates can collide to the same h, and their xor contributions
        // would annihilate — silently under-reporting the match set while
        // found_count still incremented. `lo` is also what the coordinator needs
        // when it bisects the range to recover the actual matches.
        atomicAdd(&result.found_count, 1u);
        atomicMin(&result.min_match, lo);
        atomicXor(&result.match_xor, lo);
    }

    // CHUNK INVARIANCE (what step 4.2 tests). The result buffer is initialised
    // ONCE PER TASK by the host — zeros, with min_match = 0xFFFFFFFFu — and
    // never per chunk. Chunks accumulate into it. Because all three reductions
    // are partition-independent, one dispatch of N and four of N/4 produce
    // identical bytes. Re-initialising per chunk is exactly the "state leaks
    // between chunks" bug, and it would look like a scheduling nondeterminism.
    //
    // found_count and match_xor also cross-check the HOST: they derive from the
    // same events by different operations, so a candidate processed twice
    // (overlapping chunks) doubles the count while its xor contribution
    // cancels. The two disagreeing is a host bug, not a lying worker — a
    // distinction Phase 3 step 3.11 needs to keep straight.
}

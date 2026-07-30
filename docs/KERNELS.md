# Kernel Authoring Guide

WGSL kernels in `kernels/` are loaded **verbatim** by every worker target — `worker-browser` (WASM), `worker-native`, and the CI kernel tests. Since all three run the same `worker-core` host code, a kernel that works on one target and not another indicates either a real driver/vendor difference (worth investigating, see R6) or a bug in the platform seam.

---

## 1. The R5 gate — do this before writing any WGSL

Every new kernel requires a documented arithmetic-intensity calculation. **No exceptions.**

```
FLOP_per_output_byte = total_FLOP_per_task / output_spec.bytes

Requirement: FLOP_per_output_byte > 1e6
```

If it fails, you have two options and only two:

1. **Add on-node accumulation.** Keep refining locally, upload the running result on a fixed interval. This makes intensity a tuning knob — raise iterations until the ratio clears. This is the intended fix and is why `AccumulationSpec` exists.
2. **Reject the workload.** It is transfer-bound. Adding nodes will not help. Say so and move on.

Record the calculation in `kernels/manifest.toml` **and** as a `DECISIONS.md` entry. See `RESEARCH.md` §2 for the derivation.

### Worked example — path tracer tile

```
tile 64×64, 4×f32 per pixel      → output = 65,536 bytes
1,000 spp × ~1,000 FLOP/sample   → 4.1e9 FLOP
ratio = 4.1e9 / 6.5e4 ≈ 6.3e4    → FAILS (16× short)

With accumulation to 100,000 spp before upload:
ratio ≈ 6.3e6                    → PASSES
```

The output size never changed. Only the local iteration count did. That is the whole trick.

---

## 2. Manifest format — `kernels/manifest.toml`

```toml
[kernels.brute_search_v1]
file            = "brute_search.wgsl"
entry_point     = "main"
workgroup_size  = [256, 1, 1]
determinism     = "exact"
output_bytes    = 32
output_dtype    = "u32"
param_layout    = "BruteSearchParams"      # struct name in protocol crate
accumulates     = false
flop_per_unit   = 128                      # for R5 calc + sizer calibration
r5_ratio        = 4.0e8
r5_note         = "32-byte result per task; input is a 16-byte range spec. Trivially clears."

[kernels.pathtrace_tile_v1]
file            = "pathtrace_tile.wgsl"
entry_point     = "main"
workgroup_size  = [8, 8, 1]
determinism     = "statistical"
output_bytes    = 65536
output_dtype    = "f32"
param_layout    = "PathTraceParams"
accumulates     = true
min_iterations  = 100000
flop_per_unit   = 1000
r5_ratio        = 6.3e6
r5_note         = "Fails at 1k spp (6.3e4). Clears via accumulation to 100k spp before upload."
```

`determinism` maps to `DeterminismClass` in `PROTOCOL.md` and **decides how the validator compares replicas**. Getting this field wrong produces false Byzantine accusations against honest workers.

---

## 3. Hard authoring rules

### K1 — Chunkable, always (enforces R4)
A kernel must accept a `(start_unit, unit_count)` range in its params so the host can split one task into many dispatches of **≤250 ms expected work**. Never write a kernel whose smallest meaningful dispatch is long.

### K2 — Deterministic given `(seed, unit_range)`
Same seed + same range ⇒ same work performed, on any device. Use a counter-based RNG seeded from `(task_seed, global_invocation_id)` — **not** stateful/sequential RNG. PCG or Philox-style. This is what makes replication and speculative re-execution sound.

### K3 — Declare determinism honestly
- `exact` — integer-only math, no floats anywhere in the result path.
- `tolerant` — float math with bounded error. State `rel_eps` / `abs_eps` and justify them.
- `statistical` — Monte Carlo. Validator checks distributional consistency, not values.

When unsure, choose the weaker guarantee. A false `exact` claim manifests as honest workers being blacklisted, which is a miserable bug to trace.

### K4 — Respect queried limits, never defaults
Read `adapter.limits` at runtime and tile to fit. Never hardcode buffer sizes. Watch `max_storage_buffer_binding_size` (commonly ~128 MB), `max_buffer_size`, and `max_compute_workgroups_per_dimension` (65535 — loop or use 2D/3D dispatch grids past it).

### K5 — No fp64. Ever.
WebGPU has none. If an algorithm needs it, the algorithm is wrong for this project.

### K6 — Optional features are optional
`shader-f16` and `subgroups` can give large wins (Google measured 2.3–2.9× on matrix-vector shaders with subgroups) but **must** have an f32/non-subgroup fallback path. Select at runtime from reported `capabilities.features`. A worker lacking a feature must still be able to contribute.

### K7 — Workgroup size ≥ 64
Below that you waste occupancy on most hardware. 256 is a reasonable default for 1D; `8×8` for 2D tiles. Tune later with `timestamp-query`, not by guessing.

### K8 — No race conditions
The most common WebGPU compute bug. Any cross-invocation communication goes through `workgroupBarrier()` or atomics. If two invocations can write the same address without synchronization, the kernel is wrong even if it appears to work on your Mac.

---

## 4. Host-side contract

`worker-core` is responsible for:

1. Reading the manifest and selecting the feature-appropriate variant (K6).
2. Splitting `work_units` into ≤250 ms dispatches (R4/K1).
3. Yielding between dispatches so the tab stays responsive and TDR never fires.
4. Accumulating into a persistent GPU buffer when `accumulates = true`, uploading only on `upload_interval_ms`.
5. Handling device loss — release lease, re-acquire, re-register.
6. Populating every field of `TaskStats`.

**This is written once and compiled to both targets** (R2), so browser and native cannot drift. Only device acquisition and yielding differ, behind the platform seam in `src/worker-core/platform/`.

The sizing and chunking *policy* comes from the coordinator (R1) — the host applies it, never computes it.

### Params struct parity

The WGSL params struct and its C++ counterpart must match field for field, in order. Add a `static_assert` on `sizeof` and offsets in C++ (`CONVENTIONS.md` §5). Silent layout drift produces garbage results that look like a kernel bug and will cost you hours.

---

## 5. Testing a kernel

Every kernel needs, in `tests/kernels/` (run headless via `worker-native`, since CI has no browser):

1. **Golden test.** Fixed seed + range ⇒ known-good output, checked at the kernel's declared tolerance.
2. **Chunk-invariance test.** One dispatch of N units == four dispatches of N/4 units. Catches state leaks between chunks.
3. **Cross-implementation test.** Native vs. browser output for the same input, at declared tolerance — and across **vendors**, which is the case that actually matters. Manual/CI-gated is acceptable; it is the test that validates the `determinism` field. See Phase 4 step 4.6.
4. **Limits test.** Runs with artificially reduced limits to prove K4 tiling works.

Because both worker targets share `worker-core`, a same-vendor native-vs-browser mismatch means a platform-seam bug, not float divergence. Genuine R6 divergence only appears across *different* vendors.

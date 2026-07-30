# Phase 0 — Foundations & WebGPU Spike

**Objective:** get the C++ toolchain working on both targets, prove WebGPU compute runs, and get a real throughput number. Nothing distributed yet.

**Why first:** every sizing assumption downstream depends on actual device throughput. And the dual-target build (native + WASM) is the structural bet of this whole project — if it does not work on day one, that must be known before anything is built on it.

**Entry criteria:** none.

---

## Steps

### Toolchain

- [x] **0.1 — Prerequisites.**
  Install CMake ≥3.25, a C++20 compiler (Clang preferred — libFuzzer needs it), [vcpkg](https://github.com/microsoft/vcpkg), and the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html). Record exact versions in `docs/DECISIONS.md`.

- [x] **0.2 — Verify the scaffold builds.**
  `cmake --preset native-debug && cmake --build build/native-debug`. Fix whatever the scaffold got wrong — it was written without a toolchain available and has never been run. Pin exact dependency versions in `vcpkg.json` (no floating ranges).

- [x] **0.3 — Verify the WASM preset builds.**
  `cmake --preset wasm && cmake --build build/wasm`. Producing a `.wasm` + `.js` pair that loads in a browser is the gate on the entire dual-target architecture (R2). If Emscripten fights you here, that is important information — raise it (`WORKFLOW.md` §3).

- [ ] **0.4 — Prove the sanitizers actually work.**
  Plant a deliberate out-of-bounds read behind a `#if 0`, enable it, confirm ASan catches it, remove it. **Do not trust that a preset enables a sanitizer just because it says so** — a silently-not-instrumented build would invalidate the entire R11 story.

- [ ] **0.5 — Verify `flatc` runs as a build step.**
  Put one trivial table in `protocol/p2pgpu.fbs`, confirm headers generate into the build tree and are includable. Confirm they are **not** written into the source tree (R3).

### WebGPU — browser

- [ ] **0.6 — Minimal compute in the browser via WASM.**
  Request adapter → device → storage buffer → trivial WGSL compute (`out[i] = in[i] * 2.0`) → map and read back → **assert correctness**.
  Do not skip the readback assertion. A shader that appears to run but writes nothing is the most common early WebGPU bug.

- [ ] **0.7 — Log adapter capabilities.**
  Print adapter info (vendor, architecture, device, backend), full limits, and available features. Note whether `shader-f16`, `subgroups`, and `timestamp-query` are present.
  Save to `results/adapter-<device>.json` — the first row of the E7 table.

### WebGPU — native

- [ ] **0.8 — Same kernel via `wgpu-native`.**
  Headless, no surface. Same WGSL source file, loaded verbatim. Same readback assertion.
  When both targets run the same kernel correctly, the dual-target thesis is proven and everything after this is ordinary work.

- [ ] **0.9 — Compare native vs. browser output.**
  Identical input, same device. For an integer kernel expect bitwise equality. Any difference here, on the *same* GPU, is a bug in your host code, not R6 float divergence — R6 is about differing *vendors*.

### Measurement

- [ ] **0.10 — Throughput benchmark kernel.**
  Compute-bound with a known FLOP count (large-N fused multiply-add loop, or naive tiled matmul), parameterized by iteration count.
  This becomes the join-time calibration benchmark in Phase 2 — write it with that in mind.

- [ ] **0.11 — Measure GFLOP/s.**
  Use `timestamp-query` if available, wall clock otherwise (note which). Sweep problem sizes. Record achieved GFLOP/s and fraction of device theoretical peak.
  **Expected: 10–20% of peak.** If you measure ~2%, there is a kernel bug — likely workgroup size too small (K7) or a memory-bound inner loop.

- [ ] **0.12 — Measure per-dispatch overhead.**
  Time a trivial dispatch, many times. Expect ~24–71 µs. This sets the floor on useful task size and feeds the Phase 2 sizer.

- [ ] **0.13 — Verify in Safari.**
  Run 0.6–0.11 in Safari 26+. Record differences; add any to `RISKS.md`.

- [ ] **0.14 — Device-loss recovery, both targets.**
  Force a device loss, detect it, re-acquire a working device. Write the recovery helper now behind the platform seam — every worker needs it (R4), and the two targets surface loss differently.

- [ ] **0.15 — Chunking spike.**
  Split the 0.10 benchmark into N sequential dispatches with yields between them. Confirm total time is comparable to one big dispatch. Validates the K1/R4 chunking contract before anything is built on it.
  On WASM, "yield" means returning to the Emscripten main loop — verify the tab stays responsive.

### Retire the hardware risk early

- [ ] **0.16 — One session on the borrowed non-Apple machine.**
  Per `RISKS.md` R-D. Run in its browser: integer kernel bitwise-match vs. Mac (validates `Exact`), float kernel last-ULP divergence (**empirical R6 evidence, captured two phases before you need it**), and a deliberately triggered Windows TDR to confirm device-loss recovery.
  TDR cannot be tested on macOS. Getting this datapoint now is cheap; getting it at Phase 4 is a scheduling dependency on someone else's availability.

- [ ] **0.17 — Record findings.**
  `DECISIONS.md` entry with measured GFLOP/s, % of peak, per-dispatch overhead, toolchain versions, and any surprises. If measurements contradict `RESEARCH.md`, say so explicitly.

---

## Deliverables

- Working `native-debug`, `native-release`, and `wasm` presets
- Verified ASan/UBSan instrumentation
- `flatc` generating into the build tree
- Same WGSL kernel running correctly on both targets, outputs compared
- `results/adapter-<device>.json`, throughput and overhead measurements
- Device-loss recovery behind the platform seam
- Cross-vendor spike data from the borrowed machine

## Exit criteria

1. All three presets configure and build clean
2. ASan demonstrably catches a planted bug
3. Compute shader produces verified-correct output in Chrome, Safari, **and** native
4. Native and browser agree bitwise on the integer kernel
5. GFLOP/s, % of peak, and per-dispatch overhead recorded
6. Chunked execution equivalent to monolithic, tab stays responsive
7. Device loss detected and recovered on both targets

---

## → HUMAN GATE G0

Produce for review:
- Measured GFLOP/s, % of theoretical peak, per-dispatch overhead
- Adapter capability dumps (Mac + borrowed machine)
- Proof the same kernel runs on native and WASM with matching output
- Cross-vendor float divergence observed in 0.16 (the R6 evidence)

**The questions being answered:** is measured throughput in the expected range? And does the dual-target build actually work, before we bet the architecture on it?

If throughput is >3× off from expectation, sizing assumptions downstream are invalid. If the WASM target is fighting, that is a structural problem worth discussing before Phase 1.

**Stop here. Do not begin Phase 1 without written approval.**

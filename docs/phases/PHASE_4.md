# Phase 4 — Cross-Vendor Validation & Hardening

**Objective:** prove the determinism classification holds across real GPU vendors, and prove the trust boundary is hardened rather than merely intended.

**Why this phase changed (D-0012):** the original Phase 4 was "build a native worker." Under the C++ pivot, `worker-native` and `worker-browser` are both thin wrappers over one `worker-core`, so that work happened in Phase 1. What remains — and what actually matters — is validation on hardware you do not own and hardening the input surface.

**Entry criteria:** G3 approved.

> **`RISKS.md` R-D applies here.** The borrowed non-Apple machine is needed hands-on. If you did step 0.16, you already have preliminary cross-vendor data and this phase is confirmation rather than discovery. If you skipped it, do it now before anything else in this phase.

---

## Steps

### Kernel testing in CI

- [ ] **4.1 — Golden tests.**
  Fixed seed + range ⇒ known-good output at declared tolerance, headless via `worker-native` (`KERNELS.md` §5).

- [ ] **4.2 — Chunk-invariance tests.**
  One dispatch of N units == four dispatches of N/4. Catches state leaks between chunks — a real risk once accumulation arrives in Phase 5.

- [ ] **4.3 — Limits tests.**
  Run with artificially reduced limits to prove K4 tiling works. Catches bugs that would otherwise only appear on a stranger's low-end GPU.

- [ ] **4.4 — CI wiring.**
  Kernel tests on every commit. Software adapter (lavapipe/WARP) where CI has no GPU; log which adapter was used.
  **Software adapters are CI-correctness only — never counted toward vendor or determinism claims** (`RISKS.md` R-D, `EVALUATION.md` honesty rule).

### Cross-vendor

- [ ] **4.5 — Full run on non-Apple hardware.**
  Both worker targets, both kernels. Record the adapter dump into `results/`.

- [ ] **4.6 — Cross-implementation, cross-vendor comparison.**
  Same task, same seed: Mac browser, Mac native, Windows browser, Windows native. Compare at declared tolerance.
  **This validates the `determinism` manifest field.** If `Exact` results differ across vendors, the classification is wrong and must be corrected (K3).

- [ ] **4.7 — Tighten epsilons from data.**
  Replace the Phase 3 starting epsilons with values derived from measured cross-vendor divergence. Log the before/after in `DECISIONS.md` per DL7.

- [ ] **4.8 — Feature-fallback verification.**
  Kernel runs correctly with and without `shader-f16` and `subgroups` (K6). Measure the speedup where available — good E7 material.

- [ ] **4.9 — Transfer-cost reality check.**
  Compare `transfer_ms` on Apple unified memory vs. the discrete GPU. Expect a meaningful difference (`RISKS.md` §1). If sizing was tuned only on the Mac, revisit and log a `DECISIONS.md` entry.

- [ ] **4.10 — Real Windows TDR.**
  Confirm the R4 chunking ceiling actually prevents TDR under sustained load, and that device-loss recovery works when it does fire. Untestable on macOS.

- [ ] **4.11 — E7 first real rows.**
  Achieved GFLOP/s vs. theoretical peak per device/vendor/backend.

### Hardening

- [ ] **4.12 — Extended fuzzing campaign.**
  Hours, not minutes, on `fuzz_protocol`. Report exec count and coverage. Commit any new corpus entries.

- [ ] **4.13 — Second fuzz target.**
  `fuzz/fuzz_asset.cpp` on asset chunk reassembly — the integer-overflow site flagged in `PROTOCOL.md` §4 invariant 10. Needed before Phase 6 makes that path live.

- [ ] **4.14 — TSan pass.**
  Coordinator test suite under TSan. Fix every finding; do not suppress without a `DECISIONS.md` entry.

- [ ] **4.15 — Sustained hostile soak.**
  `malformed_frames` profile at high rate for an extended run alongside honest workers. Coordinator stays up, honest throughput degrades gracefully, rate limiting engages.

- [ ] **4.16 — Audit the boundary for R11 compliance.**
  Grep `src/protocol/` and transport code for `memcpy`, `reinterpret_cast`, raw pointer arithmetic, and C arrays. Every hit is either removed or gets an inline justification. Record the audit result.

### Mixed fleet

- [ ] **4.17 — Simultaneous three-way run.**
  Browser, native, and mock workers on one job at the same time, across two vendors. Coordinator treats them uniformly.

- [ ] **4.18 — Cloud load-test fleet.**
  Script to launch `worker-native` on cloud VMs (GPU where affordable, CPU/software adapter otherwise) for `MIXED`-labeled scaling data.

- [ ] **4.19 — One-command join package.**
  Per `RISKS.md` R-D: package `worker-native` so the borrowed machine's owner can run *one command* to join over the network, headless. This makes the Phase 7 hardware favor cheap instead of requiring another setup visit.

---

## Deliverables

- Kernel golden / chunk-invariance / limits tests in CI
- Cross-vendor comparison validating determinism classes; epsilons tightened from data
- Real Windows TDR confirmation
- E7 table with ≥2 vendors
- Extended fuzzing, second fuzz target, TSan clean, hostile soak, R11 audit
- Mixed three-way run
- One-command remote join package

## Exit criteria

1. Same job completes with all three worker types across ≥2 vendors simultaneously
2. Kernel tests run headless in CI on every commit
3. Cross-implementation outputs agree at declared tolerance across vendors; epsilons justified by measurement
4. `shader-f16` / `subgroups` fallback paths verified
5. TDR confirmed prevented under load on Windows; recovery works when forced
6. Extended fuzz campaign clean; asset fuzz target exists; TSan clean
7. Coordinator survives sustained hostile soak
8. R11 boundary audit complete with no unjustified findings
9. E7 table has ≥2 vendors

---

## → HUMAN GATE G4

Produce for review: the three-way cross-vendor run; CI test output; cross-vendor comparison with the epsilon justification; fuzzing and TSan reports; the R11 audit; E7 table so far.

**The questions being answered:** does the determinism classification hold on real hardware from different vendors? And is the trust boundary demonstrably hardened, not just intended?

This gate is where R6 and R11 stop being design claims and become measured properties. Both are load-bearing for the project's credibility.

**Stop here.**

# Roadmap

Eight phases, each ending in a **HUMAN GATE**. Detailed steps live in `docs/phases/PHASE_<n>.md`.

**Rule R10: do not begin phase N+1 until gate G<N> is approved in writing.** When a gate is reached, produce the required evidence, record it in the gate block below, and stop.

---

## Phase order and rationale

The order is dependency-driven, not importance-driven:

```
0  Foundations & WebGPU spike        ── toolchain + dual-target build + a real number
1  Protocol & single-worker pipeline ── schema, coordinator, BOTH worker targets, fuzzing
2  Scheduling, leases, mock harness  ── the harness unlocks all later measurement
3  Trust & validation                ── needs the mock harness to test Byzantine cases
4  Cross-vendor validation & hardening ── real vendors, CI, fuzz campaign, sanitizers
5  Workload A: path tracer           ── biggest kernel effort; needs accumulation working
6  P2P data plane                    ── needs a large asset (the BVH) to justify existing
7  Evaluation, deploy, writeup       ── the actual deliverable
```

Workload B comes first because it is `Exact`, tiny, and lets the whole pipeline be debugged without the path tracer's complexity confounding it. The mock harness comes early because nearly every chart in `EVALUATION.md` depends on it.

**Phase 1 ships both worker targets.** Because `worker-native` and `worker-browser` are thin wrappers over one `worker-core`, the second falls out of the first at near-zero cost — which also makes headless kernel testing in CI available two phases earlier than originally planned. Phase 4 was repurposed accordingly; see D-0012.

---

## Status board

| Phase | Name | Status | Gate | Approved |
|---|---|---|---|---|
| 0 | Foundations & WebGPU spike | ⬜ not started | G0 | — |
| 1 | Protocol & single-worker pipeline | ⬜ not started | G1 | — |
| 2 | Scheduling, leases & mock harness | ⬜ not started | G2 | — |
| 3 | Trust & validation | ⬜ not started | G3 | — |
| 4 | Cross-vendor validation & hardening | ⬜ not started | G4 | — |
| 5 | Workload A — path tracer | ⬜ not started | G5 | — |
| 6 | P2P data plane | ⬜ not started | G6 | — |
| 7 | Evaluation, deploy & writeup | ⬜ not started | G7 | — |

Status values: ⬜ not started · 🟡 in progress · 🟢 complete, gate approved · 🔴 blocked

---

## Gates

Each gate requires: **all exit criteria met**, **evidence produced**, **tree green**, and **written human approval** recorded in the table above.

---

### HUMAN GATE G0 — The toolchain and WebGPU are real on this machine

**Evidence required**
- Measured GFLOP/s for a compute kernel on the dev Mac, and what fraction of device peak that represents
- Per-dispatch overhead measurement
- The same WGSL kernel running correctly on **both** the native and WASM targets, with outputs compared
- Log of the kernel running in Chrome and Safari
- Proof that ASan catches a deliberately planted out-of-bounds read
- Cross-vendor float divergence observed on the borrowed machine (step 0.16)

**Question the human is answering:** *is measured throughput in the expected range, and does the dual-target build actually work before we bet the architecture on it?*

Throughput >3× off from expectation invalidates sizing assumptions downstream. A WASM target that fights is a structural problem, not a detail.

---

### HUMAN GATE G1 — One worker completes a real job, on both targets

**Evidence required**
- Recorded runs: coordinator starts, worker joins, N tasks complete, results verified against the CPU reference — once with `worker-native`, once with `worker-browser`, once mixed
- `PROTOCOL.md` diffed against `protocol/p2pgpu.fbs` to show agreement
- Fuzzing report: exec count, coverage, corpus size, crashes found and fixed
- The R5 calculation for `brute_search_v1` in the manifest and `DECISIONS.md`
- Proof that no `#ifdef __EMSCRIPTEN__` leaked outside `src/worker-core/platform/`

**Question:** *is the protocol shape right before everything is built on it?*

This is the most important gate. Protocol changes after Phase 4 are expensive. Scrutinize whether the Verifier path is genuinely the only route from bytes to fields (R11).

---

### HUMAN GATE G2 — Scheduling holds under chaos

**Evidence required**
- Scaling run: throughput vs. worker count for 1 → 50 mock workers
- Fault injection: kill 30% of the fleet mid-job → job completes, **zero tasks lost**, recovery time measured
- Straggler experiment: job completion time with vs. without speculative re-execution on a deliberately heterogeneous mock fleet
- Adaptive sizing demonstrably converging (plot predicted vs. actual task duration over time)
- Dashboard showing live fleet state

**Question:** *does the scheduler actually work, and is the mock harness good enough to generate the rest of the project's evidence?*

---

### HUMAN GATE G3 — Byzantine workers are caught

**Evidence required**
- Detection rate vs. replication overhead, for injected liar fractions of 5%, 10%, 20%, 40%
- Reputation-weighted adaptive replication compared against naive 2× replication on the same runs
- Demonstration that `Tolerant` comparison does **not** flag honest workers with differing float results (R6)
- False-positive rate measured and reported

**Question:** *is the verification scheme sound, and does adaptive replication actually beat the naive baseline?*

---

### HUMAN GATE G4 — Cross-vendor determinism holds, and the boundary is hardened

**Evidence required**
- The same job completing with browser, native, and mock workers **simultaneously across ≥2 vendors**
- Kernel golden / chunk-invariance / limits tests running headless in CI
- Cross-implementation, cross-vendor output comparison, with tolerance epsilons justified by measured divergence rather than guesswork
- Real Windows TDR confirmed prevented under load; recovery works when forced
- Extended fuzz campaign report; second fuzz target on asset reassembly; TSan clean
- Sustained hostile-soak result (`malformed_frames` at high rate)
- The R11 boundary audit

**Question:** *does the determinism classification hold on real hardware from different vendors, and is the trust boundary demonstrably hardened rather than merely intended?*

This gate is where R6 and R11 stop being design claims and become measured properties.

---

### HUMAN GATE G5 — Distributed rendering converges

**Evidence required**
- Image visibly refining as workers join; final image matching a single-machine reference within stated tolerance
- The R5 calculation for `pathtrace_tile_v1` showing accumulation clears the 10⁶ gate
- Measured node utilization ≥85% (proving the accumulation design works in practice, not just on paper)
- Demo GIF/video

**Question:** *does on-node accumulation deliver the utilization the arithmetic predicted?*

This gate tests the project's central thesis empirically. If utilization is low, the thesis needs revisiting before anything else.

---

### HUMAN GATE G6 — P2P distribution reduces egress

**Evidence required**
- Coordinator egress vs. worker count, with and without the P2P data plane (should be flat vs. linear)
- Peer-fetch success rate, and measured fallback rate to the coordinator
- Correctness proof: full job completes with the data plane **entirely disabled**
- NAT/TURN behavior documented — how many peers needed relay

**Question:** *is the P2P claim backed by a real measurement, or is it decoration?*

---

### HUMAN GATE G7 — Ship it

**Evidence required**
- All seven measurements in `EVALUATION.md` produced as reproducible charts
- README written, with the honest framing from `PROJECT_OVERVIEW.md` §5 and explicit labeling of real-hardware vs. simulated results
- The security section (Phase 7 step 7.15): threat model, R11 posture, fuzzing evidence, sanitizer CI, hostile soak, malicious-peer result
- Deployed demo reachable, with COOP/COEP headers set and opt-in UI working (R7)
- A run across ≥3 physically distinct GPUs from ≥2 vendors
- `DECISIONS.md` complete and coherent as a narrative, including the C++ pivot

**Question:** *does this stand up to a skeptical technical reader?*

One of the first three questions such a reader will ask is the memory-safety question. Confirm 7.15 answers it with evidence, not assertion.

---

## Stretch goals (post-G7, explicitly optional)

Do not start any of these before G7 is approved.

- **S2 — Third workload** demonstrating a different determinism class.
- **S3 — WebTransport control plane** alongside WebSocket, as a comparison measurement.
- **S4 — Collusion-resistant replica assignment** (SERENE-style randomized placement) with a measured comparison against the Phase 3 scheme.
- **S5 — Parser isolation.** Run deserialization in a sandboxed subprocess (seccomp), with measured overhead vs. the in-process design. The strongest possible answer to the memory-safety question, deliberately deferred as disproportionate for the core project (D-0010).

*(S1 in the original roadmap was "port the browser worker to Rust." It no longer exists — under D-0008 the browser worker is already the same C++ as everything else.)*

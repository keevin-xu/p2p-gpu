# Project Overview

**Project:** p2pgpu — a WebGPU volunteer-compute grid
**Owner:** Kevin
**Started:** 2026-07-29
**Purpose:** Portfolio / resume project demonstrating distributed-systems depth

---

## 1. One-paragraph description

p2pgpu lets anyone contribute idle GPU cycles by opening a web page. A C++ coordinator decomposes a job into tasks, sizes them adaptively per device, and leases them to workers running WGSL compute kernels via WebGPU. Workers are assumed to be heterogeneous, unreliable, and untrusted: the coordinator handles stragglers with speculative re-execution, node loss with lease expiry, and incorrect results with reputation-weighted replication. Large shared assets (e.g. a scene BVH) are distributed peer-to-peer over WebRTC data channels so coordinator egress does not scale with fleet size.

The whole system is C++20. One `worker-core` library compiles to both a native binary and a WebAssembly module, so the browser worker and the native worker are the same code — see D-0008.

---

## 2. Why this design and not the obvious one

The obvious pitch — "a decentralized GPU cloud, rent idle GPUs from anyone" — does not survive contact with arithmetic. **Read `RESEARCH.md` §2 before doing anything else.** The summary:

```
required arithmetic intensity = compute rate / network rate
                              ≈ 1e12 FLOP/s / 1e6 B/s
                              ≈ 10^6 FLOP per byte transferred
```

Below that ratio, nodes sit idle waiting on the network and adding workers accomplishes nothing. This eliminates distributed matmul (~10²), distributed NN training (~10³), and naive per-frame distributed rendering (~10⁴).

**The design response:** every workload must either be naturally above 10⁶ FLOP/byte (brute-force search, Monte Carlo) or be restructured to get there via **on-node accumulation** — the worker keeps refining a local result and uploads only the running average on a fixed cadence. This turns arithmetic intensity into a tuning knob rather than a fixed property of the workload.

This single insight is the intellectual core of the project and should be prominent in the README.

---

## 3. Scope

### In scope
- Coordinator: job/task queue, lease management, adaptive task sizing, speculative re-execution, replication + reputation-based validation, WebRTC signaling, metrics.
- Three worker targets against one protocol: browser (WASM), native headless, and a mock/chaos harness with no GPU. The first two share `worker-core` verbatim.
- A demonstrated memory-safety posture at the trust boundary: schema-driven deserialization, sanitizer CI, and a committed fuzz corpus (R11, D-0010).
- Two workloads proving the task abstraction is generic:
  - **Workload A — distributed path tracer.** Tile decomposition, on-node sample accumulation, coordinator-side compositing. Visually demonstrable; drives the P2P asset-distribution requirement.
  - **Workload B — brute-force search.** Integer-exact, so bitwise verification is valid. Stresses the scheduler with many tiny tasks.
- P2P data plane over WebRTC for bulk assets.
- Evaluation harness and the measurements in `EVALUATION.md`.

### Explicitly out of scope
- Payments, tokens, blockchain, any market mechanism.
- Distributed neural network training. It does not clear the gate (R5) and pretending otherwise is the fastest way to lose a technically strong reader.
- Competing with datacenter GPUs on efficiency or cost.
- Mobile-first optimization (Android Chrome should *work*; it need not be fast).
- Multi-tenancy, auth beyond a session token, production-grade security hardening.
- Anything requiring fp64 (WebGPU has none).

---

## 4. Success criteria

The project is done when all of the following are true:

1. A job runs to completion across ≥3 physically distinct GPUs from ≥2 vendors.
2. All three worker targets run the same job against the same WGSL kernels.
3. Killing 30% of the fleet mid-job loses zero work and the job still completes.
4. Injecting Byzantine workers that return garbage produces a measurable detection rate, and the reputation-weighted scheme beats naive 2× replication on the overhead/detection tradeoff.
5. Coordinator egress is measurably flat vs. worker count with P2P asset distribution enabled, and linear without it.
6. All seven measurements in `EVALUATION.md` exist as reproducible charts.
7. The README states honestly what was measured on real hardware vs. simulated.
8. The trust boundary is hardened with evidence, not assertion: fuzz corpus, exec counts, sanitizer CI, and a sustained hostile-traffic soak the coordinator survives.

---

## 5. Non-negotiable framing (for README and interviews)

**Say:**
> A volunteer-compute grid running GPU workloads across browser tabs via WebGPU. The interesting problems are scheduling across heterogeneous unreliable nodes, verifying results from untrusted workers, and staying compute-bound on ~1 MB/s uplinks — which constrains you to workloads above ~10⁶ FLOP/byte, so the task format is designed around on-node accumulation.

**Never say:** "decentralized AI training", "an alternative to AWS", "blockchain", or anything implying this beats a datacenter.

**Be honest about the two independent taxes:**
- WebGPU vs. native CUDA: **~6–8×**. This is the browser sandbox, not the distribution.
- Distribution overhead: **~5–20%** for embarrassingly parallel work.

Break-even against one native CUDA machine of the same class is roughly **8 volunteer nodes**. Past that it scales near-linearly. The value proposition is *access to capacity you do not own*, not efficiency.

---

## 6. Known hard problems (where the depth lives)

Ordered by how much signal solving them sends. Detail in `RESEARCH.md` §5.

1. **Float nondeterminism breaks naive verification.** Two honest GPUs disagree in the last ULPs. Verification must be determinism-class-aware. Most implementations miss this.
2. **Verifying untrusted compute.** Replication → spot-checking → credibility/reputation → collusion resistance. BOINC's adaptive replication pushes overhead from 2× toward 1×.
3. **Stragglers and the GPU watchdog.** Adaptive sizing, the 250 ms dispatch ceiling (R4), speculative re-execution, lease expiry.
4. **Browser hostility.** Background tab throttling, buffer limits, driver blocklists, cross-origin isolation, device heterogeneity.
5. **Scheduling and cache affinity.** Pull-based work stealing; keep sending a node tasks that reuse assets it already cached, because transfer is the expensive thing.
6. **A hostile input surface in a memory-unsafe language.** Anyone can connect as a worker, so every byte reaching the parser is attacker-controlled. Addressed by R11 and D-0010 — schema-driven deserialization behind a Verifier, no raw pointer arithmetic at the boundary, sanitizers and fuzzing in CI. In Rust this property is free; here it is earned and therefore demonstrable.

---

## 7. Platform notes

Development is on **macOS (Apple Silicon)**. This is fine and in some ways ideal — WebGPU is vendor-agnostic by construction (Metal on Mac, Vulkan on Linux/Android, D3D12 on Windows), and Chrome + Safari 26+ both ship it, so cross-browser testing works on one machine. CUDA is never involved.

Three caveats to keep in mind while developing:

- **Unified memory hides a bottleneck.** Apple Silicon has no PCIe transfer stall on buffer upload/readback. Discrete GPUs do. Do not design assuming readback is free; validate on a non-Apple GPU before claiming cross-vendor results.
- **Absolute throughput will be lower** than a discrete NVIDIA card (M-series ≈ 3.6–10 TFLOP/s peak). Irrelevant — the deliverable is a scaling curve, not a peak-FLOPS record.
- **Metal has higher per-dispatch overhead** (~32–71 µs vs ~24–36 µs on Vulkan), and macOS lacks Windows' harsh TDR. A kernel that feels fine locally can kill a Windows node's driver. Design to R4's 250 ms ceiling regardless of what your machine tolerates.

Toolchain note: development needs CMake ≥3.25, a C++20 compiler (Clang preferred — libFuzzer requires it), vcpkg, and the Emscripten SDK. Apple Clang works but ships its own libFuzzer quirks; if fuzzing misbehaves, try Homebrew LLVM before assuming a code bug.

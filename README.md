# p2pgpu

> **Status: scaffolding.** Phase 0 not started. This README is a placeholder — it gets written properly in Phase 7 (step 7.13), once there are measurements to put in it.

A volunteer-compute grid that runs GPU workloads across browser tabs via **WebGPU**. A Rust coordinator schedules work across heterogeneous, unreliable, untrusted worker nodes; workers run WGSL compute kernels and return results. Bulk assets are distributed peer-to-peer over WebRTC so coordinator egress stays flat as the fleet grows.

---

## The constraint that shapes everything

A WebGPU-capable node delivers roughly 1 TFLOP/s against roughly 1 MB/s of consumer upload bandwidth. To stay compute-bound rather than idle:

```
required arithmetic intensity = compute rate / network rate
                              ≈ 1e12 FLOP/s / 1e6 B/s
                              ≈ 10^6 FLOP per byte transferred
```

Distributed matmul is ~10². Distributed neural network training is ~10³. Naive per-frame distributed rendering is ~10⁴. **All of them fail this by orders of magnitude**, which is why this project does not attempt them.

Workloads here either clear 10⁶ naturally (brute-force search, Monte Carlo) or are restructured to clear it via **on-node accumulation** — workers refine a result locally and upload only the running average on a fixed cadence, making arithmetic intensity a tuning knob rather than a fixed property of the workload.

Full derivation and prior art: [`RESEARCH.md`](RESEARCH.md).

---

## Documentation

Start with [`CLAUDE.md`](CLAUDE.md) — the hard rules and repository map.

| Doc | What it covers |
|---|---|
| [`RESEARCH.md`](RESEARCH.md) | Background, prior art, the physics |
| [`docs/PROJECT_OVERVIEW.md`](docs/PROJECT_OVERVIEW.md) | Scope, non-goals, success criteria |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Modules, boundaries, state machines |
| [`docs/PROTOCOL.md`](docs/PROTOCOL.md) | The wire contract |
| [`docs/KERNELS.md`](docs/KERNELS.md) | WGSL authoring rules, the R5 gate |
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | Eight phases, eight human gates |
| [`docs/EVALUATION.md`](docs/EVALUATION.md) | The measurements that are the deliverable |
| [`docs/DECISIONS.md`](docs/DECISIONS.md) | Why the system is the way it is |
| [`docs/RISKS.md`](docs/RISKS.md) | Known traps — check here when things behave strangely |

---

## Stack

Rust everywhere except the browser tab. Coordinator, native worker, and mock harness in Rust (`axum`, `tokio`, `wgpu`); browser worker and dashboard in TypeScript; kernels in WGSL, shared verbatim across implementations. Types cross the language boundary via `ts-rs` codegen — logic never does.

Rationale: [`docs/DECISIONS.md`](docs/DECISIONS.md) D-0004.

---

## What this is not

Not decentralized AI training. Not an alternative to AWS. No blockchain, no tokens, no market mechanism.

Two independent taxes are paid and will be reported honestly: WebGPU vs. native CUDA costs ~6–8×, and distribution overhead costs ~5–20%. Break-even against a single native CUDA machine of the same class is roughly 8 volunteer nodes. The value proposition is access to capacity you do not own, not efficiency.

---

## License

MIT

# p2pgpu

> to be updated

A volunteer-compute grid that runs GPU workloads across browser tabs via **WebGPU**. A C++ coordinator schedules work across heterogeneous, unreliable, untrusted worker nodes; workers run WGSL compute kernels and return results. Bulk assets are distributed peer-to-peer over WebRTC so coordinator egress stays flat as the fleet grows.

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

## Stack

**C++20 everywhere**, including the browser. One `worker-core` library compiles to both a native binary and a WebAssembly module from identical source, so `worker-native` and `worker-browser` are thin `main()` wrappers over the same code running the same WGSL kernels.

| | |
|---|---|
| Build | CMake + CMakePresets, vcpkg manifest |
| Wire format | FlatBuffers — schema-driven, `Verifier` on all untrusted input |
| Coordinator | uWebSockets, SQLite |
| Worker transport | libdatachannel / datachannel-wasm — same API on both targets |
| WebGPU | wgpu-native (`webgpu.h`) natively, Emscripten bindings in the browser |
| Kernels | WGSL, shared verbatim |

The single-language architecture is possible because libdatachannel exposes one WebRTC API to both native and WASM — the gap that usually forces a JavaScript or TypeScript layer in the browser. Rationale: [`docs/DECISIONS.md`](docs/DECISIONS.md) D-0008.

## Security posture

Anyone can connect as a worker, so every byte reaching the parser is attacker-controlled. In a memory-unsafe language that is a real cost, and it is treated as an engineered, *demonstrated* property rather than an assumed one: schema-driven deserialization behind a verifier, no raw pointer arithmetic at the boundary, every length validated before allocation, ASan/UBSan and libFuzzer in CI with a committed corpus.

---

## What this is not

Not decentralized AI training. Not an alternative to AWS. No blockchain, no tokens, no market mechanism.

Two independent taxes are paid and will be reported honestly: WebGPU vs. native CUDA costs ~6–8×, and distribution overhead costs ~5–20%. Break-even against a single native CUDA machine of the same class is roughly 8 volunteer nodes. The value proposition is access to capacity you do not own, not efficiency.

---

## License

MIT

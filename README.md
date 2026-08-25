# p2pgpu

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

Every kernel's intensity is computed from its source before it is admitted. The path tracer's reduces to `samples × ops-per-sample > 1.6e7`, in which the tile size *cancels* — so "use bigger tiles to amortise the upload" is false here, and the only real levers are sample count and output size.

---

## Architecture

```
                    ┌──────────────────────────────┐
   browser tab ───► │                              │
   (WebGPU)         │        coordinator           │
                    │  queue · leases · sizing     │
   native worker ─► │  validation · reputation     │
   (wgpu-native)    │  signalling relay            │
                    └──────────────────────────────┘
          │                      │
          │  WebSocket           │  content-addressed
          │  control plane       │  asset fallback
          │                      ▼
          └────── WebRTC ──── peers serve each other
                  data plane    the same bytes
```

**One `worker-core` library compiles to both targets from identical source.** `worker-native` and `worker-browser` are thin `main()` wrappers over the same code, running the same WGSL. Platform differences live in two small files selected at build time; a compile-time check fails the build if a platform conditional appears anywhere else.

That property is only affordable because one WebRTC API covers both targets, so the peer-to-peer data plane is also written once.

**Workers make no decisions.** They execute tasks and report facts. Scheduling, task sizing, validation, reputation and retry all live in the coordinator — which is what keeps the dual-target library small enough to be confidently correct in two very different environments.

**Tasks are leased, not assigned.** A worker vanishing mid-task is the normal case; unrenewed leases return to the queue. No code path assumes a worker comes back.

---

## Measurements

Numbers below are from committed CSVs in `results/`, regenerable with `tools/reproduce.sh`.

### Peer-to-peer distribution keeps coordinator egress flat

Bulk assets are content-addressed and served by whichever peers already hold them, with the coordinator as a mandatory fallback.

| workers | egress, P2P off | egress, P2P on | saving |
|---|---|---|---|
| 1 | 257 KB | 257 KB | 1.0× |
| 2 | 513 KB | **257 KB** | 2.0× |
| 4 | 1027 KB | **257 KB** | 4.0× |
| 6 | 1540 KB | **257 KB** | 6.0× |
| 8 | 2054 KB | **257 KB** | 8.0× |
| 10 | 2567 KB | **257 KB** | 10.0× |

**Exactly one copy leaves the coordinator regardless of fleet size**, across 36 runs with no variance. Measured as bytes counted where they leave the coordinator — not from worker self-reports, which a fleet could fabricate.

The cost is latency: a worker joining an 8-node fleet waits ~700 ms longer to start useful work, because it fetches from a peer rather than a server with spare capacity.

### On-node accumulation keeps the GPU busy

| samples/pixel | accumulation | GPU time | transfer |
|---|---|---|---|
| 512 | **on** | **98.4%** | 1.6% |
| 512 | off | 88.4% | 11.6% |
| 8192 | **on** | **99.7%** | 0.3% |
| 8192 | off | 98.7% | 1.3% |

Switching accumulation off multiplies transfer time 4–6× at identical compute. Note the control still exceeds 90% here — this machine has unified memory and pays no bus crossing, so it shows accumulation *delivers* utilization without proving it *necessary*.

### Untrusted results are caught

Replicating every task doubles the work. Replicating adaptively — heavily for newcomers, rarely for workers with a record — costs **11% overhead** and still caught **100%** of independently lying workers, against 101% overhead for blanket duplication.

Colluding workers that agree on the same wrong answer are a different matter: detection falls to **84% at 20% liars and 70% at 40%**.

### Cross-vendor determinism

Integer kernel results are **bitwise identical** across Apple and NVIDIA GPUs (1000/1000), which is what makes exact comparison sound rather than merely convenient. Float results diverge by at most **5 units in the last place**, so float kernels are compared with a tolerance derived from that measurement instead of a guess.

### Achieved throughput

| device | backend | achieved |
|---|---|---|
| Apple M4 Pro | Metal, native | 1875 GFLOP/s |
| NVIDIA GTX 1650 Super | D3D12, browser | 2260 GFLOP/s |

The per-submission cost is the more interesting number: ~110 µs natively against ~4.8 ms in the browser, which is why a browser worker is submission-bound and why task sizing adapts per worker rather than using a fixed value.

---

## Security posture

Anyone can connect as a worker, so **every byte reaching the parser is attacker-controlled**. In a memory-unsafe language that is a real cost, and it is treated as an engineered and demonstrated property rather than an assumed one.

- **All deserialization goes through a schema verifier** before any field is read. Nothing is hand-parsed, and a malformed buffer is dropped before it reaches program logic.
- **No raw pointer arithmetic, `memcpy`, or C arrays** in code that touches network bytes.
- **Every length is validated against a maximum before allocating.** A verifier prevents memory corruption; it does not stop an attacker declaring a 4 GB payload.
- **Sanitizer and fuzz builds run in CI, with a committed corpus.** Over 550 million executions against the protocol parser and 1.07 billion against the asset path, zero crashes.

The one asset that *cannot* go through the verifier is the scene structure — a GPU shader cannot traverse a graph of relative offsets, so it needs a flat layout and a hand-written validator. Fuzzing that validator found two defects in code that was already finished, reviewed, and covered by thirteen hostile unit tests, within three minutes:

- an acyclic graph is not necessarily a *tree*, and a shared child means exponential traversal on the GPU — with nothing for a sanitizer to observe
- `left + 1 >= count` wraps at `0xFFFFFFFF`, so a range check read backwards

A sustained hostile-input soak found a third: a use-after-free that only fires when a single connection reaches 64 malformed frames, which no unit test and no short run produces.

A worker that serves corrupt bytes to a peer is detected by content hash, and the victim falls back to the coordinator rather than losing its task — costing one wasted transfer instead of a task.

---

## Honest limitations

**Most distributed measurements are from one machine.** The peer-to-peer numbers above are many processes on one host over loopback, up to ten workers. A real network has been exercised once — a deployed coordinator rendering with a worker on a laptop — which worked, but that run had a single worker and therefore no peer-to-peer transfer.

**Peer-to-peer between two separate machines has never succeeded.** The one attempt failed because no STUN/TURN servers were configured, so only private addresses were offered. This is a configuration gap, not a demonstrated capability.

**NAT traversal is unmeasured.** No relay server has ever been configured, so the fraction of connections that would need one is unknown. Published figures suggest 10–20%; this project has no number of its own.

**The scaling curve is simulated.** Throughput versus worker count uses mock workers that compute real answers but simulate device speed. It measures scheduling, not GPU throughput, and is labelled that way in the chart title. Real-hardware scaling is measured only to a handful of nodes.

**Two GPU vendors, and no percentage of theoretical peak.** Achieved throughput is measured; the fraction of each device's rated peak is not, because that requires published specifications rather than measurements.

**Windows driver-reset behaviour is unverified.** Long GPU work is chunked to stay far below the watchdog threshold, and deliberately violating that limit does trigger a reset. What has never been confirmed is that *obeying* it prevents one under sustained load. Device-loss recovery is exercised on macOS; the Windows environmental behaviour is not.

**Colluding workers evade detection**, as above. So does a coordinated fleet biasing every result by ~1% — measured, the detection floor is reliably 5%, partially 2%, and never 1%.

**One coordinator.** Leases, the queue and reputation live in a single process. It is a single point of failure and the ceiling on fleet size.

---

## Running it

```bash
cmake --preset native-release
cmake --build build/native-release -j

# coordinator with a render queued
./build/native-release/coordinator --seed-render scenes/default.scene \
    --render-size 384x288 --render-spp 65536

# a worker, in another terminal
./build/native-release/worker-native --coordinator ws://localhost:8080/ws
```

The browser worker needs a secure context — WebGPU does not exist without one — and cross-origin isolation headers:

```bash
cmake --preset wasm && cmake --build build/wasm -j
python3 tools/serve.py          # sets the required headers
```

Reproduce the measurements, or regenerate the charts from committed data:

```bash
tools/reproduce.sh charts
tools/reproduce.sh experiments   # ~30 min, overwrites results/
```

---

## Stack

**C++20 everywhere**, including the browser.

| | |
|---|---|
| Build | CMake + presets, vcpkg manifest with a pinned baseline |
| Wire format | FlatBuffers — schema-driven, verified on all untrusted input |
| Coordinator | uWebSockets, SQLite |
| Worker transport | libdatachannel / datachannel-wasm — same API on both targets |
| WebGPU | wgpu-native natively, Emscripten bindings in the browser |
| Kernels | WGSL, shared verbatim by every worker |
| Testing | Catch2, libFuzzer, ASan/UBSan/TSan |

---

## What this is not

Not decentralized AI training. Not an alternative to AWS. No blockchain, no tokens, no market mechanism.

Two independent taxes are paid. WebGPU versus native CUDA costs roughly **6–8×** — a figure from published comparisons, not measured here, since that needs the same kernel written twice. Distribution overhead costs roughly **5–20%** for embarrassingly parallel work. Break-even against a single native machine of the same class is roughly **8 volunteer nodes**; past that it scales close to linearly.

The value proposition is access to capacity you do not own, not efficiency.

---

## License

MIT

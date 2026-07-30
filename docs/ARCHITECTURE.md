# Architecture

Authoritative for target boundaries, responsibilities, and state machines.
The wire format itself is specified in `PROTOCOL.md`.

---

## 1. System diagram

```
                    ┌────────────────────────────────────┐
                    │   COORDINATOR  (C++ / uWebSockets) │
                    │                                    │
                    │  job manager ── task queue         │
                    │  lease manager ── heartbeat/expiry │
                    │  adaptive sizer                    │
                    │  validator (replication+reputation)│
                    │  signaling relay (WebRTC SDP/ICE)  │
                    │  metrics + SSE feed → dashboard    │
                    │  SQLite (durable job/task state)   │
                    └────────────────────────────────────┘
                         ▲  WebSocket — CONTROL PLANE
                         │  (FlatBuffers frames)
        ┌────────────────┼────────────────┬─────────────────┐
        ▼                ▼                ▼                 ▼
  ┌───────────┐    ┌───────────┐   ┌─────────────┐   ┌─────────────┐
  │  browser  │    │  browser  │   │worker-native│   │ mock-worker │
  │  (WASM)   │    │  (WASM)   │   │  (native)   │   │  (no GPU)   │
  │           │    │           │   │  headless   │   │   chaos     │
  │ worker-core    │ worker-core   │ worker-core │   └─────────────┘
  └───────────┘    └───────────┘   └─────────────┘
        ◄──── WebRTC DataChannel mesh — DATA PLANE ────►
              (bulk assets: scene BVH, datasets, by content hash)
```

**Two planes, deliberately separated:**

- **Control plane** — star topology, WebSocket, coordinator-mediated. Small FlatBuffers messages. Always available. Everything that matters for correctness happens here.
- **Data plane** — mesh, WebRTC, peer-to-peer. Bulk immutable assets addressed by content hash. **Best-effort: any peer fetch must be able to fall back to the coordinator.** The system must remain correct if the data plane is entirely disabled (this is also how you produce the egress comparison in `EVALUATION.md` §6).

**Note the three worker boxes share one `worker-core`.** That is the defining property of this architecture — see §3.

---

## 2. CMake targets

| Target | Type | Builds for | Responsibility |
|---|---|---|---|
| `p2pgpu-protocol` | STATIC | native + wasm | Generated schema headers, framing, Verifier wrappers, invariants |
| `p2pgpu-worker-core` | STATIC | native + wasm | Task loop, kernel host, transport client. **The shared brain-stem.** |
| `coordinator` | EXE | native | The only decision-maker (R1) |
| `worker-native` | EXE | native | Thin `main()` over worker-core |
| `worker-browser` | WASM | wasm | Thin Emscripten entry over worker-core |
| `mock-worker` | EXE | native | Protocol-only chaos harness, no GPU |
| `tests` | EXE | native | Catch2, all tiers |
| `fuzz_protocol` | EXE | native (clang) | libFuzzer harness on the parser |

---

## 3. The dual-target rule (R2)

`worker-core` is compiled twice from identical source. This is the single most important structural property of the codebase and the main payoff of the C++ choice.

Platform differences are confined to **named seam files**, never scattered `#ifdef`s:

```
src/worker-core/
├── task_loop.cpp          portable — no #ifdef permitted
├── kernel_host.cpp        portable — chunking, accumulation, stats
├── accumulator.cpp        portable
├── transport.cpp          portable — speaks to the platform seam below
└── platform/
    ├── gpu_native.cpp     #if !__EMSCRIPTEN__  — wgpu-native device acquisition
    ├── gpu_wasm.cpp       #if  __EMSCRIPTEN__  — Emscripten WebGPU device acquisition
    ├── timer_native.cpp
    └── timer_wasm.cpp     — Emscripten main-loop / setTimeout driving
```

Both seams implement the same header-declared interface (`include/p2pgpu/worker/platform.hpp`). CMake selects the right `.cpp` per target; the rest of `worker-core` never knows which it got.

**Transport needs no seam.** libdatachannel and datachannel-wasm expose the same API, so `transport.cpp` is genuinely portable for both WebSocket and WebRTC. This is the reason the whole single-language architecture works.

**Review rule:** a `#ifdef __EMSCRIPTEN__` appearing outside `src/worker-core/platform/` is a defect. If a portable file seems to need one, the interface is wrong — fix the interface.

---

## 4. Module responsibilities

### `protocol/p2pgpu.fbs` + `src/protocol/`
The schema is the source of truth (R3). `flatc` generates headers at build time into the build tree.

Hand-written C++ alongside it is thin and does only:
- Frame read/write (the binary envelope in `PROTOCOL.md` §1)
- `Verifier` wrappers — **the only sanctioned path from bytes to typed data** (R11)
- The invariants in `PROTOCOL.md` §4
- `PROTOCOL_VERSION`

No I/O, no policy.

### `src/coordinator/`
The only component that makes decisions (R1).

| Component | Responsibility |
|---|---|
| `job` | Job lifecycle, decomposition into tasks, completion detection |
| `queue` | Task queue, priority, cache-affinity-aware assignment |
| `lease` | Lease grant/renew/expire, heartbeat tracking, requeue on expiry |
| `sizer` | Per-worker adaptive task sizing from benchmark score + observed history |
| `validator` | Replication policy, determinism-class-aware comparison, reputation |
| `signaling` | WebRTC SDP/ICE relay, peer list distribution |
| `metrics` | Counters, histograms, SSE stream to dashboard |
| `store` | SQLite persistence, crash recovery |
| `net` | uWebSockets HTTP + WS + SSE, asset serving |
| `kernels` | Loads `kernels/manifest.toml`, serves WGSL by id, builds `Welcome` descriptors |

### `src/worker-core/`
Thin by mandate (R1). Task loop, kernel compilation and dispatch, chunking, accumulation, `TaskStats` collection, transport. Applies coordinator policy; never computes it.

### `src/mock-worker/`
Implements the full protocol, never touches a GPU. Sleeps for a configurable duration, returns canned results. Injectable behaviors:

`slow` · `dies_mid_task` · `returns_garbage` · `lies_probabilistically(p)` · `high_latency(ms)` · `never_renews_lease` · `duplicate_submit` · `flaps`

Runs N virtual workers as N coroutines/tasks in a **single process**. This harness produces most of the charts in `EVALUATION.md` and is the highest-leverage code in the repo. Build it in Phase 2, not later.

### `kernels/`
WGSL plus `kernels/manifest.toml` declaring per kernel: entry point, workgroup size, param layout, output spec, `DeterminismClass`, and the documented FLOP/byte calculation required by R5. Loaded **verbatim** by every worker target.

### `web/`
Static assets only. `index.html` loads the WASM module. `ui.js` holds the DOM glue for the R7 opt-in surface — the one place JavaScript exists, because WASM cannot touch the DOM directly. Called from C++ via `EM_JS`/`EM_ASM` declared in `src/worker-browser/ui_bridge.cpp`.

Keep this glue **dumb and small**: buttons, a slider, a text indicator. Any logic here violates R1 and R2.

---

## 5. State machines

### Task lifecycle

```
   ┌──────────┐
   │  Queued  │◄──────────────────────────────┐
   └────┬─────┘                               │
        │ grant                               │ lease expiry
        ▼                                     │ / explicit release
   ┌──────────┐   renew    ┌───────────┐      │
   │  Leased  │───────────►│  Leased   │──────┘
   └────┬─────┘            └───────────┘
        │ submit
        ▼
   ┌────────────┐
   │ Validating │
   └──┬──────┬──┘
      │      │ disagreement / low confidence
      │      ▼
      │  ┌──────────────┐   issue replica
      │  │ NeedsReplica │──────────────────► Queued (replica_of set)
      │  └──────────────┘
      │ accepted
      ▼
   ┌──────────┐          ┌──────────┐
   │ Accepted │          │ Rejected │ (worker reputation penalized)
   └──────────┘          └──────────┘
```

Model as `enum class TaskState` with a `switch` compiled under `-Werror=switch`, so adding a state breaks the build everywhere it must be handled. **Never write a `default:` arm on a state switch** — that silently defeats the check. Illegal transitions assert; they never no-op silently.

Alternative if the states carry differing payloads: `std::variant` + `std::visit` with an overload set that has no generic fallback. Same exhaustiveness property, more ceremony. Prefer the plain enum unless payloads force otherwise.

### Worker lifecycle

```
Connecting → Registered → Benchmarking → Active ⇄ Throttled
                                            │
                                            ├─► Draining (user stop / tab hidden)
                                            └─► Lost (heartbeat timeout)
```

`Lost` must release all held leases immediately. `Draining` releases leases cleanly and is the polite path.

### Reputation

Per-worker running score in `[0, 1]`, updated on each validated result. Drives:

- **Replication factor.** New/low-reputation workers get replicated; established high-reputation workers do not. This is BOINC's adaptive replication and is what keeps overhead near 1× instead of 2×.
- **Spot-check frequency.** Occasionally issue a task whose answer the coordinator already knows.
- **Blacklist threshold**, with a probation path back.

---

## 6. Key flows

### Worker join
1. Page load → user clicks opt-in (R7) → request adapter, read limits and features
2. WS connect → `Hello{ protocol_version, capabilities }`
3. Coordinator → `Welcome{ worker_id, session_token, heartbeat_ms, kernel_manifest }`
4. Coordinator → `BenchmarkRequest`; worker runs a ~2 s calibration kernel → `BenchmarkResult{ score }`
5. Worker enters `Active`, begins requesting leases

### Task execution
1. Worker → `LeaseRequest{ max_tasks }`
2. Coordinator sizer computes work units for this worker's score → `TaskGrant{ envelope }`
3. Worker resolves `input_ref` (peer first, coordinator fallback) if present
4. Worker executes as **repeated short dispatches** ≤250 ms (R4), accumulating locally, yielding between dispatches
5. Worker → `Progress{ fraction, renew }` periodically; coordinator extends lease
6. Worker → `Result{ payload, stats }`
7. Coordinator validator applies determinism-class comparison → `Accepted` / `NeedsReplica` / `Rejected`

### Failure paths (all must be exercised by the mock harness)
- Lease expires → task requeued, no work lost, worker reputation untouched (absence ≠ malice)
- Device lost → worker releases leases, re-acquires device, re-registers
- Tab hidden → `Draining`, or continue at reduced throttle per user setting
- Job ~95% complete → speculative re-issue of outstanding tasks to idle fast workers; first result wins, loser is discarded
- Coordinator restart → SQLite recovery, all in-flight leases treated as expired
- **Malformed or hostile frame → dropped at the Verifier, connection scored, process unharmed (R11)**

---

## 7. Adaptive task sizing

Target: each task takes **1–3 seconds** on the assigned worker.

```
work_units = target_duration_ms × worker_throughput_score × correction_factor
```

- `worker_throughput_score` from the join-time benchmark
- `correction_factor` is an EWMA over that worker's observed actual-vs-predicted durations
- Clamp to `[min_units, max_units]`; never let a single task exceed the lease duration
- The task is then internally chunked by the worker into ≤250 ms dispatches (R4)

Why 1–3 s: below ~1 s, per-dispatch overhead (24–71 µs) and network RTT stop amortizing; above ~3 s, straggler tails and lease churn get expensive.

---

## 8. Deployment

| Component | Target | Notes |
|---|---|---|
| Coordinator | Fly.io / Railway | Static binary in a slim container; persistent volume for SQLite |
| Worker page | Same origin as coordinator | Serves `.html` + `.js` + `.wasm`; must set **COOP/COEP** |
| TURN | Cloudflare TURN or self-hosted coturn | ~10–20% of peers are behind symmetric NAT and need relay |
| Load-test fleet | Cheap cloud VMs | CPU-only VMs run `mock-worker`; GPU VMs run `worker-native` |

Set COOP/COEP before deploying, not after — discovering it later invalidates test results.

---

## 9. Deliberate non-choices

Recorded here so they are not relitigated. Full rationale in `DECISIONS.md`.

- **No Postgres, no Redis.** SQLite is sufficient at this scale and adds zero operational surface.
- **No DHT for peer discovery.** Coordinator-brokered peer lists are ~10× less code and adequate for the target fleet size.
- **No WebTransport for the data plane.** It is client-server only — no P2P — and is only expected to reach broad implementation in 2027.
- **No hand-rolled parsing.** Every byte from the network enters through the FlatBuffers `Verifier` (R11). Writing a bespoke binary parser would reintroduce exactly the risk C++ was chosen despite.
- **No exceptions across the transport boundary.** See `CONVENTIONS.md` §1 on the error model.
- **No second language.** Not even for the dashboard. The only JavaScript is the DOM glue in `web/ui.js`, and it contains no logic.

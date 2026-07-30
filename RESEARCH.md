# Distributed P2P GPU Sharing with WebGPU — Research & Build Plan

*Research doc, July 2026. Target: resume project optimized for demonstrable systems depth, built fast with heavy AI assistance.*

---

## 1. The honest verdict up front

**The idea works, but not in the form most people imagine it.**

The naive pitch — "a browser-based decentralized GPU cloud, rent idle GPUs from anyone" — is not buildable by one person in a month, and the parts that make it sound impressive (distributed AI training, renting compute for money) are exactly the parts that don't work over consumer internet. If you pitch it that way in an interview, a strong interviewer will find the hole in about 90 seconds.

**What *is* buildable, impressive, and defensible:** a volunteer-compute grid where browser tabs contribute GPU cycles to a shared job, with a coordinator that does adaptive task sizing, straggler mitigation, fault tolerance, and result verification against untrusted nodes — plus P2P asset distribution over WebRTC so the coordinator isn't a bandwidth bottleneck.

That's the same project, scoped to the physics. It's still genuinely hard, it demos beautifully, and every hard part is a real distributed-systems problem you can measure and talk about.

---

## 2. The one calculation that determines everything

This is the core insight. Understand this and the rest of the design falls out.

A worker is only useful if it spends most of its time computing, not transferring. So:

```
required arithmetic intensity  =  compute rate (FLOP/s)  /  network rate (bytes/s)
```

Plug in real numbers:

| Quantity | Realistic value | Notes |
|---|---|---|
| Consumer GPU peak | 5–30 TFLOP/s fp32 | RTX 3060 ≈ 13, M2 ≈ 3.6 |
| **Achievable via WebGPU** | **0.3–2 TFLOP/s** | WebGPU hits ~11–17% of peak vs ~75% for CUDA/cuBLAS |
| Home upload bandwidth | 10–50 Mbps = **1–6 MB/s** | Upload, not download. This is the binding constraint. |
| Round-trip latency | 20–150 ms | |

So: `1e12 FLOP/s ÷ 1e6 B/s` ≈ **10⁶ FLOP per byte moved.**

A million floating-point operations per byte. That is a brutal filter. Now sort candidate workloads by it:

| Workload | FLOP/byte | Verdict |
|---|---|---|
| Distributed matmul (shard a big matrix) | ~10–100 | ❌ Dead on arrival |
| Distributed NN training (sync gradients) | ~10³ | ❌ Needs datacenter interconnect |
| LLM inference, pipeline-parallel (Petals-style) | ~10³–10⁴ | ⚠️ Works but only with fat pipes + big models |
| Path tracing, per-frame tiles | ~10⁴–10⁵ | ⚠️ Close, but 10× short |
| **Path tracing with on-node sample accumulation** | **~10⁶–10⁸** | ✅ Tunable, arbitrarily high |
| **Brute-force / parameter search** (hash preimage, docking, SAT, sweeps) | **~10⁸+** | ✅ Trivially fits |
| **Monte Carlo simulation** (N-body, Ising, options pricing) | **~10⁶–10⁸** | ✅ Fits |

### The trick that makes rendering work

Don't send back a rendered tile. Send back an *accumulated* tile. The worker renders tile T with 200 samples-per-pixel, then 400, then 800 — accumulating into a local buffer — and only uploads the running average every ~2 seconds. Output bytes stay constant while compute grows without bound. Arithmetic intensity becomes a **tuning knob you control**, not a property of the workload.

This one design decision is the difference between "my nodes are 6% utilized" and "my nodes are 95% utilized." It is also exactly the kind of thing worth a paragraph in your README.

**Corollary for what to build:** anything where the *task description* is small (a seed, a range, a tile ID, a parameter vector) and the *result* is small (a scalar, a hash, an averaged buffer) is a green light. Anything that ships bulk data both ways is a red light.

---

## 3. What the actual pieces are

### WebGPU

The browser API that gives JavaScript access to the GPU's *compute* pipeline, not just rendering. This is the thing WebGL never had.

- **Status (2026):** W3C Recommendation. Shipping in Chrome/Edge/Opera (113+), Safari (26.0+), Firefox. ~82% global support. Android needs Chrome 121+ *and* a Qualcomm/ARM GPU.
- **Shader language:** WGSL — Rust-flavored, statically typed, compiles to SPIR-V/MSL/DXIL under the hood. If you know C you'll be fine in a day.
- **Programming model:** you write a `@compute @workgroup_size(x,y,z)` entry point, bind storage buffers, and `dispatchWorkgroups(nx, ny, nz)`. Data goes in via `GPUBuffer`, comes out via a staging buffer you `mapAsync`.
- **Performance reality:** ~11–17% of native CUDA on the same silicon. A well-optimized WebGPU matmul can hit ~1 TFLOP/s. Per-dispatch API overhead is **24–71 µs**, so tasks smaller than a few milliseconds are pure overhead.
- **Optional features worth requesting:** `shader-f16` (2× throughput on many devices), `subgroups` (SIMD-level ops within a workgroup — Google Meet measured **2.3–2.9× speedups** on matrix-vector shaders), `timestamp-query` (nanosecond GPU timing; optional because of timing-attack concerns).
- **No fp64.** No tensor-core access. No direct video encode/decode from compute.

### Where the "P2P" actually belongs

Be precise about this, because "P2P" is the word most likely to get you challenged.

WebRTC `RTCDataChannel` is the only true browser-to-browser transport. It's SCTP-over-DTLS, encrypted, supports unordered/unreliable modes. But:

- Establishing a connection needs a **signaling server** (WebSocket) to exchange SDP offers/ICE candidates. There is no fully serverless browser P2P.
- NAT traversal uses STUN; symmetric NATs (~10–20% of users) fail and need a **TURN relay**, which you pay for in bandwidth. Coturn is easy to self-host.
- Message size limits are ugly: safe cross-browser chunk is **16 KB**, up to 256 KiB with EOR support. You chunk manually.

**So: control plane over WebSocket (star topology), bulk data plane over WebRTC (mesh).** The honest justification for P2P is *asset distribution* — if 50 workers each need the same 200 MB scene BVH, serving that from the coordinator is 10 GB of egress. BitTorrent-style peer swarming makes it O(scene size) instead of O(scene × workers). That's a real, defensible reason for P2P, and it's the one to lead with.

**WebTransport** (QUIC-based) is the modern alternative but is **client-server only** — no P2P — and is still headed to Candidate Recommendation during 2026 with broad implementation expected 2027. Use it for the control plane if you want to look current; don't expect it to replace WebRTC here.

### Headless workers

WebGPU is not browser-only. **`webgpu.h` is a stable multi-vendor C header** ([webgpu-native/webgpu-headers](https://github.com/webgpu-native/webgpu-headers)), jointly maintained by Dawn (Chromium's C++ implementation) and wgpu-native. So the same WGSL kernels run headless from a native binary:

- Load-test with 50 synthetic nodes on cheap cloud VMs without opening 50 browser tabs.
- Run the fault-injection harness and kernel correctness tests in CI, where there is no browser.
- Prove the worker protocol is transport- and host-agnostic.

Under the C++ stack (D-0008) this comes nearly free: `worker-native` and `worker-browser` are thin wrappers over one `worker-core`, so the headless worker *is* the browser worker.

---

## 4. Prior art — know these, they'll come up

**Classical volunteer computing**
- **BOINC / SETI@home / Folding@home** — the canonical reference. Read the BOINC paper; it's short and it already solved most of your problems 20 years ago (job replication, credit accounting, host reliability tracking). Folding@home is the proof that volunteer GPU compute reaches exaflop scale.
- Key BOINC lesson you should steal: **adaptive replication.** Naive 2× replication halves your effective capacity. BOINC instead tracks per-host reliability and only replicates work from unproven hosts, pushing the overhead factor close to 1.

**Browser-based attempts**
- `dis.io`, CrowdProcess, Wasimoff (WebAssembly offloading), `webrtc-tree-overlay` (scalable WebRTC mesh in a tree topology). Mostly CPU/WASM. **A polished WebGPU-native one is a real gap** — that's your opening.

**Modern decentralized ML**
- **Petals** (Yandex Research) — BLOOM-176B served BitTorrent-style: each volunteer hosts a slice of transformer blocks, clients form a pipeline-parallel chain and pass activations along. ~1 step/sec on consumer GPUs; 3–25× better *latency* than offloading, worse throughput.
- **Prime Intellect / OpenDiLoCo** — implements DeepMind's DiLoCo: inner optimizer (AdamW) runs locally for hundreds of steps, outer optimizer (Nesterov SGD) syncs rarely. **~500× less communication.** They trained INTELLECT-1 (10B params) and INTELLECT-2 (32B, RL) across globally distributed nodes. Built on Hivemind, using a DHT for metadata and peer discovery.
- **Takeaway:** the entire field's answer to the bandwidth wall is *communicate less often*, not *compress harder*. Same lesson as your sample-accumulation trick.

**Commercial DePIN GPU markets**
- Akash (reverse-auction leases, on-chain escrow), Render (Solana-settled, merged with Salad in April 2026 adding ~60k GPUs), io.net, Aethir, Nosana, Bittensor. All use **native** GPUs on real machines, not browsers. Useful for the "market design" section of your README; not a technical model for you.

**The cautionary tale**
- **Coinhive** (2017–2019): in-browser Monero mining. The Pirate Bay deployed it silently and misconfigured it, CPUs pegged, every antivirus vendor blacklisted the domain, and the term "cryptojacking" was born. Coinhive shipped "AuthedMine" (opt-in) too late and shut down in 2019.
- **Design consequence, not just ethics:** explicit opt-in, a visible "contributing" indicator, a user-set throttle, and instant stop. Build these in from day one and say so prominently. Silent background GPU use will get your demo domain flagged.

---

## 5. The hard parts (this is where the resume value lives)

Every item here is a real problem with a real, implementable solution. Solving even four of them well is a strong project.

### 5.1 Floating-point nondeterminism breaks naive verification

The obvious verification scheme — send the same task to two nodes, compare bytes — **fails**. GPU float results differ across vendors and drivers due to FMA contraction, fast-math flags, and differing transcendental implementations. Two *honest* nodes will disagree in the last few ULPs.

Fixes, in increasing order of sophistication:
1. Compare with a tolerance (`|a-b| < ε` or relative error). Simple; leaks a small attack surface.
2. Quantize/round results to a canonical precision before hashing, then compare hashes.
3. Design the workload to be **integer-exact** (hash search, combinatorial search) so bitwise comparison works — good for your second workload.
4. Statistical validation: for Monte Carlo, check that the returned distribution is consistent with the seed rather than checking exact values.

Noticing this problem unprompted is a strong signal. Most people don't.

### 5.2 Verifying untrusted compute

You cannot trust a volunteer. They may be malicious, or just have a flaky overclock. The literature (BOINC, SERENE, sabotage-tolerance work) gives you a ladder:

- **Replication** — send to k nodes, majority vote. Costs k×.
- **Spot-checking** — occasionally issue a task whose answer you already know; catch liars probabilistically.
- **Credibility / reputation** — track per-host accuracy, replicate only low-credibility hosts. This is BOINC's adaptive replication.
- **Collusion resistance** — colluding nodes can beat naive replication; SERENE-style schemes randomize replica assignment to make collusion hard.
- Blacklist on detection, with a probation path.

**Implementable in an afternoon; measurable with a fault-injection harness.** Inject 20% Byzantine nodes returning garbage, plot detection rate vs. replication overhead. That chart is the single best artifact this project can produce.

### 5.3 Stragglers and the GPU watchdog

Your slowest node determines job completion. Classic problem, classic fixes:

- **Adaptive task sizing.** Benchmark each node on join, then size tasks so each takes ~1–3 seconds *on that node*. Fast nodes get big tasks, phones get small ones.
- **Hard ceiling from the OS.** Windows TDR (Timeout Detection and Recovery) kills GPU work that blocks for ~2 seconds and resets the driver — the browser surfaces this as a lost `GPUDevice`. **Never dispatch a single long-running kernel.** Split into many short dispatches with yields between them. You must handle `device.lost` and re-acquire.
- **Speculative re-execution.** MapReduce's backup-task trick: when a job is ~95% done, re-issue outstanding tasks to idle fast nodes and take whichever finishes first.
- **Heartbeats + lease expiry.** Tasks are leased, not assigned. A lease that isn't renewed goes back in the queue. This handles the "user closed the tab" case, which is the *normal* case, not the exceptional one.

### 5.4 Browser-specific hostility

| Problem | Mitigation |
|---|---|
| Background tabs are throttled; `requestAnimationFrame` stops firing | Run the worker in a **Web Worker** with WebGPU (supported), drive with `setTimeout`, don't depend on rAF |
| `maxStorageBufferBindingSize` / `maxBufferSize` default to ~128 MB / 256 MB | Query `adapter.limits`, request higher, tile your data to fit the *actual* limits |
| `maxComputeWorkgroupsPerDimension` = 65535 | Loop dispatches or use 2D/3D dispatch grids |
| Chrome blocklists WebGPU on known-bad driver/GPU pairs | Detect adapter-request failure and fall back to WASM/CPU or decline gracefully |
| `SharedArrayBuffer` needs cross-origin isolation (COOP/COEP headers) | Set headers at deploy time; know this before you deploy, not after |
| Device heterogeneity (M1 vs 4090 vs Android) | Capability negotiation at join: report limits + features, coordinator only assigns compatible kernels |

### 5.5 Scheduling and topology

- Work-stealing vs. push scheduling — pull-based is simpler and naturally load-balances across heterogeneous nodes. Recommend pull.
- Locality-aware assignment: once a node has the scene/dataset cached, keep sending it tasks that reuse it. Cache affinity is a real win here since transfer is the expensive thing.
- Peer discovery for the data plane: a small DHT or just coordinator-brokered peer lists. Hivemind uses a DHT; for your scale, coordinator-brokered is fine and 10× less code.

---

## 6. Recommended architecture

```
                    ┌──────────────────────────────┐
                    │     COORDINATOR (C++)        │
                    │  • job + task queue          │
                    │  • lease/heartbeat manager   │
                    │  • adaptive task sizer       │
                    │  • replication + reputation  │
                    │  • WebRTC signaling          │
                    │  • live metrics dashboard    │
                    └──────────────────────────────┘
                       ▲ WebSocket (control plane)
          ┌────────────┼────────────┬────────────┐
          ▼            ▼            ▼            ▼
      ┌───────┐    ┌───────┐    ┌───────┐    ┌────────┐
      │Worker │    │Worker │    │Worker │    │ Headless│
      │ (tab) │    │ (tab) │    │ (tab) │    │ native  │
      └───────┘    └───────┘    └───────┘    └────────┘
          ◄───── WebRTC DataChannel mesh ─────►
             (bulk assets: scene BVH, datasets)
```

**Worker lifecycle**
1. Load page → explicit opt-in click → request `GPUAdapter`, report limits + features
2. Run a 2-second self-benchmark → report throughput score
3. Open WebSocket, register, receive peer list
4. Loop: lease task → fetch inputs (from peers if large, coordinator if small) → dispatch WGSL kernel in short chunks → accumulate → submit result → renew lease
5. On `device.lost`, tab hide, or user throttle: release lease cleanly

**Task envelope** — keep it small and generic:
```
{ taskId, kernelId, params: <uniform buffer, ~KB>,
  inputRef: <content hash | null>, outputSpec: {bytes, dtype},
  leaseMs, replicaOf: <taskId | null> }
```

**Two workloads, one engine.** This is the move that proves the abstraction is real rather than a demo hardcoded to one thing:
- **Workload A — distributed path tracer.** Visually stunning, progressively refines on screen as nodes join, perfect for a demo GIF. Uses sample accumulation (§2) and P2P scene distribution.
- **Workload B — brute-force search** (hash preimage, or a molecular-docking-style parameter sweep). Integer-exact, so bitwise verification works, and it stresses the scheduler with millions of tiny tasks.

---

## 7. Build plan

Aggressive, assumes heavy AI assistance. Adjust the calendar, keep the order — each phase is demoable on its own, so you always have something to show.

> **Superseded by the phase files.** The table below is the original sketch from initial research. The authoritative plan is `docs/ROADMAP.md` plus `docs/phases/PHASE_0.md`–`PHASE_7.md`, which reorder these and add trust/hardening work. Kept for the reasoning, not the schedule.

| Phase | Goal | Done when |
|---|---|---|
| **0. WebGPU hello world** | A WGSL compute shader, Mandelbrot or matmul. Measure GFLOP/s with `timestamp-query`. | You can state your device's real WebGPU throughput |
| **1. Single-worker pipeline** | Coordinator, WebSocket, task envelope, one worker, 1000 tasks end to end | Tasks complete, results are correct |
| **2. Multi-worker + scheduling** | N workers, pull scheduling, self-benchmark, adaptive sizing, heartbeats/leases, straggler re-issue. Live dashboard. | Scaling curve: throughput vs. node count |
| **3. Path tracer** | WGSL path tracer with BVH, tile decomposition, sample accumulation, coordinator-side compositing | Image sharpens live as nodes join |
| **4. P2P data plane** | WebRTC signaling, chunked BVH transfer, peer swarming, egress-vs-node-count measurement | Egress stays flat as workers scale |
| **5. Trust layer** | Replication, tolerance-based comparison, spot-checks, reputation, blacklisting | Byzantine injection harness produces a detection-rate chart |
| **6. Evaluation + writeup** | Fault injection, benchmark suite, README, deployed demo, short video | The numbers in §8 exist |

**Stack: C++20 everywhere** — see `docs/DECISIONS.md` D-0008 for the full reasoning. Briefly: `libdatachannel` (native) and `datachannel-wasm` (browser) expose the same WebRTC/WebSocket API, and WebGPU is available on both targets, so one `worker-core` library compiles to a native binary *and* a WASM module from identical source. That closes the gap that would otherwise force a second language in the browser tab.

The cost is that memory safety at the trust boundary becomes an earned property rather than a free one — addressed by rule R11 and D-0010, and turned into a demonstrable artifact (fuzz corpus, sanitizer CI) rather than a caveat.

---

## 8. What to measure (this is the deliverable)

A project like this is judged on whether you have *numbers*. Produce these:

1. **Scaling curve** — aggregate throughput vs. worker count, 1→50 nodes (use headless native workers on cloud VMs). Include the efficiency line; the point where it bends is the interesting part.
2. **Utilization breakdown** — per task: % time in GPU dispatch vs. transfer vs. idle-waiting-for-work. Shows you understand where time goes.
3. **Fault tolerance** — kill 30% of nodes mid-job. Plot time-to-recovery and confirm zero lost work.
4. **Byzantine detection** — inject f% liars, plot detection rate vs. replication overhead. Compare naive 2× replication against reputation-weighted adaptive replication.
5. **Straggler impact** — job completion time with and without speculative re-execution, on a deliberately heterogeneous fleet.
6. **P2P bandwidth win** — coordinator egress vs. worker count, with and without WebRTC swarming. Should be flat vs. linear.
7. **WebGPU efficiency** — your kernel's achieved GFLOP/s as a fraction of device peak, across 3+ GPU vendors.

---

## 9. How to talk about it (and how not to)

**Say:**
> A volunteer-compute grid that runs GPU workloads across browser tabs via WebGPU. The interesting problems are scheduling across heterogeneous unreliable nodes, verifying results from untrusted workers, and staying compute-bound on 10 Mbps uplinks — which constrains you to workloads above ~10⁶ FLOP/byte, so I designed the task format around on-node accumulation.

**Don't say:** "decentralized AI training," "an alternative to AWS," "blockchain," or anything implying you can beat a datacenter. You can't, the constraint is arithmetic, and claiming otherwise is the fastest way to lose a technically strong interviewer.

**The strongest single sentence you can deliver** is the §2 calculation. Being able to derive, on a whiteboard, why distributed matmul over the internet is hopeless while Monte Carlo is fine — that's a systems-thinking signal that survives any amount of interview probing.

### Known weak points to have answers ready for
- *"Why browsers instead of native?"* → Zero-install participation is the entire distribution model; it's also a genuinely harder engineering environment (sandboxing, TDR, tab lifecycle). Be honest that native would be 6× faster.
- *"Why P2P and not just a server?"* → Asset distribution egress, with the measurement from §8.6 to back it.
- *"What's the real use case?"* → Don't oversell. "Embarrassingly parallel Monte Carlo and search workloads for people who have zero budget and a community willing to donate cycles" is a fine, honest answer.

---

## 10. Reading list, roughly in build order

**WebGPU / WGSL**
- WebGPU Fundamentals — compute shader basics, and the limits-and-features chapter: https://webgpufundamentals.org/webgpu/lessons/webgpu-compute-shaders.html
- MDN WebGPU API reference: https://developer.mozilla.org/en-US/docs/Web/API/WebGPU_API
- "Optimizing a WebGPU Matmul Kernel for 1TFLOP+": https://www.nuss-and-bolts.com/p/optimizing-a-webgpu-matmul-kernel
- Chrome WebGPU release notes (subgroups, f16): https://developer.chrome.com/blog/new-in-webgpu-134
- `webgpu.h`, the multi-vendor C header: https://github.com/webgpu-native/webgpu-headers
- Learn WebGPU for C++: https://eliemichel.github.io/LearnWebGPU/
- libdatachannel (native WebRTC) and datachannel-wasm (same API, browser): https://github.com/paullouisageneau/libdatachannel · https://github.com/paullouisageneau/datachannel-wasm

**Distributed systems**
- BOINC: A Platform for Volunteer Computing (Anderson) — https://arxiv.org/pdf/1903.01699
- "Volunteer computing: the ultimate cloud" — https://boinc.berkeley.edu/boinc_papers/crossroads.pdf
- SERENE: Collusion-Resilient Replication-based Verification — https://arxiv.org/pdf/2404.11410
- Petals: Distributed Inference and Fine-tuning of LLMs Over The Internet — https://arxiv.org/html/2312.08361v1
- OpenDiLoCo — https://arxiv.org/pdf/2407.07852 and https://www.primeintellect.ai/blog/intellect-1

**Networking**
- WebRTC data channels (web.dev): https://web.dev/articles/webrtc-datachannels
- Large data channel messages (Mozilla) — chunking and the 16 KB/256 KiB story: https://blog.mozilla.org/webrtc/large-data-channel-messages/
- WebRTC for the Curious — ICE/STUN/TURN: https://webrtcforthecurious.com/docs/11-faq/

**Rendering**
- Building a Real-Time Path Tracer in WebGPU: https://www.jamesdrandall.com/posts/building-a-real-time-path-tracer-in-webgpu/
- three-gpu-pathtracer (reference implementation to read, not copy): https://github.com/gkjohnson/three-gpu-pathtracer

**Context**
- The End of Coinhive: https://blog.avast.com/coinhive-shuts-down
- awesome-volunteer-computing: https://github.com/ranjithrajv/awesome-volunteer-computing

---

## 11. First three things to do tomorrow

1. Open a blank HTML file, get a WGSL compute shader dispatching, and print your GPU's actual measured GFLOP/s. Everything downstream depends on that number.
2. Write the task envelope struct (§6) before writing any coordinator code. Getting the abstraction right early is what lets Workload B drop in later for free.
3. Get the same kernel running from a headless native binary as well as the browser. Under the C++ stack these share `worker-core`, so proving both targets work on day 3 makes load testing and CI kernel tests free for the rest of the project.

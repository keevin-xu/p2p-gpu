# Glossary

Terms used precisely throughout this repo. Where a word has a loose colloquial meaning, the definition here is the one that applies.

---

**Arithmetic intensity** — FLOP performed per byte transferred. The governing constraint of this project. Must exceed ~10⁶ for a workload to stay compute-bound on consumer uplinks. See `RESEARCH.md` §2, rule R5.

**Accumulation** — a worker refining a result locally over many iterations and uploading only the running average on a fixed cadence. The mechanism that turns arithmetic intensity from a fixed property into a tuning knob. Spec: `AccumulationSpec` in `PROTOCOL.md`.

**Adaptive replication** — replicating tasks only from low-reputation workers rather than uniformly, pushing verification overhead from 2× toward 1×. BOINC's technique; adopted here.

**Control plane** — the WebSocket star topology between coordinator and workers. Authoritative, always available. All correctness-relevant messages travel here.

**Coordinator** — the single C++ server. The **only** component that makes decisions (R1).

**Data plane** — the WebRTC mesh between workers, carrying immutable content-addressed assets. Best-effort; always falls back to the coordinator (D-0007).

**Determinism class** — a kernel's declared reproducibility guarantee: `Exact` (bitwise), `Tolerant{eps}` (float within bounds), or `Statistical` (distributional). Determines how the validator compares replicas. Misdeclaring it causes honest workers to be blacklisted. See R6, D-0003.

**Dispatch** — one `dispatchWorkgroups` call. Must represent ≤250 ms of expected work (R4). A task consists of many dispatches.

**FLOP/byte** — see *arithmetic intensity*.

**Job** — a unit of user-facing work (render this image; search this keyspace). Decomposed into tasks.

**Lease** — a time-bounded grant of a task to a worker. Tasks are leased, never assigned (R8). Expiry returns the task to the queue with no reputation penalty — a worker vanishing is normal, not malicious.

**Mock worker** — a worker that speaks the full protocol but never touches a GPU. Runs N virtual workers as N coroutines in one process. Supports injectable misbehavior. Produces most of the project's evidence.

**Platform seam** — the small set of files under `src/worker-core/platform/` where native and WASM implementations of the same interface diverge (device acquisition, timing, yielding). A `#ifdef __EMSCRIPTEN__` anywhere else is a defect (R2).

**Verifier** — `flatbuffers::Verifier`. The only sanctioned path from network bytes to typed data (R11). A buffer that fails verification is dropped before any field is read.

**worker-core** — the static library containing the task loop, kernel host, and transport, compiled to **both** native and WebAssembly from identical source. `worker-native` and `worker-browser` are thin `main()` wrappers over it. This is the defining structural property of the codebase.

**Reputation** — per-worker score in [0,1] from validated results. Drives replication factor, spot-check frequency, and blacklisting.

**Speculative re-execution** — re-issuing outstanding tasks to idle fast workers near job completion, taking whichever result lands first. MapReduce's backup-task technique. The loser gets `Revoke{SpeculativeLoser}`.

**Spot-check** — issuing a task whose correct answer the coordinator already knows, to probabilistically catch liars without full replication.

**Straggler** — a worker slow enough to dominate job completion time. Addressed by adaptive sizing and speculative re-execution.

**Task** — the unit of scheduling. Defined by `TaskEnvelope`. Sized to 1–3 seconds on the assigned worker.

**TDR** (Timeout Detection and Recovery) — Windows' GPU watchdog. Kills work blocking ~2 s and resets the driver, surfacing as a lost `GPUDevice`. The reason for R4. macOS is more permissive, so a kernel that is fine locally can still kill Windows nodes.

**TURN** — relay server for peers behind symmetric NAT (~10–20% of users) where direct connection fails. Costs bandwidth.

**ULP** (unit in the last place) — the granularity of floating-point representation. Honest GPUs from different vendors disagree by a few ULPs, which is why bitwise verification fails (R6).

**Work units** — kernel-defined quantity of work in a task, produced by the coordinator's adaptive sizer. Meaning varies per kernel (samples, keyspace entries); the sizer treats it as an abstract scalar calibrated by the join-time benchmark.

**Workload A / B** — the two kernels proving the abstraction is generic. A = distributed path tracer (`Statistical`, accumulating). B = brute-force search (`Exact`, non-accumulating). See D-0002.

**Trust boundary** — every point where attacker-controlled bytes enter the process: the WebSocket frame parser, and peer-supplied asset chunks over WebRTC. Code at the boundary is subject to the constraints in `CONVENTIONS.md` §2 and rule R11.

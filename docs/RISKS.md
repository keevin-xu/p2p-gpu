# Risk & Trap Register

Known ways this project fails. Consult when something behaves strangely — the answer is often here. Add entries as new traps are discovered (with a `DECISIONS.md` reference if a design change resulted).

---

## 1. Platform traps

| Trap | Symptom | Mitigation | Phase |
|---|---|---|---|
| **Windows TDR** kills GPU work blocking ~2 s and resets the driver | `device.lost` on Windows nodes only; works fine on your Mac | R4 — never dispatch >250 ms of expected work; chunk and yield | 1 |
| **Background tab throttling** | Worker throughput collapses when the user switches tabs; `rAF` stops firing | Run the task loop in a Web Worker, drive with `setTimeout`, never depend on `requestAnimationFrame` | 1 |
| **`maxStorageBufferBindingSize`** defaults to ~128 MB | Buffer allocation fails on some devices, works on others | K4 — query `adapter.limits` at runtime and tile; never hardcode | 1 |
| **`maxComputeWorkgroupsPerDimension` = 65535** | Large dispatches silently clamp or error | Loop dispatches or use 2D/3D grids | 1 |
| **Chrome driver blocklist** disables WebGPU on known-bad GPU/driver pairs | `requestAdapter()` returns null on some machines | Detect and decline gracefully with a clear user message; count as a capability, not a crash | 1 |
| **Cross-origin isolation (COOP/COEP)** needed for `SharedArrayBuffer` | Works locally, breaks on deploy | Set headers at first deploy, not at the end. Discovering this late invalidates test results. | 7 |
| **`webgpu.h` revision skew between wgpu-native and emdawnwebgpu** | Same `worker-core` source fails to compile for one target: callback shape (`WGPUFuture`/`CallbackInfo` vs. callback+userdata), strings (`WGPUStringView` vs. `const char*`) | **Confirmed, not theoretical** — Emscripten 6 removed `-sUSE_WEBGPU` and says its replacement is "a newer (but incompatible) version of webgpu.h". Step 0.8 measures the gap; widening the platform seam to cover it is pre-authorized (D-0014). If the gap is broad, switch native to Dawn (R-F escape hatch 3). | 0 |
| **Apple Clang has no libFuzzer runtime** | `ld: library 'libclang_rt.fuzzer_osx.a' not found` on any `-fsanitize=fuzzer` build | Not a "quirk" — it is absent. Fuzzing uses the `native-fuzz` preset on Homebrew LLVM (D-0015) | 0 |
| **Safari WebGPU differences** | Kernel works in Chrome, fails or differs in Safari | Test both from Phase 0. Safari 26+ has some limitations vs Chrome. | 0 |
| **Android Chrome needs 121+ and a Qualcomm/ARM GPU** | Phones silently unsupported | Report as capability; do not treat as failure | 4 |
| **Apple unified memory hides transfer cost** | `transfer_ms` ≈ 0 locally, significant on discrete GPUs | Never design assuming readback is free; validate on non-Apple hardware before claiming cross-vendor results | 4 |

---

## 2. Distributed-systems traps

| Trap | Symptom | Mitigation |
|---|---|---|
| **Float nondeterminism read as malice** | Honest workers blacklisted; detection "works" but false-positive rate is terrible | R6 / D-0003 — determinism-class-aware comparison. Suspect this **first** whenever cross-vendor validation misbehaves. |
| **Misdeclared `determinism` field** | Same as above, but the bug is in the manifest, not the validator | `KERNELS.md` §5 cross-implementation test |
| **Clock skew** | Leases expiring early/late, nonsensical durations | Never trust worker clocks. `PROTOCOL.md` §5 — expiry is coordinator-decided, absolute, and the worker measures locally-elapsed time |
| **Duplicate result submission** (speculative winner + loser both land) | Double-counted work, corrupted accumulation | Idempotent submit keyed on `task_id`; first accepted wins, later ones discarded, not errored |
| **Lease expiry treated as malice** | Reputation collapses for users who close tabs — i.e. everyone | Absence ≠ malice (R8). Expiry carries no reputation penalty. |
| **Checksum mismatch treated as malice** | Reputation penalties for flaky networks | Discard and requeue with no penalty; only *wrong answers that pass checksum* count against reputation |
| **Thundering herd on job start** | All N workers request leases simultaneously; coordinator stalls | Jittered `LeaseRequest` backoff; grant `max_tasks > 1` so workers hold a small backlog |
| **Accumulation state lost on device loss** | Silent loss of hours of sample accumulation | Treat accumulated-but-unuploaded work as lost; upload cadence bounds the loss. Document the tradeoff. |

---

## 2b. Trust-boundary traps (C++ specific)

The coordinator's entire input surface is attacker-controlled — anyone can connect as a worker. In C++ these are integrity failures, not just availability failures. Rule **R11** and decision **D-0010** exist for this section.

| Trap | Symptom | Mitigation |
|---|---|---|
| **Declared length exceeds the frame** (`payload_bytes` / `fb_len`) | Heap overread — leaks adjacent memory, plausibly other workers' session tokens. Heartbleed's exact shape. | Check every length against both the frame size and its `kMax*` constant **before** indexing. `PROTOCOL.md` §1 read sequence. |
| **Declared length exceeds the destination buffer** | Heap overflow → potential RCE | Same check, plus preallocate to known size and reject rather than clamp |
| **Integer overflow in chunk offset math** (`index * kChunkBytes`) | Wraps to a small value; writes land at a wildly wrong offset | Checked arithmetic on all attacker-controlled index math; `PROTOCOL.md` §4 invariant 10 |
| **Reading FlatBuffer fields before `Verifier`** | Undefined behavior on a crafted buffer, and it will *appear to work* in testing | Verifier is the only sanctioned bytes→fields path. No "internal" exceptions. |
| **Unbounded allocation from a declared size** | OOM / DoS. **Identical in Rust — the language does not save you here.** | Validate against `kMax*` before allocating, always |
| **Deeply nested tables** | Stack exhaustion during verification | Bounded `kMaxVerifyDepth` / `kMaxVerifyTables` |
| **`assert()` on untrusted input** | Compiles out in release; the check silently disappears in production | Assertions are for internal invariants only. Untrusted input gets a real check in every build. |
| **Malformed frames penalizing task reputation** | Honest-but-buggy clients blacklisted; signal conflated | Frame rejection is connection-level scoring + rate limiting, **not** task reputation. Phase 3 step 3.11. |
| **Sanitizer preset not actually instrumented** | You believe you are covered and are not | Phase 0 step 0.4 plants a deliberate bug and confirms ASan catches it |
| **Experiments run under ASan** | Every timing number in `EVALUATION.md` silently ~2× wrong | Experiments run under `native-release` only (`CONVENTIONS.md` §8) |

---

## 3. Project-level risks

### R-A — The arithmetic-intensity thesis fails empirically
**Signal:** Phase 5 utilization measures well below 85% despite accumulation.
**Response:** This is the project's central claim, so treat it as a finding, not a setback. Measure *where* the time actually goes (E2), and report it. A well-measured negative result is far better than a hidden one. Do not tune the experiment until it agrees with the prediction.

### R-B — Protocol churn after Phase 4
**Signal:** Wanting to change `TaskEnvelope` once three implementations exist.
**Response:** Gate G1 exists precisely to prevent this. If it happens anyway, bump `PROTOCOL_VERSION`, update all three implementations in one commit, and log why G1 missed it.

### R-C — Scope creep via "one more workload"
**Signal:** A third kernel appearing before G7.
**Response:** Stretch goal S2. Two workloads already prove the abstraction (D-0002). A third produces no new measurement.

### R-D — The demo works only on the dev Mac
**Signal:** Never having run on a non-Apple GPU by Phase 5.
**Response:** G4 blocks on this deliberately. Line up a second machine (a friend, a cloud GPU VM, an old laptop) **before** reaching Phase 4, not during it. This is the most commonly deferred and most commonly fatal item.

**Hardware plan (added 2026-07-29).** The non-Apple GPU need is **bursty, not continuous** — it concentrates at two points, with Mac-only development in between. Do not read "Phase 4 passed" as "hardware handled"; it goes dormant for two phases, then G7 hard-requires it again.

- **Phase 4 (validate):** cross-vendor determinism check (4.6), epsilon tightening from measured divergence (4.7), first real E7 rows (4.11), feature-fallback (4.8), transfer-cost reality check (4.9), and real Windows TDR (4.10). Requires the non-Apple machine hands-on.
- **Phases 5–6 (build):** Mac-only is fine. The path tracer, reference render, E2 utilization, the entire P2P data plane, and E6 egress all run on Mac + mock/CPU workers. No borrowed hardware needed.
- **Phase 7 (ship):** required again and non-negotiable — success criterion #1 (≥3 physical GPUs / ≥2 vendors), E7 across ≥3 vendors (7.3), and the E1 real-hardware overlay (7.2). Load-bearing for the thesis, not cosmetic: E7 is the empirical basis for the WebGPU-tax and ~8-node break-even claims, and the cross-vendor run is what backs "cross-vendor determinism holds."

**Primary asset:** a borrowed gaming PC (NVIDIA or AMD + Windows). One box covers the 2nd vendor, the D3D12 backend, real TDR (untestable on macOS), and browser cross-testing. If its CPU has an integrated GPU, that is a free 3rd distinct GPU — possibly a 3rd vendor — on the same machine, selectable via explicit adapter choice at device request.

**Retire the risk early:** during Phase 0 (step 0.16), one in-person session on the borrowed machine's browser — integer kernel bitwise-match vs. Mac (validates `Exact`), float kernel last-ULP divergence (empirical R6 evidence), and a deliberately triggered TDR to confirm device-loss recovery.

That early divergence data pays off twice: it is the R6 evidence, and it lets Phase 3 pick `Tolerant` epsilons from measurement instead of guesswork (step 3.2), which Phase 4 then only has to confirm rather than discover.

**Make the second borrow cheap:** in Phase 4 (step 4.19), package `worker-native` so the machine's owner runs *one command* to join the coordinator over the network (headless, no setup visit). Phase 7 then costs a couple of short favor-runs instead of re-sourcing hardware months later.

**Software adapters (lavapipe/WARP) are CI-correctness only — never counted toward the vendor or determinism claims** (`EVALUATION.md` honesty rule).

### R-E — Docs drift from code
**Signal:** `PROTOCOL.md` disagreeing with `protocol/p2pgpu.fbs`.
**Response:** `WORKFLOW.md` §6 makes doc updates part of "done." For a handoff repo where each session starts cold, a drifted doc is worse than no doc — it actively misleads.

### R-F — Build tooling consumes the timeline
**Signal:** Phase 0 or 1 dominated by CMake, vcpkg, or Emscripten configuration rather than design work.
**Response:** This is the known cost of C++ over `cargo` and it is front-loaded by design — Phase 0 exists partly to absorb it. Budget a day or two; if it stretches well past that, raise it (`WORKFLOW.md` §3) rather than grinding.

Concrete escape hatches, in order of preference:
1. **Emscripten fighting you?** Get the native target fully working first and defer the WASM target within Phase 0. The dual-target claim is structural, not urgent — but do not defer it past G0, since the whole architecture rests on it.
2. **vcpkg fighting you?** Fall back to `FetchContent` for the offending dependency only. Slower cold builds, no other loss.
3. **`wgpu-native` fighting you?** Dawn is the alternative WebGPU implementation, at the cost of a depot_tools/GN build.

What is *not* an escape hatch: reverting to Rust. That decision was made at the cheapest possible moment (D-0008) and the cost of reversing it rises with every phase.

### R-H — Memory safety treated as a checkbox rather than a demonstrated property
**Signal:** R11 mitigations exist in the docs but the fuzz corpus is empty, sanitizer CI is not actually wired, or the boundary audit (step 4.16) never happened.
**Response:** This is the single most likely question a strong interviewer asks about a C++ system whose stated threat model is hostile workers. "I used FlatBuffers" is a weak answer; "here is the corpus, here are the exec counts, here is the ASan CI run, here is the hostile soak result" is a strong one. The gap between those is roughly a day of work — see D-0010 and Phase 7 step 7.15.

Watch specifically for the failure mode where the sanitizer preset silently is not instrumented. Phase 0 step 0.4 exists to catch exactly that.

### R-G — TURN costs or unavailability block Phase 6
**Signal:** No working TURN when P2P work begins.
**Response:** D-0007 makes the data plane best-effort with mandatory coordinator fallback, so this degrades E6 rather than blocking the project. Verify TURN access early; Cloudflare's TURN service is the low-friction option.

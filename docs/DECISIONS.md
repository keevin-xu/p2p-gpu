# Decision Log

Append-only record of consequential choices. This file exists so that a future session — human or agent — can reconstruct *why* the system is the way it is, without archaeology.

---

## 1. RULES FOR THE IMPLEMENTING AGENT

**These are binding. Read before appending.**

### DL1 — Append before implementing
This is rule **R9** in `CLAUDE.md`. The entry is written **before** the code that implements it, not after. Writing it first forces the alternatives to be considered while they are still cheap to choose. An entry added retroactively is a rationalization, not a decision record.

### DL2 — Append only
Never edit or delete an existing entry's Context/Decision/Alternatives/Consequences. If a decision is reversed, append a **new** entry that references and supersedes the old one, and add a `> **SUPERSEDED by D-00NN**` line at the top of the old entry. That one line is the only permitted modification to a past entry.

### DL3 — Number and timestamp strictly
`D-00NN`, zero-padded, monotonically increasing, never reused. Timestamp in ISO 8601 with timezone: `2026-07-29T14:32:00-07:00`. Get the real time from the system clock — do not guess or approximate.

### DL4 — What requires an entry

Write one for any of:

- Choosing between two or more viable technical approaches
- Adding, removing, or replacing a dependency
- Any change to the wire protocol (`PROTOCOL.md`)
- Any new kernel — the entry must contain the **R5 arithmetic-intensity calculation**
- Any change to a public interface between library targets, or to the `worker-core` platform seam
- Any change to scheduling, sizing, validation, or reputation policy
- Any performance-motivated change, with the before/after numbers
- Discovering that a documented assumption is wrong
- Any deviation from `ARCHITECTURE.md` or a phase file
- Anything you would want to explain in an interview

**Not required for:** naming, formatting, test additions, typo fixes, or implementing a step exactly as the phase file specifies it.

### DL5 — Requires human sign-off before implementing
For these, append the entry with `**Status:** PROPOSED`, then **stop and ask** (`WORKFLOW.md` §3):

- Anything that would violate or weaken a hard rule R1–R10
- Adding a runtime dependency not in the `CLAUDE.md` stack table
- Introducing a second language, or moving work across the native/WASM platform seam
- Weakening any R11 constraint, including adding a sanitizer suppression
- Adding a new workload/kernel category
- Any change to the success criteria in `PROJECT_OVERVIEW.md` §4
- Introducing a service (queue, cache, DB) beyond SQLite

Change `PROPOSED` to `ACCEPTED (approved <date>)` only after explicit human approval. Never implement a `PROPOSED` entry.

### DL6 — Record what was rejected
The **Alternatives** section is the most valuable part of an entry. "We chose X" is nearly worthless a month later; "we chose X over Y because Y costs Z" is what prevents relitigating the same argument. Always list at least one real alternative that was seriously considered.

### DL7 — Record measurements, not adjectives
"Faster" is not a decision record. `1.8 s → 0.4 s on a 200-node mock run` is. If a decision was performance-motivated and you have no number, either get the number or say explicitly that the choice was made without measurement.

### DL8 — Link outward
Reference the commit, the phase step (`P2.4`), and any doc section the decision changes. Cross-reference related entries by ID.

---

## 2. Entry template

Copy this exactly.

```markdown
### D-00NN — <short imperative title>

**Date:** 2026-MM-DDTHH:MM:SS-07:00
**Phase / Step:** P<n>.<step>
**Status:** ACCEPTED | PROPOSED | SUPERSEDED by D-00NN
**Affects:** <files, crates, or doc sections>

**Context.**
What situation forced a choice. What was known and unknown at the time.

**Decision.**
What was chosen, stated concretely enough to implement from.

**Alternatives considered.**
- **<Option B>** — why rejected. Include the cost.
- **<Option C>** — why rejected.

**Consequences.**
What this makes easy. What it makes hard or forecloses. What would need to be true to revisit it.

**Measurements.** (required if performance-motivated)
Before / after, with the harness and conditions.
```

---

## 3. Seeded decisions

These were made during project design, before implementation. They are `ACCEPTED` and form the baseline.

> **Reading note for D-0001 … D-0007.** These predate the C++ pivot (D-0008) and are preserved verbatim per DL2 — including their Rust-era `Affects:` paths and dependency names. Read them as their C++ equivalents:
>
> | Written as | Now means |
> |---|---|
> | `crates/protocol/` | `protocol/p2pgpu.fbs` + `src/protocol/` |
> | `crates/coordinator/<x>` | `src/coordinator/<x>` |
> | `crates/worker-native/` | `src/worker-core/` + `src/worker-native/` |
> | `crates/mock-worker/` | `src/mock-worker/` |
> | `web/worker/` (TypeScript) | `src/worker-browser/` (WASM over `worker-core`) |
> | `rusqlite` | the SQLite C API |
> | `bindings/` + `ts-rs` | deleted — no codegen boundary exists |
>
> The *reasoning* in these entries is unaffected by the pivot; only the file paths and library names moved. D-0004 is the exception — it was superseded outright.

---

### D-0001 — Constrain all workloads by arithmetic intensity rather than pursuing general GPU sharing

**Date:** 2026-07-29T00:00:00-07:00
**Phase / Step:** Design
**Status:** ACCEPTED
**Affects:** `PROJECT_OVERVIEW.md`, `KERNELS.md`, rule R5

**Context.**
The initial concept was a general "decentralized GPU cloud." Analysis of the compute-to-bandwidth ratio showed this is not achievable over consumer internet: a WebGPU-capable node delivers ~1 TFLOP/s against ~1 MB/s upload, requiring ~10⁶ FLOP per transferred byte to remain compute-bound. Distributed matmul (~10²), NN training (~10³), and naive per-frame rendering (~10⁴) all fail this by orders of magnitude.

**Decision.**
Scope the system to workloads above 10⁶ FLOP/byte, and require every kernel to document its ratio (R5). Where a workload falls short, restructure it with on-node accumulation so the ratio becomes a tuning knob rather than a fixed property.

**Alternatives considered.**
- **Build the general version anyway** — would produce a system that is provably idle-bound; the demo would show near-zero scaling. Rejected as dishonest and undemonstrable.
- **Aggressive result compression** — buys at most ~10×, needs ~10⁴. Does not close the gap.
- **Restrict to LAN** — would work, but eliminates the volunteer-internet premise that motivates the project.

**Consequences.**
Makes the project honest and its scaling curve real. Forecloses distributed training as a workload. Requires `AccumulationSpec` in the protocol and makes the R5 gate a permanent authoring cost. This constraint is the project's central intellectual contribution and should lead the README.

---

### D-0002 — Two workloads (path tracer + brute-force search) against one task abstraction

**Date:** 2026-07-29T00:00:00-07:00
**Phase / Step:** Design
**Status:** ACCEPTED
**Affects:** `kernels/`, phases 1 and 5

**Context.**
A single-workload system cannot demonstrate that the task/scheduling abstraction is real rather than a demo hardcoded to one problem.

**Decision.**
Ship two: (A) a distributed path tracer — visually demonstrable, requires accumulation, motivates P2P asset distribution; (B) brute-force search — integer-exact so bitwise verification is valid, and stresses the scheduler with many small tasks.

**Alternatives considered.**
- **Path tracer only** — better demo, but no `Exact` determinism class to validate the verification path against, and no scheduler stress case.
- **Three or more workloads** — diminishing returns against a tight timeline.

**Consequences.**
Forces `DeterminismClass` to exist from the start, which is what surfaces the float-nondeterminism problem (R6) early rather than as a late surprise.

---

### D-0003 — Verification must be determinism-class-aware, not bitwise

**Date:** 2026-07-29T00:00:00-07:00
**Phase / Step:** Design
**Status:** ACCEPTED
**Affects:** `PROTOCOL.md`, `crates/coordinator/validator`, rule R6

**Context.**
The obvious replication check — send a task to two workers, compare bytes — fails on GPUs. Honest nodes on different vendors/drivers disagree in the last few ULPs due to FMA contraction, fast-math flags, and differing transcendental implementations.

**Decision.**
Each kernel declares a `DeterminismClass`: `Exact` (integer, bitwise comparison valid), `Tolerant{rel_eps, abs_eps}` (float, compare within stated bounds), or `Statistical` (Monte Carlo, distributional consistency check). The validator dispatches on this.

**Alternatives considered.**
- **Bitwise only, restrict to integer workloads** — would exclude the path tracer and most interesting GPU work.
- **Canonical rounding then hash** — viable and simpler, but loses information about *how far* apart two results are, which the reputation system wants.

**Consequences.**
A kernel that misdeclares its class causes honest workers to be blacklisted — a nasty bug to diagnose. Mitigated by the cross-implementation test required in `KERNELS.md` §5.

---

### D-0004 — Rust for everything except the browser tab

> **SUPERSEDED by D-0008** (2026-07-29). Retained in full per DL2 — the analysis below is still the reason the *shape* of the architecture is what it is, and one of its rejected alternatives turned out to rest on a factual error worth remembering.

**Date:** 2026-07-29T00:00:00-07:00
**Phase / Step:** Design
**Status:** SUPERSEDED by D-0008
**Affects:** whole repo, `CLAUDE.md` stack table

**Context.**
`wgpu` compiles to `wasm32-unknown-unknown` and drives the browser's WebGPU, so an all-Rust implementation including the browser worker is genuinely possible. The question was where to put the language boundary.

**Decision.**
Rust for protocol, coordinator, native worker, and mock worker. TypeScript only for the browser worker and dashboard. Types cross via `ts-rs` codegen; logic never crosses (R2).

**Alternatives considered.**
- **All TypeScript** — fastest to build, but loses compile-time exhaustiveness on the task-lifecycle state machine, which is the system's core correctness surface. Also weaker signal for the project's purpose.
- **All Rust including a WASM browser worker** — architecturally elegant (one `worker-core` crate, two targets via `cfg(target_arch)`). Rejected because `web-sys` WebRTC/DOM bindings are where tooling quality and AI assistance collapse; estimated 1.5–2 weeks of additional risk on a fast timeline. Explicitly retained as a stretch goal in Phase 7 — the WGSL kernels are already language-neutral, so the port is mostly the networking layer.

**Consequences.**
Two toolchains and two test runners in CI. Requires the `ts-rs` codegen step to be wired into the build or types drift silently. Requires structured logging with correlation IDs from day one so cross-boundary failures are diagnosable. Makes R1 (dumb worker) load-bearing rather than merely tidy — if the worker held logic, it would have to exist in both languages.

**Note.** An earlier version of this analysis claimed the mock-worker harness was substantially cheaper in Rust because TypeScript would need one process per mock node. That was wrong — the correct TS design is N async tasks in one process, which Deno handles fine. The argument was withdrawn; it is not a reason for this decision.

---

### D-0005 — Pull-based (work-stealing) scheduling, not push

**Date:** 2026-07-29T00:00:00-07:00
**Phase / Step:** Design
**Status:** ACCEPTED
**Affects:** `crates/coordinator/queue`, `PROTOCOL.md` `LeaseRequest`

**Context.**
The fleet is extremely heterogeneous — an M-series laptop, a phone, and a desktop with a discrete GPU may differ by 20× in throughput, and any node may be throttled by its user at any time.

**Decision.**
Workers request work (`LeaseRequest`); the coordinator decides how much to grant. Naturally load-balances without the coordinator needing an accurate model of each node's instantaneous state.

**Alternatives considered.**
- **Push scheduling** — requires the coordinator to track per-worker capacity precisely, which is exactly what it cannot do reliably across throttling, thermal limits, and tab backgrounding.

**Consequences.**
Adds one RTT per task acquisition. Mitigated by allowing `max_tasks > 1` so a worker can keep a small local backlog. Note the sizing decision still lives in the coordinator (R1) — the worker asks *whether*, never *how much*.

---

### D-0006 — SQLite for coordinator state; no external services

**Date:** 2026-07-29T00:00:00-07:00
**Phase / Step:** Design
**Status:** ACCEPTED
**Affects:** `crates/coordinator/store`, deployment

**Context.**
The coordinator needs durable job/task state to survive restart.

**Decision.**
SQLite via `rusqlite`, single file on a persistent volume. Hot path stays in memory; SQLite is the durability layer and crash-recovery source.

**Alternatives considered.**
- **Postgres** — operational overhead with no benefit at this scale.
- **Redis** — another process to run, and durability semantics that are worse for this use.
- **In-memory only** — coordinator restart would lose all jobs, and "coordinator restart recovers cleanly" is a success criterion.

**Consequences.**
Single-coordinator only; horizontal scaling of the coordinator is foreclosed. Acceptable and explicitly out of scope.

---

### D-0007 — WebRTC data plane is best-effort with mandatory coordinator fallback

**Date:** 2026-07-29T00:00:00-07:00
**Phase / Step:** Design
**Status:** ACCEPTED
**Affects:** `ARCHITECTURE.md` §1, phase 6

**Context.**
NAT traversal fails for roughly 10–20% of peers (symmetric NAT), requiring TURN relay, which costs bandwidth and can be unavailable.

**Decision.**
The control plane is authoritative and always available over WebSocket. The P2P data plane carries only immutable content-addressed assets, and every peer fetch falls back to `GET /asset/{hash}` on the coordinator. The system must be fully correct with the data plane disabled.

**Alternatives considered.**
- **P2P-mandatory** — would make correctness depend on NAT topology. Unacceptable.
- **No P2P at all** — simpler, but removes the only honest justification for "P2P" in the project's name and the egress measurement that supports it.

**Consequences.**
Enables the `EVALUATION.md` §6 experiment (egress with vs. without P2P) essentially for free, since the fallback path is the control condition.

---

### D-0008 — Pivot to C++20 for the entire system, including the browser worker

**Date:** 2026-07-29T00:00:00-07:00
**Phase / Step:** Design (pre-Phase-0)
**Status:** ACCEPTED
**Supersedes:** D-0004
**Affects:** whole repo — build system, all targets, `CLAUDE.md`, `ARCHITECTURE.md`, `PROTOCOL.md`, `CONVENTIONS.md`, all phase files

**Context.**
D-0004 chose Rust everywhere except the browser tab, on the reasoning that `web-sys` WebRTC/DOM bindings are where tooling quality collapses, making an all-Rust stack (including a WASM browser worker) too risky on a fast timeline. That reasoning was correct **for Rust** but was over-generalized to "any non-JS language pays this cost in the browser." It does not hold for C++:

- [`libdatachannel`](https://github.com/paullouisageneau/libdatachannel) is a standalone C++17 WebRTC DataChannel + WebSocket implementation for native platforms.
- [`datachannel-wasm`](https://github.com/paullouisageneau/datachannel-wasm) exposes **the same API** compiled to WebAssembly for browsers, integrated transparently via CMake.

So the exact gap that forced a language seam in the Rust plan is closed by a maintained library in C++. WebGPU compute is available on both targets (`wgpu-native` implementing the standard `webgpu.h` for native, Emscripten's WebGPU bindings for browser), and WGSL kernels were already language-neutral.

Separately: the target audience for this portfolio project is FAANG / quant / big-tech systems roles, where C++ is the common denominator — near-universal in quant, standard in infra and graphics — whereas Rust is a bonus at some and irrelevant at others.

**Decision.**
C++20 for every component: coordinator, `worker-core`, native worker, browser worker (via Emscripten), and mock harness. No TypeScript, no second language, no cross-language codegen. A single `worker-core` static library compiles to both native and WASM from identical source, with platform differences confined to named seam files under `src/worker-core/platform/` (rule R2).

The only JavaScript in the repo is `web/ui.js` — DOM glue for the R7 opt-in surface, because WASM cannot touch the DOM directly. It contains no logic and is called from C++ via `EM_JS`.

**Alternatives considered.**
- **Stay with Rust + TypeScript tab (D-0004).** Rejected: gives up the single-language architecture for a seam that C++ does not need, and Rust is weaker signal for the specific roles targeted. The memory-safety advantage is real but addressable — see D-0010.
- **All Rust including a WASM browser worker.** Still blocked by `web-sys` WebRTC ergonomics; the C++ equivalent has a purpose-built cross-target library and Rust does not.
- **C++ backend with a TypeScript tab.** Explicitly rejected as the worst configuration: forfeits the single-language benefit (the main reason to pick C++ here) while keeping all of C++'s costs, and reintroduces a codegen/IDL boundary that the all-C++ design deletes entirely.
- **Deferring the decision until after Phase 1.** Rejected: switching cost is near zero right now (ten stub files, no implementation) and rises steeply once three worker implementations exist. This is the cheapest possible moment.

**Consequences.**
*Simplifications gained:*
- `bindings/` and the entire codegen step are deleted. One shared schema, one language.
- `worker-native` becomes nearly free — it is a thin `main()` over the same `worker-core` the browser uses. Phase 4 loses most of its original content and is repurposed (see D-0012).
- R2 changes meaning from "never duplicate logic across languages" to "never fork `worker-core` across targets" — structurally easier to enforce, since a compiler builds both.

*Costs accepted:*
- **Memory safety on untrusted input** becomes an earned property rather than a free one. This is the material downside and is addressed as its own decision, D-0010, plus hard rule R11.
- State-machine exhaustiveness needs `-Werror=switch` discipline rather than the compiler enforcing it unconditionally.
- Build and dependency management is heavier than `cargo`. Mitigated by D-0011.
- Emscripten SDK is an additional toolchain to install and keep current.

*Explicitly not a cost:* WGSL kernels, the protocol design, the scheduling and verification algorithms, and all seven evaluation experiments are language-independent and carry over unchanged.

---

### D-0009 — FlatBuffers as the wire format

**Date:** 2026-07-29T00:00:00-07:00
**Phase / Step:** Design (pre-Phase-0)
**Status:** ACCEPTED
**Affects:** `protocol/p2pgpu.fbs`, `src/protocol/`, `PROTOCOL.md`, rules R3 and R11

**Context.**
The Rust plan used `serde` JSON control messages with a hand-written binary framing layer, plus `ts-rs` codegen for the TypeScript side. With the C++ pivot the codegen requirement disappears, but a new one appears: **the parser is now the primary attack surface in a memory-unsafe language**, reachable by any anonymous worker.

**Decision.**
FlatBuffers. `protocol/p2pgpu.fbs` is the single source of truth (R3); `flatc` generates C++ headers as a CMake build step into the build tree — never committed, never hand-edited. All control messages are FlatBuffers `Envelope` unions in binary WebSocket frames. Bulk result payloads ride *outside* the FlatBuffer in the same frame, since their interpretation is fully determined by `OutputSpec` and verifying 8 MiB of GPU output buys nothing.

Every buffer passes `flatbuffers::Verifier` — with bounded depth and table count — before a single field is read. The exact required sequence is specified in `PROTOCOL.md` §1.

**Alternatives considered.**
- **JSON (nlohmann).** Human-readable on the wire and pleasant to debug, but it puts hand-written validation and depth-attack defense on us, in the language where those mistakes are most expensive. Rejected on R11 grounds. FlatBuffers' reflection-based JSON dump recovers most of the debuggability.
- **Protobuf.** Also schema-driven and heavily fuzzed; slightly more industry-standard as a résumé keyword. Rejected for the lack of a first-class untrusted-buffer verifier equivalent and no zero-copy reads. The margin was small.
- **Hand-rolled binary parsing.** Rejected outright. It would reintroduce precisely the risk that C++ was chosen despite, for no benefit.

**Consequences.**
Removes essentially all hand-written parsing from the trust boundary — the remaining hand-written surface is the ~12-byte fixed frame header, which is small enough to fuzz exhaustively. Costs a schema-compilation build step and makes the wire format non-human-readable by default. FlatBuffers' additive-field compatibility means most schema evolution will not require a `PROTOCOL_VERSION` bump.

---

### D-0010 — Memory-safety posture at the trust boundary

**Date:** 2026-07-29T00:00:00-07:00
**Phase / Step:** Design (pre-Phase-0)
**Status:** ACCEPTED
**Affects:** rule R11, `CONVENTIONS.md` §2 and §8, `fuzz/`, CI

**Context.**
The coordinator accepts WebSocket connections from anonymous, unauthenticated strangers — that is the product, not a weakness. Every byte reaching the parser is attacker-controlled. In Rust, a parser bug is a panic (availability). In C++, it is potentially an information leak or remote code execution (integrity).

This project's threat model is unusually explicit about hostility: it has a reputation system, a blacklist, and an evaluation experiment (E4) that measures Byzantine detection. A memory-unsafe input path would directly contradict the security posture the rest of the architecture assumes — and it is the first thing a sharp reader will probe.

Concretely, the framing layer contains the classic bug shapes: an attacker-supplied `payload_bytes` larger than the frame (Heartbleed-style overread), larger than the destination buffer (heap overflow), and asset-chunk `index * CHUNK_SIZE` offset arithmetic that can be made to wrap.

**Decision.**
Treat memory safety as an explicitly engineered, *demonstrated* property rather than an assumed one. Codified as hard rule **R11**:

1. All deserialization through `flatbuffers::Verifier`, following the sequence in `PROTOCOL.md` §1. No path skips it.
2. No raw pointer arithmetic, `memcpy`, C arrays, or `reinterpret_cast` in any file touching network bytes. `std::span` / `std::string_view` / bounds-checked accessors only.
3. Every length field validated against its `kMax*` constant **before allocation**.
4. All offset arithmetic on attacker-controlled indices uses checked arithmetic.
5. ASan + UBSan on the default dev preset and every CI run; TSan on coordinator tests.
6. libFuzzer harnesses on the frame parser and asset reassembly, running in CI with a committed corpus. Any crash found becomes a committed regression seed.
7. Release hardening: `-D_GLIBCXX_ASSERTIONS -D_FORTIFY_SOURCE=2 -fstack-protector-strong`.
8. Never `assert()` on attacker input — assertions compile out; untrusted input gets a real check in every build.

**Alternatives considered.**
- **"Be careful."** Rejected. It is not a testable property and is the answer that loses the room in an interview.
- **Sandbox the parser in a separate process** (seccomp / subprocess). Genuinely stronger, and rejected as disproportionate for a portfolio project — significant complexity for a threat that is theoretical here.
- **Accept the risk and note it in limitations.** Rejected: the mitigation is roughly a day of work and converts a liability into a demonstrable artifact.

**Consequences.**
Adds a fuzzing and sanitizer setup task to Phase 0/1 and a standing constraint on how boundary code is written. In exchange, the project gains a concrete, showable answer to *"your threat model says workers are hostile — what is your parser's memory-safety story?"*: a committed corpus, sanitizer CI, and schema-driven deserialization. Rust would have given the property free and invisibly; here it costs a day and is visible, which for a portfolio artifact is arguably the better trade.

Note this does **not** eliminate the resource-exhaustion class of bug, which is identical in any language — hence invariant checks 3 and 4 above.

---

### D-0011 — CMake + vcpkg manifest mode for build and dependencies

**Date:** 2026-07-29T00:00:00-07:00
**Phase / Step:** Design (pre-Phase-0)
**Status:** ACCEPTED
**Affects:** `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, CI, `PHASE_0.md`

**Context.**
C++ has no `cargo`. The build must produce native binaries on macOS (dev), Windows (the borrowed cross-vendor machine, `RISKS.md` R-D), and Linux (CI and cloud load-test fleet), plus a WASM target via Emscripten — reproducibly, with pinned dependency versions.

**Decision.**
CMake ≥3.25 with `CMakePresets.json` defining `native-debug` (ASan+UBSan), `native-tsan`, `native-release`, and `wasm` (Emscripten toolchain). Dependencies via vcpkg **manifest mode** (`vcpkg.json`) with exact pinned versions. `wgpu-native` and `datachannel-wasm` come via `FetchContent` since they are not vcpkg ports.

**Alternatives considered.**
- **FetchContent for everything.** No package manager to install and a simpler bootstrap, but every dependency is compiled from source on every cold build, which is slow and painful across three platforms.
- **Conan.** Comparable to vcpkg with better binary caching and somewhat more common in quant shops specifically. Rejected on config ceremony; the margin was small and vcpkg's manifest mode is simpler to keep reproducible.
- **Hand-rolled Makefiles.** Rejected — Emscripten cross-compilation and multi-platform CI make this a false economy.

**Consequences.**
Requires vcpkg and the Emscripten SDK as documented prerequisites (Phase 0 step 0.1). Presets make "which build am I running" explicit, which matters because **experiments must never run under sanitizers** (`CONVENTIONS.md` §8) — a mistake that would silently corrupt every timing measurement in `EVALUATION.md`.

---

### D-0012 — Repurpose Phase 4 now that `worker-native` is nearly free

**Date:** 2026-07-29T00:00:00-07:00
**Phase / Step:** Design (pre-Phase-0)
**Status:** ACCEPTED
**Affects:** `ROADMAP.md`, `docs/phases/PHASE_1.md`, `docs/phases/PHASE_4.md`

**Context.**
Phase 4 was originally "Native Worker & Cross-Vendor" — the bulk of it was *building a second worker implementation* in Rust to match the TypeScript browser worker. Under D-0008 that work no longer exists: `worker-native` and `worker-browser` are both thin `main()` wrappers over the same `worker-core`, so the second implementation falls out of the first at near-zero cost.

**Decision.**
Move `worker-core` dual-target compilation into **Phase 1**, where both the native and WASM workers come up together. Repurpose **Phase 4** as "Cross-Vendor Validation & Hardening": cross-vendor determinism verification, kernel tests in CI, sanitizer and fuzzing hardening, feature-fallback verification, and the mixed-fleet run.

Gate G4's question changes from *"is the task abstraction implementation-independent?"* (now proven structurally by construction) to *"does the determinism classification hold across real vendors, and is the trust boundary actually hardened?"*

**Alternatives considered.**
- **Leave the phase structure alone.** Rejected: Phase 4 would be mostly empty and Phase 1 would ship a browser worker while a native binary sat one CMake target away, unbuilt — needlessly delaying headless CI kernel testing.

**Consequences.**
Phase 1 grows modestly; Phase 4 shrinks and sharpens. Headless kernel testing in CI becomes available from Phase 1 instead of Phase 4, which is a real acceleration — kernel bugs get caught by CI two phases earlier. The `RISKS.md` R-D hardware plan is unaffected: the borrowed non-Apple machine is still needed at Phase 4 and again at Phase 7.

---

### D-0013 — Pin the toolchain and the vcpkg baseline

**Date:** 2026-07-29T19:26:04-05:00
**Phase / Step:** P0.1 / P0.2
**Status:** ACCEPTED
**Affects:** `vcpkg.json`, `CMakePresets.json`, `docs/STACK.md`, `CLAUDE.md` commands table

**Context.**
`PHASE_0.md` step 0.1 requires recording exact toolchain versions, and step 0.2 requires pinning dependency versions so the dev Mac, the borrowed Windows machine (`RISKS.md` R-D), and CI resolve identically. The scaffold shipped `"builtin-baseline": "REPLACE_ME_IN_STEP_0.2"`, which cannot resolve at all.

**Decision.**
Toolchain as installed on the dev Mac (macOS 15.6 / Darwin 24.6.0, arm64):

| Tool | Version |
|---|---|
| CMake | 4.3.2 |
| Ninja | 1.12.1 |
| Apple Clang | 17.0.0 (clang-1700.0.13.5) |
| Homebrew LLVM | 22.1.8 (fuzzing only — see D-0015) |
| Emscripten | 6.0.5 |
| vcpkg | 2026-07-13-bf04c909169fdbb30821c02c6eb01f1cd1295d05 |

Dependencies pinned by **`builtin-baseline` alone**, set to vcpkg commit `827a2e1203bc19941126c657166da44f2623acc4`. Versions this baseline resolves to: flatbuffers 25.12.19, spdlog 1.17.0, cli11 2.6.2, stduuid 1.2.3, libdatachannel 0.24.5, uwebsockets 20.79.0, sqlite3 3.53.4, blake3 1.8.5, catch2 3.15.3.

**Alternatives considered.**
- **Baseline plus an `overrides[]` block naming every direct dependency.** A literal reading of `CONVENTIONS.md` §10 ("pin exact versions"), and more legible to a reader. Rejected: a baseline commit already pins the entire graph — including transitive dependencies, which `overrides[]` does not. Worse, overrides survive a baseline bump, so a future update would silently hold direct dependencies back while their transitive deps move, which is the standard way to manufacture an ABI mismatch. The baseline is the stronger pin; the resolved versions are recorded above instead, where they cost nothing to keep accurate.
- **`FetchContent` for everything, no vcpkg.** Already rejected in D-0011.

**Consequences.**
Reproducible across all three platforms from one line of config. Updating dependencies is a single deliberate baseline bump, which is reviewable as one diff. `wgpu-native` and `datachannel-wasm` still arrive via `FetchContent` and must be pinned separately at their own steps — they are not covered by this baseline.

---

### D-0014 — Adopt the `emdawnwebgpu` port; pre-authorize widening the platform seam

**Date:** 2026-07-29T19:26:04-05:00
**Phase / Step:** P0.2 (finding) / P0.8 (resolution)
**Status:** ACCEPTED
**Affects:** `CMakeLists.txt`, `include/p2pgpu/worker/platform.hpp`, `docs/ARCHITECTURE.md` §3, `docs/RISKS.md` §1, rule R2

**Context.**
The scaffold links `worker-browser` with `-sUSE_WEBGPU=1`. Emscripten 6.0.5 — the version installed at step 0.1 — has **removed** that setting. `tools/settings.py:259` reads:

> `USE_WEBGPU`: *No longer supported; replaced by `--use-port=emdawnwebgpu`, which implements a newer (but incompatible) version of webgpu.h*

This is not a deprecation warning; it is a hard configure error, and the toolchain is stating outright that the two `webgpu.h` revisions are incompatible. emdawnwebgpu is Dawn's fork of the original bindings.

This matters beyond a flag change. R2 claims `worker-core` compiles to both targets from identical source, with platform differences confined to `src/worker-core/platform/`. But `platform.hpp` as scaffolded abstracts only device acquisition, timing, yield, and logging — everything else in `worker-core` (buffer creation, pipeline creation, dispatch, readback) would call `webgpu.h` directly. That is only sound if `wgpu-native` and emdawnwebgpu agree at the *same header revision*. Historically they have not: callback style (`WGPUFuture` / `CallbackInfo` vs. legacy callback + userdata) and string handling (`WGPUStringView` vs. `const char*`) have both diverged.

**Decision.**
Two parts.

1. Replace `-sUSE_WEBGPU=1` with `--use-port=emdawnwebgpu` on both compile and link for the WASM target.
2. **Pre-authorized (approved by Kevin, 2026-07-29):** if step 0.8 shows the two implementations disagree at the API level, widen the `platform.hpp` seam to cover the differing surface — buffer/pipeline creation, dispatch, readback — rather than stopping for a gate. The same pre-authorization applies to step 1.18: if `datachannel-wasm` turns out to be a compatible *subset* of libdatachannel rather than an identical API, add a thin transport seam. Each use requires its own `DECISIONS.md` entry recording exactly what diverged.

Widening the seam is **not** an R2 violation. R2 forbids *forking `worker-core`* — duplicating logic across targets. Moving a divergent API behind one interface with two implementations is precisely what R2 prescribes; `platform.hpp` already says so: *"If a portable file appears to need one, THIS INTERFACE IS WRONG — widen it here rather than forking the caller."*

**Alternatives considered.**
- **Pin an older Emscripten that still has `USE_WEBGPU`.** Would defer the problem onto a deprecated, unmaintained binding while Chrome and Safari move on, and would fight step 0.13 (Safari) and Phase 4's cross-vendor work. Rejected.
- **Use Dawn natively instead of `wgpu-native`,** so both targets are Dawn and the header revision matches by construction. Genuinely attractive and is the natural fallback if 0.8 shows real divergence — `RISKS.md` R-F already names it. Rejected as the default only because of Dawn's depot_tools/GN build (D-0011), which is a heavy prerequisite to take on before knowing whether it is needed.
- **Stop at a human gate if 0.8 diverges.** Rejected by the human as unnecessary friction: the resolution is already known and bounded, and R2's own text prescribes it.

**Consequences.**
Unblocks the WASM target on the installed toolchain. Raises the value of step 0.8 sharply — it is now the step most likely to change the architecture, so it runs **out of order, immediately after the build is green**, while the cost of moving the seam is still near zero. If divergence is broad rather than narrow, switching native to Dawn (alternative 2) becomes the preferred answer and gets its own entry.

---

### D-0015 — Separate `native-fuzz` preset on Homebrew LLVM; Apple Clang stays the default

**Date:** 2026-07-29T19:26:04-05:00
**Phase / Step:** P0.2
**Status:** ACCEPTED
**Affects:** `CMakePresets.json`, `CMakeLists.txt`, `fuzz/CMakeLists.txt`, `CLAUDE.md` commands table, `docs/CONVENTIONS.md` §8

**Context.**
The scaffold sets `P2PGPU_BUILD_FUZZ=ON` in `native-debug`, so the default development preset builds the libFuzzer harness. **Apple Clang cannot do this** — verified at step 0.1:

```
ld: library '.../libclang_rt.fuzzer_osx.a' not found
```

Apple ships no libFuzzer runtime. `PROJECT_OVERVIEW.md` §7 describes this as "libFuzzer quirks"; it is a hard gap, so as scaffolded the default preset cannot configure at all.

Homebrew LLVM 22.1.8 has libFuzzer and works (smoke-tested). But it defaults to its **own** libc++ headers, while vcpkg builds ports with the triplet's default compiler (Apple Clang, system libc++). Compiling our code against one libc++ and linking libraries built against another is an ABI mismatch across every `std::` type crossing that boundary.

**Decision.**
Apple Clang remains the compiler for `native-debug`, `native-tsan`, and `native-release`, matching the stock `arm64-osx` triplet so no ABI question arises. `P2PGPU_BUILD_FUZZ` is `OFF` in those presets.

Add a dedicated `native-fuzz` preset that uses Homebrew LLVM with ASan + UBSan, plus a `P2PGPU_FUZZ_ONLY` option that builds **only** `p2pgpu-protocol` and the fuzz harnesses. This keeps the ABI surface to header-only code: `flatbuffers::Verifier` lives entirely in headers, so the fuzz target links no vcpkg-compiled library.

**Standing constraint this creates:** `p2pgpu-protocol` must not acquire a dependency on a compiled vcpkg library. In particular it must not link spdlog — logging at the trust boundary is the caller's job. Recorded in `fuzz/CMakeLists.txt`; violating it silently reintroduces the ABI mismatch.

**Alternatives considered.**
- **Homebrew LLVM for every native preset, with a custom `arm64-osx-llvm` triplet chainloading it so vcpkg builds ports with the same compiler.** One compiler everywhere, no ABI question, no `P2PGPU_FUZZ_ONLY`, and fuzzing available from the default preset exactly as `WORKFLOW.md` §2 assumes. This is the tidier end state and is the revisit path if the split preset becomes annoying. Rejected for now: it forces a full rebuild of every port with a non-default compiler, adding a slower and less-trodden failure surface to the very first build, in exchange for convenience rather than correctness.
- **Drop fuzzing on macOS; fuzz only in Linux CI.** Rejected outright. R-H names exactly this failure mode — R11 mitigations documented but the corpus empty. Fuzzing must be runnable on the machine where the protocol is being written, or step 1.5 will not actually happen.
- **Force Homebrew Clang onto the macOS SDK's libc++** with `-nostdinc++ -isystem $(xcrun --show-sdk-path)/usr/include/c++/v1`. Works, but silently couples the build to SDK layout and would break confusingly on an Xcode update.

**Consequences.**
`WORKFLOW.md` §2's "touched the protocol? run the fuzzer" becomes a second build directory rather than a flag in the default one — `CLAUDE.md`'s commands table is updated accordingly. Two compilers in the repo is a real cost, partly offset by GCC/MSVC needing to build clean anyway (`CONVENTIONS.md` §1). The `P2PGPU_FUZZ_ONLY` constraint on `p2pgpu-protocol` is a genuine design pressure, and a healthy one: it keeps the trust boundary free of policy, which `ARCHITECTURE.md` §4 already requires ("No I/O, no policy").

---

<!-- APPEND NEW ENTRIES BELOW THIS LINE. Do not modify entries above except to add a SUPERSEDED marker (DL2). -->

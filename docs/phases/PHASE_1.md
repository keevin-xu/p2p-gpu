# Phase 1 — Protocol & Single-Worker Pipeline

**Objective:** one worker completes a real job end to end, driven by the coordinator, over the FlatBuffers protocol. Both worker targets come up together. Simplest possible workload.

**Why now:** the protocol shape must be right before anything is built on it. G1 is the most important gate in the project.

**Scope note (D-0012):** because `worker-native` and `worker-browser` are thin wrappers over one `worker-core`, both ship in this phase. The native binary is what makes headless kernel testing in CI possible from here on.

**Entry criteria:** G0 approved.

---

## Steps

### Protocol

- [ ] **1.1 — Write the schema.**
  `protocol/p2pgpu.fbs` — all types from `PROTOCOL.md` §2: `Uuid`, `Hash32`, `DType`, `DeterminismClass` union, `OutputSpec`, `AccumulationSpec`, `TaskEnvelope`, `GpuLimits`, `WorkerCapabilities`, `TaskStats`, and the full `Body` union with every message variant.
  Include variants not used until later phases — additive now is free, and FlatBuffers' additive compatibility means adding later is cheap too. Reordering is not.

- [ ] **1.2 — Frame header codec.**
  The 12-byte envelope from `PROTOCOL.md` §1. `ParseHeader(std::span<const std::byte>) -> std::optional<Header>`. **No pointer arithmetic** (R11, `CONVENTIONS.md` §2).

- [ ] **1.3 — The Verifier path.**
  Implement the exact sequence from `PROTOCOL.md` §1 as the single sanctioned bytes→typed entry point. Bounded `kMaxVerifyDepth` / `kMaxVerifyTables`. Everything else in the codebase calls this and nothing else.

- [ ] **1.4 — Invariants.**
  All ten from `PROTOCOL.md` §4 as functions in `src/protocol/`. Unit-test each, **including every failure case** — these are security boundaries, not tidiness.

- [ ] **1.5 — Fuzz harness (do this now, not later).**
  `fuzz/fuzz_protocol.cpp` over `ParseHeader` + Verifier + invariants. Seed the corpus with valid frames from your own encoder plus hand-made malformed ones: truncated header, `fb_len` exceeding the frame, `fb_len` at `UINT32_MAX`, valid header with garbage FlatBuffer, deeply nested tables.
  Run it. **Any crash gets its input committed as a regression seed** (`CONVENTIONS.md` §7 T4).
  This is the step that makes R11 real rather than aspirational.

- [ ] **1.6 — Strong ID types.**
  `WorkerId` / `TaskId` / `JobId` as distinct types over the schema `Uuid`, mutually non-convertible.

### Kernel — Workload B

- [ ] **1.7 — Write the R5 calculation FIRST.**
  Before any WGSL. `brute_search_v1` in `kernels/manifest.toml` plus a `DECISIONS.md` entry. It clears 10⁶ by orders of magnitude — this is the easy case, and doing the ritual on the easy case establishes the habit.

- [ ] **1.8 — Implement `brute_search.wgsl`.**
  Search a keyspace range for inputs whose hash matches a target prefix. Integer-only (`DeterminismClass::Exact`). Params carry `(start_unit, unit_count, target, seed)` per K1. Counter-based RNG per K2. Fixed 32-byte output.

- [ ] **1.9 — Params layout parity.**
  C++ struct mirroring the WGSL params struct field-for-field, with `static_assert(sizeof(...) == ...)` to catch drift (`CONVENTIONS.md` §5).

- [ ] **1.10 — Manifest entry.**
  All fields from `KERNELS.md` §2, including `flop_per_unit` and the R5 numbers.

### Coordinator

- [ ] **1.11 — uWebSockets server skeleton.**
  WebSocket endpoint, HTTP health endpoint, `spdlog` with the correlation fields from `CONVENTIONS.md` §6, CLI11 config.

- [ ] **1.12 — Kernel registry.**
  Parse `kernels/manifest.toml` at startup, serve WGSL by kernel id over HTTP, include descriptors in `Welcome`.

- [ ] **1.13 — Task lifecycle state machine.**
  `enum class TaskState` + `switch` under `-Werror=switch`, **no `default:` arm** (`ARCHITECTURE.md` §5). Unit-test every legal transition; assert every illegal one fails.

- [ ] **1.14 — In-memory job + task queue.**
  Create a job, decompose into a fixed task count (no adaptive sizing yet — hardcode `work_units`), grant on `LeaseRequest`, accept on `ResultHeader`.

- [ ] **1.15 — Handshake.**
  `Hello` → version check → `Welcome`. Mismatched version ⇒ fatal `Error`. Test the mismatch path.

- [ ] **1.16 — Result ingestion.**
  Accept `ResultHeader` + trailing payload, verify BLAKE3 checksum, enforce one-in-flight-header. Checksum mismatch ⇒ requeue, **no reputation penalty**.

### worker-core (dual target)

- [ ] **1.17 — Platform seam interface.**
  `include/p2pgpu/worker/platform.hpp` declaring device acquisition, timing, and yield. Two implementations under `src/worker-core/platform/`: `gpu_native.cpp` / `gpu_wasm.cpp`, `timer_native.cpp` / `timer_wasm.cpp`.
  **Establish the review rule now:** an `#ifdef __EMSCRIPTEN__` outside `platform/` is a defect (R2).

- [ ] **1.18 — Transport client.**
  libdatachannel's WebSocket on native, datachannel-wasm in the browser — **same API, one `transport.cpp`, no seam needed**. This is the payoff of D-0008; verify it holds here.

- [ ] **1.19 — Kernel execution host.**
  Fetch WGSL by kernel id, compile, allocate buffers against **queried** limits (K4), execute in ≤250 ms chunks with yields (R4/K1), read back, populate every `TaskStats` field.

- [ ] **1.20 — Task loop.**
  Lease → resolve inputs → execute → report → renew. Portable; no platform conditionals.

- [ ] **1.21 — Device-loss handling.**
  Wire the Phase 0 recovery helper: release leases, re-acquire, re-register.

### The two thin wrappers

- [ ] **1.22 — `worker-native`.**
  `main()` + CLI11 args over `worker-core`. Should be well under 100 lines. If it is growing, logic is leaking out of `worker-core`.

- [ ] **1.23 — `worker-browser` + opt-in UI (R7).**
  Emscripten entry point. `web/index.html` + `web/ui.js` for the DOM surface: explicit start button, visible contributing indicator, throttle slider, instant stop. `src/worker-browser/ui_bridge.cpp` connects them via `EM_JS`.
  **Build this now, not as polish later** — it is a hard rule and it shapes the worker's control flow. Keep `ui.js` logic-free (R1, R2).

- [ ] **1.24 — Run the task loop off the browser main thread.**
  Background tabs throttle the main thread. Use a Web Worker (`-sPROXY_TO_PTHREAD` or an explicit worker) and never depend on `requestAnimationFrame`.

### Verification

- [ ] **1.25 — CPU reference implementation.**
  Single-threaded C++ brute-force search. Ground truth for all later kernel testing.

- [ ] **1.26 — End-to-end run, native worker.**
  Coordinator starts, `worker-native` joins, 1000 tasks complete, results match the reference exactly.

- [ ] **1.27 — End-to-end run, browser worker.**
  Same job, same kernel, same protocol, browser target. Results identical to 1.26.

- [ ] **1.28 — Mixed run.**
  Both worker types on the same job simultaneously. The coordinator should not be able to tell them apart structurally.

---

## Deliverables

- `protocol/p2pgpu.fbs` complete, generating headers at build time
- Verifier path + all ten invariants + fuzz harness with committed corpus
- `brute_search_v1` kernel + manifest + R5 documentation
- Coordinator serving jobs with a tested state machine
- `worker-core` compiling to both targets from one source
- Both worker binaries completing the same job
- Opt-in UI functional

## Exit criteria

1. 1000-task job completes on native, on browser, and mixed
2. Results match the CPU reference exactly (`Exact` class)
3. `flatc` generates from schema; no generated code in the source tree
4. State machine has exhaustive transition tests
5. Fuzzer runs clean for ≥10 minutes; corpus committed
6. ASan/UBSan clean across the whole test suite
7. No `#ifdef __EMSCRIPTEN__` outside `src/worker-core/platform/`
8. R5 calculation documented in manifest + `DECISIONS.md`
9. `PROTOCOL.md` matches the schema exactly
10. Opt-in, indicator, throttle, and stop all functional

---

## → HUMAN GATE G1

Produce for review:
- Recorded end-to-end runs on both targets
- `PROTOCOL.md` diffed against `protocol/p2pgpu.fbs`
- Fuzzing report: exec count, coverage, corpus size, crashes found and fixed
- The R5 calculation
- Proof that no `#ifdef` leaked outside the platform seam

**The question being answered:** is the protocol shape right before everything is built on it? Protocol changes after Phase 4 are expensive (`RISKS.md` R-B).

Scrutinize specifically: does `TaskEnvelope` carry everything the path tracer will need in Phase 5? Is `AccumulationSpec` sufficient? Is `TaskStats` rich enough for E2? Is the Verifier path genuinely the only route from bytes to fields?

**Stop here.**

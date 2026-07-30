# Code Conventions

**C++20 everywhere.** Compilers: Clang (primary, needed for libFuzzer), GCC and MSVC must also build clean.

---

## 1. Error model

**No exceptions across the transport or task boundary.** A hostile worker must not be able to steer control flow through your unwind paths.

- Fallible operations return `std::expected<T, Error>` (C++23 where available) or a project `Result<T>` alias. `Error` is a struct with an `ErrorCode` and a message.
- Parsing, validation, and anything touching network bytes: **always** `std::expected`, never throw.
- Exceptions are permitted only in startup/config code before serving begins, and in tests.
- `-fno-exceptions` is **not** set — vcpkg dependencies use them — but our code does not throw.

**Never** `assert()` on attacker-controlled input. Assertions are for internal invariants only, and they compile out in release. Untrusted input gets a real check that returns an error in every build.

`std::optional` for "absent is normal." `std::expected` for "this failed and the caller needs to know why."

---

## 2. Memory safety at the trust boundary (R11)

This section is load-bearing. It is the reason C++ is defensible for this project.

**In any file that touches network bytes — `src/protocol/`, transport, asset reassembly:**

| Banned | Use instead |
|---|---|
| Raw pointer arithmetic | `std::span::subspan` |
| `memcpy` | `std::ranges::copy` into a sized destination |
| C arrays, `new`/`delete` | `std::array`, `std::vector`, `std::span` |
| `reinterpret_cast` on input | FlatBuffers accessors after `Verifier` |
| `.at()`-less indexing on input-derived indices | bounds-checked access, or validate the index first |
| Unchecked integer math on lengths/offsets | checked arithmetic; overflow ⇒ reject |

**Every deserialization goes through `flatbuffers::Verifier`** following the exact sequence in `PROTOCOL.md` §1. There is no "internal" path that skips it.

**Validate before allocating.** FlatBuffers stops memory corruption; it does not stop an attacker declaring a 4 GB payload and OOM-ing you. Every length is checked against its `kMax*` constant first.

Build flags, always on in release: `-D_GLIBCXX_ASSERTIONS -D_FORTIFY_SOURCE=2 -fstack-protector-strong`. On MSVC: `/GS /sdl`.

---

## 3. Types and style

- **Newtype the IDs.** `WorkerId`, `TaskId`, `JobId` as distinct strong types over the schema `Uuid`, not interchangeable aliases. Mixing them up is a bug class worth designing out.
- **State machines are `enum class` + `switch` under `-Werror=switch`.** Never write a `default:` arm on a state switch — the compiler error when you add a state is the entire point. Use `std::variant` + `std::visit` (with no generic fallback in the overload set) only when states carry differing payloads.
- `const` by default. `constexpr` where it costs nothing.
- Pass by `std::string_view` / `std::span` for non-owning; `const&` for owning types; value for cheap types.
- `[[nodiscard]]` on every function returning `Result`/`expected`/`optional`.
- No `using namespace` at namespace scope in headers.
- Prefer free functions over member functions when they don't need private state.
- RAII for everything. No manual cleanup paths.

**Formatting:** `.clang-format` at repo root, enforced in CI. Do not hand-format.
**Linting:** `.clang-tidy`, warnings as errors. Suppressions require an inline comment explaining why.

---

## 4. Concurrency

- Coordinator: single uWebSockets event loop for I/O, a thread pool for CPU-bound validation work. Do not block the loop.
- Worker: single-threaded task loop. On WASM the "thread" is the Emscripten main loop — **never busy-wait, never block**; yield between dispatches (R4).
- Shared state: `std::shared_mutex` at this scale. Do not reach for lock-free structures without a measurement showing the lock matters.
- Periodic work (lease expiry sweep) is a timer on the loop, **not** one timer per task.
- TSan runs in CI on the coordinator test suite.

---

## 5. WGSL

See `KERNELS.md` for authoring rules K1–K8. Style points:

- One kernel per file, named after its `kernel_id` minus the version suffix.
- Params struct at the top, mirroring the C++ layout struct **field for field, in order**, with a comment linking to the C++ definition. Add a `static_assert` on `sizeof` in C++ to catch drift.
- Explicit `@workgroup_size(...)` matching the manifest — never rely on a default.
- Comment every barrier with what it protects.
- Bounds-check against the actual dispatch size; do not assume the grid divides evenly.

---

## 6. Logging

**Structured, `spdlog`, JSON sink in production and pretty in dev.**

Every log line concerning a task carries:

```
worker_id, task_id, job_id, phase
```

Not optional. Failures cross a process boundary and two build targets; consistent correlation IDs are what make them traceable. The WASM worker logs the same field names, routed through `emscripten_console_*` to the browser console.

| Level | Use |
|---|---|
| `error` | The system did something wrong. Requires action. |
| `warn` | A worker did something wrong, or a recoverable fault fired. Lease expiry is `warn`. |
| `info` | Lifecycle: job start/complete, worker join/leave, gate-relevant events. |
| `debug` | Per-task transitions. |
| `trace` | Per-dispatch. Off by default; high volume. |

**A rejected result is always `warn` with the full comparison detail** — the primary diagnostic for the R6 determinism-class trap.
**A Verifier rejection is always `warn` with the source, frame length, and first 32 bytes hex** — the primary diagnostic for protocol bugs and the first signal of an actual attack.

---

## 7. Testing

Five tiers, all required.

### T1 — Unit (Catch2)
Pure logic: sizer math, reputation updates, tolerance comparison, state-machine transitions. Fast, no I/O. The task-lifecycle state machine gets exhaustive transition tests including every illegal transition.

### T2 — Integration (coordinator + mock workers, in-process)
Spin up the coordinator and N mock workers in one test. The workhorse tier. Every failure path in `ARCHITECTURE.md` §6 has a test here: lease expiry · device loss · tab hidden · speculative re-issue · coordinator restart recovery · duplicate submit · lease-not-held · malformed frame.

### T3 — Kernel (via `worker-native`, headless, in CI)
Golden, chunk-invariance, cross-implementation, and limits tests per `KERNELS.md` §5.

### T4 — Fuzz (libFuzzer)
`fuzz/fuzz_protocol.cpp` on the frame parser. `fuzz/fuzz_asset.cpp` on chunk reassembly. Corpus committed under `fuzz/corpus/`. CI runs a bounded time budget per commit; run longer campaigns manually before gates.

**Any crash found by fuzzing gets its input committed to the corpus as a regression seed.**

### T5 — Chaos / experiment
Not pass/fail — these **produce the charts** in `EVALUATION.md`. Live under `src/mock-worker/experiments/`, reproducible from a seed, emit CSV. Build artifacts, not tests; run on demand.

**Rule:** a bug fix gets a test that fails before the fix. Especially for failure paths — those are the product here, not the edge cases.

---

## 8. Sanitizers

| Build | Sanitizers | When |
|---|---|---|
| `native-debug` | ASan + UBSan | default local dev, every CI run |
| `native-tsan` | TSan | CI, coordinator tests |
| `native-release` | none, hardening flags on | benchmarks and experiments only |

**Never run experiments under ASan** — the ~2× slowdown corrupts every timing measurement in `EVALUATION.md`. Correctness under sanitizers, performance under release, and never confuse the two.

---

## 9. Determinism in tests

Every experiment and chaos test takes an explicit seed and is reproducible from it. Record the seed in the emitted CSV header. A chart that cannot be regenerated is not evidence.

---

## 10. Dependencies

Adding a dependency requires a `DECISIONS.md` entry and human sign-off (DL5). Pin exact versions in `vcpkg.json` — no floating ranges. The approved baseline:

| Dep | Purpose |
|---|---|
| `flatbuffers` | Wire format + Verifier |
| `libdatachannel` | WebRTC + WebSocket, native |
| `datachannel-wasm` | Same API, browser (FetchContent) |
| `uwebsockets` | Coordinator HTTP / WS / SSE |
| `sqlite3` | Coordinator persistence |
| `spdlog` | Logging |
| `catch2` | Testing |
| `cli11` | Argument parsing |
| `blake3` | Content hashing |
| `stduuid` | UUID handling |
| `wgpu-native` | WebGPU on native (FetchContent, prebuilt) |

Explicitly **not** approved without discussion: Boost, any ORM, any actor framework, any additional datastore, any JSON library on the hot path, any hand-rolled crypto.

---

## 11. Repo hygiene

- Generated `flatc` headers live in the **build tree only**. Never committed, never edited (R3).
- No binary fixtures over 1 MB, except fuzz corpus seeds (which are tiny by nature). Scene assets are generated by a script.
- `.env` never committed. Config via CLI11 flags with sane defaults; env vars for secrets only.
- Commit messages per `WORKFLOW.md` §5.

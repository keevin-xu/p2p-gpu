# Tech Stack — quick reference

Personal cheat sheet. Everything here is decided; rationale links to `DECISIONS.md`.

---

## Languages

| Language | Where | Notes |
|---|---|---|
| **C++20** | Everything — coordinator, worker-core, native worker, browser worker, mock harness | Single language across all targets (D-0008) |
| **WGSL** | `kernels/` | GPU compute shaders. Shared verbatim by all worker targets |
| **FlatBuffers IDL** | `protocol/p2pgpu.fbs` | Schema-driven wire format; generates C++ headers |
| **JavaScript** | `web/ui.js`, `web/dashboard/dashboard.js` | ~50 lines total. DOM glue only — WASM can't touch the DOM. **No logic permitted** |
| **CMake** | Build | |

No TypeScript. No Rust. No Python in the shipped system.

---

## Build & tooling

| Tool | Purpose |
|---|---|
| **CMake ≥3.25** + `CMakePresets.json` | Build system. Presets: `native-debug` (ASan+UBSan), `native-tsan`, `native-release`, `wasm` |
| **vcpkg** (manifest mode) | Dependency management, pinned versions, reproducible across macOS / Windows / Linux |
| **Ninja** | Generator |
| **Emscripten SDK** | C++ → WebAssembly for the browser worker |
| **flatc** | Schema → C++ headers, runs as a build step into the build tree |
| **clang-format / clang-tidy** | Style + static analysis, warnings-as-errors |
| **ASan / UBSan / TSan** | Sanitizers, on by default in dev and CI |
| **libFuzzer** | Fuzzing the protocol parser (Clang only) |

Clang is the primary compiler (libFuzzer needs it); GCC and MSVC must also build clean.

---

## Libraries

| Library | Purpose | Why this one |
|---|---|---|
| **FlatBuffers** | Wire format | Built-in `Verifier` designed for untrusted buffers; zero-copy; schema is the single source of truth (D-0009) |
| **libdatachannel** | WebRTC DataChannel + WebSocket, native | Standalone C++17 WebRTC without importing Google's full stack |
| **datachannel-wasm** | Same, browser | **Same API as libdatachannel** — this is what makes the single-language architecture possible (D-0008) |
| **uWebSockets** | Coordinator HTTP + WebSocket + SSE | Fast, one event loop, does all three |
| **wgpu-native** | WebGPU, native | Implements the standard `webgpu.h`; avoids Dawn's depot_tools/GN build |
| **emdawnwebgpu** | WebGPU, browser | Emscripten port (`--use-port=emdawnwebgpu`). Dawn's `webgpu.h`; the old `-sUSE_WEBGPU` bindings were removed in Emscripten 6 and the revisions are incompatible (D-0014) |
| **SQLite** | Coordinator persistence | Single file, zero ops. Not Postgres, not Redis (D-0006) |
| **spdlog** | Structured logging | JSON sink in prod, pretty in dev |
| **Catch2 v3** | Testing | |
| **CLI11** | Argument parsing | Header-only |
| **BLAKE3** | Content hashing | Asset addressing + payload checksums |
| **stduuid** | UUID handling | |

Everything via vcpkg except `wgpu-native` and `datachannel-wasm`, which come through `FetchContent`.

---

## Build targets

| Target | Type | Platform | What it is |
|---|---|---|---|
| `p2pgpu-protocol` | static lib | both | Generated schema headers, framing, Verifier wrappers, invariants |
| `p2pgpu-worker-core` | static lib | **both** | Task loop, kernel host, transport. Compiled twice from identical source |
| `coordinator` | executable | native | The only decision-maker |
| `worker-native` | executable | native | Thin `main()` over worker-core |
| `worker-browser` | `.wasm` + `.js` | wasm | Thin Emscripten entry over worker-core |
| `mock-worker` | executable | native | Chaos harness, no GPU |
| `tests` | executable | native | Catch2 |
| `fuzz_protocol` | executable | native/clang | libFuzzer |

The dual-target `worker-core` is the defining structural property. Platform differences live only in `src/worker-core/platform/`.

---

## Protocols & formats

| | |
|---|---|
| Control plane | WebSocket, binary frames, FlatBuffers `Envelope` |
| Data plane | WebRTC DataChannel, 16 KiB chunks, content-addressed |
| NAT traversal | ICE + STUN, TURN relay fallback (Cloudflare or coturn) |
| Metrics → dashboard | Server-Sent Events |
| Asset fallback | HTTP `GET /asset/{hash}` |
| Kernel manifest | TOML |
| Experiment output | CSV |

---

## Deployment

| | |
|---|---|
| Coordinator | Static binary in a slim container — Fly.io or Railway, persistent volume for SQLite |
| Worker page | Same origin, **COOP/COEP headers required** |
| TURN | Cloudflare TURN or self-hosted coturn |
| Load-test fleet | Cheap cloud VMs — CPU-only run `mock-worker`, GPU run `worker-native` |

---

## Deliberately absent

Worth knowing so you don't reach for them, and so you can answer "why not X":

- **Postgres / Redis** — SQLite is sufficient; zero operational surface
- **Boost** — not needed; adds build weight
- **Any WebRTC wrapper** — use libdatachannel directly; understanding ICE/STUN/TURN is part of the point
- **A DHT** for peer discovery — coordinator-brokered peer lists are ~10× less code at this scale
- **WebTransport** — client-server only, no P2P; broad implementation not expected until 2027
- **Hand-rolled binary parsing** — would reintroduce the exact risk C++ was chosen despite (R11)
- **Exceptions across the transport boundary** — `std::expected` instead
- **Any frontend framework** — the dashboard is static HTML + a little JS
- **Blockchain / tokens / payments** — out of scope, and saying otherwise damages credibility

---

## Résumé / interview phrasing

**One-liner:**
> Distributed volunteer-compute grid in C++20 running GPU workloads across browser tabs via WebGPU, with a single `worker-core` compiling to both native and WebAssembly.

**Keywords this project legitimately supports:**
C++20 · WebGPU · WGSL · WebAssembly / Emscripten · WebRTC · distributed systems · lease-based fault tolerance · Byzantine fault tolerance · speculative execution · scheduling · FlatBuffers · CMake · fuzzing / libFuzzer · ASan/UBSan · SQLite · systems performance measurement

**If asked "why C++ and not Rust":**
Single language across every component including the browser, which Rust can't do cleanly because `web-sys` WebRTC bindings are painful — libdatachannel/datachannel-wasm give C++ one API on both targets. The cost is memory safety at the trust boundary, which is handled explicitly: schema-driven deserialization behind a verifier, no raw pointer arithmetic in boundary code, sanitizers and fuzzing in CI with a committed corpus. Rust gives that free; here it's earned and therefore demonstrable.

Full reasoning: `DECISIONS.md` D-0008 and D-0010.

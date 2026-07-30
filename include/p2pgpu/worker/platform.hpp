#pragma once
//
// THE PLATFORM SEAM (rule R2).
//
// worker-core is compiled twice — native and WebAssembly — from identical
// source. This header declares the only interface that differs between them.
// Two implementations satisfy it:
//
//     src/worker-core/platform/gpu_native.cpp    #if !__EMSCRIPTEN__
//     src/worker-core/platform/gpu_wasm.cpp      #if  __EMSCRIPTEN__
//     src/worker-core/platform/timer_native.cpp
//     src/worker-core/platform/timer_wasm.cpp
//
// CMake picks the right pair per target; nothing else in worker-core knows
// which it got.
//
// ── REVIEW RULE ──────────────────────────────────────────────────────────
// A `#ifdef __EMSCRIPTEN__` outside src/worker-core/platform/ is a defect.
// If a portable file appears to need one, THIS INTERFACE IS WRONG — widen it
// here rather than forking the caller. Forking is exactly what R2 exists to
// prevent, and it is how the two targets silently drift apart.
//
// Note transport is NOT in this seam. libdatachannel (native) and
// datachannel-wasm (browser) expose the same API, so transport.cpp is
// genuinely portable. That is the property the whole single-language
// architecture rests on — see docs/DECISIONS.md D-0008.
//
// Implement in Phase 1, step 1.17.

#include <chrono>
#include <cstdint>
#include <functional>
#include <string_view>

namespace p2pgpu::worker::platform {

/// Acquire a GPU device. Returns false if unavailable — a blocklisted driver
/// or unsupported browser is a *capability*, not a crash (docs/RISKS.md §1).
/// Callers report it and decline gracefully.
[[nodiscard]] bool AcquireDevice();

/// Register a device-loss callback. Windows TDR kills work blocking ~2 s and
/// resets the driver (rule R4); the browser surfaces this as a lost device.
/// The handler must release leases, re-acquire, and re-register.
void OnDeviceLost(std::function<void()> handler);

/// Yield control so the host stays responsive between dispatches (R4/K1).
/// Native: a no-op or short sleep. WASM: returns to the Emscripten main loop.
/// NEVER busy-wait — on WASM that freezes the tab.
void Yield();

/// Monotonic clock for local elapsed-time measurement only.
///
/// This is NEVER used to decide lease expiry. Lease expiry is decided solely
/// by the coordinator on its own clock; the worker only measures how much of
/// its granted window it has consumed (docs/PROTOCOL.md §5).
[[nodiscard]] std::chrono::steady_clock::time_point Now();

/// Structured log line, routed to spdlog natively and to the browser console
/// under WASM. Field names are identical on both so correlation IDs work
/// across targets (docs/CONVENTIONS.md §6).
void Log(std::string_view level, std::string_view message);

}  // namespace p2pgpu::worker::platform

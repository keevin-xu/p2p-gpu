#pragma once
//
// Device-loss recovery (step 0.14). PORTABLE — no seam widening needed, since
// `wgpuDeviceDestroy` and the device-lost callback exist identically in both
// webgpu.h implementations (D-0016).
//
// ── WHY THIS EXISTS ──────────────────────────────────────────────────────
// A GPU device can be taken away at any moment and it is NOT an exceptional
// case:
//   · Windows TDR kills GPU work blocking ~2 s and resets the driver (R4).
//     The browser surfaces that as a lost device.
//   · The OS or browser may reclaim the GPU under memory pressure.
//   · Driver updates, external GPU unplug, laptop sleep.
//
// A worker that cannot survive this is useless on exactly the machines we most
// want to reach. So recovery is core machinery, not error handling bolted on.
//
// ── ORDER MATTERS ────────────────────────────────────────────────────────
// On loss the sequence is: release leases FIRST, then re-acquire, then
// re-register. Getting it backwards means the coordinator believes we still
// hold tasks we can no longer compute, and those tasks stall until their lease
// expires — turning a 200 ms hiccup into a lease-duration outage.
//
// Accumulated-but-unuploaded work is lost with the device. The upload cadence
// (`AccumulationSpec`) is what bounds that loss (docs/RISKS.md §2).

#include <cstdint>
#include <functional>

#include "p2pgpu/worker/platform.hpp"

namespace p2pgpu::worker {

/// Owns a GPU device and keeps it alive across losses.
class DeviceSession {
public:
    DeviceSession() = default;
    ~DeviceSession();

    DeviceSession(const DeviceSession&) = delete;
    DeviceSession& operator=(const DeviceSession&) = delete;

    /// Called when the device is lost, BEFORE re-acquisition. Release every
    /// held lease here — the coordinator must not keep waiting on work this
    /// worker can no longer do.
    void OnLost(std::function<void()> handler) { on_lost_ = std::move(handler); }

    /// Called after a device has been successfully re-acquired. Re-register and
    /// resume requesting work here.
    void OnReady(std::function<void()> handler) { on_ready_ = std::move(handler); }

    /// Acquire the initial device. Returns false if none is available — which
    /// is a capability, not a crash (docs/RISKS.md §1).
    [[nodiscard]] bool Start();

    /// Release everything. Idempotent.
    void Stop();

    [[nodiscard]] const platform::GpuContext& context() const noexcept { return ctx_; }
    [[nodiscard]] bool healthy() const noexcept { return healthy_ && ctx_.valid(); }
    [[nodiscard]] std::uint32_t recovery_count() const noexcept { return recoveries_; }

    /// Release the dead device and acquire a fresh one. Bounded retries — a
    /// machine whose GPU is genuinely gone must stop trying and report that,
    /// rather than spin forever pretending it can contribute.
    [[nodiscard]] bool Recover();

    /// TEST HOOK — destroy the device so it is genuinely unusable, WITHOUT
    /// auto-recovering, so a test can assert that work now fails before proving
    /// that recovery fixes it.
    ///
    /// Honest scope: this exercises OUR recovery machinery. It cannot make a
    /// driver spontaneously drop the device — that requires real Windows TDR
    /// (steps 0.16 / 4.10), which is untestable on macOS.
    void ForceLossForTest();

    /// TEST HOOK — destroy the device AND drive the full production loss path,
    /// exactly as a genuine driver loss would: on_lost (release leases) →
    /// re-acquire → on_ready (re-register).
    ///
    /// `ForceLossForTest` deliberately skips that path so a test can assert the
    /// device is really dead in between. This one exercises the ordering, which
    /// is the part most likely to be wrong in production: releasing leases only
    /// *after* re-acquiring would leave the coordinator waiting on tasks this
    /// worker can no longer compute.
    ///
    /// Still cannot make a driver spontaneously drop the device — real TDR is
    /// steps 0.16 / 4.10, and is untestable on macOS.
    void SimulateGenuineLossForTest();

private:
    void HandleLost();

    platform::GpuContext ctx_{};
    std::function<void()> on_lost_;
    std::function<void()> on_ready_;
    std::uint32_t recoveries_ = 0;
    bool healthy_ = false;
};

}  // namespace p2pgpu::worker

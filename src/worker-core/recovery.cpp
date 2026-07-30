// Device-loss recovery (step 0.14). Portable; no #ifdef (R2).
// See recovery.hpp for why this is core machinery rather than error handling.

#include "p2pgpu/worker/recovery.hpp"

#include <string>

namespace p2pgpu::worker {
namespace {

using platform::Log;

/// Bounded, deliberately. A machine whose GPU is genuinely gone (driver
/// uninstalled, eGPU unplugged) must give up and say so — a worker that retries
/// forever looks alive to the coordinator while contributing nothing, which is
/// worse than one that disconnects cleanly.
constexpr int kMaxRecoveryAttempts = 3;

}  // namespace

DeviceSession::~DeviceSession() {
    Stop();
}

bool DeviceSession::Start() {
    if (!platform::AcquireDevice(ctx_)) {
        Log("warn", "device session: no GPU available at start");
        healthy_ = false;
        return false;
    }

    // Wire the loss callback. The seam only invokes this for GENUINE losses —
    // an intentional wgpuDeviceRelease during our own shutdown reports reason
    // `Destroyed` and is filtered out there, so this handler never fires on a
    // normal teardown (found in step 0.6).
    platform::OnDeviceLost([this] { HandleLost(); });

    healthy_ = true;
    return true;
}

void DeviceSession::Stop() {
    platform::OnDeviceLost(nullptr);
    platform::ReleaseDevice(ctx_);
    healthy_ = false;
}

void DeviceSession::HandleLost() {
    if (!healthy_) {
        // Already handling a loss. Re-entering here would release leases twice
        // and could recurse if re-acquisition itself fails loudly.
        return;
    }
    healthy_ = false;
    Log("warn", "device lost — releasing leases and re-acquiring");

    // ORDER IS LOAD-BEARING: leases go back to the coordinator BEFORE we spend
    // time re-acquiring. Otherwise those tasks sit stalled until their lease
    // expires, turning a brief hiccup into a lease-duration outage.
    if (on_lost_) {
        on_lost_();
    }

    if (!Recover()) {
        Log("error", "device recovery failed — this worker can no longer contribute");
    }
}

bool DeviceSession::Recover() {
    platform::ReleaseDevice(ctx_);

    for (int attempt = 1; attempt <= kMaxRecoveryAttempts; ++attempt) {
        if (platform::AcquireDevice(ctx_)) {
            platform::OnDeviceLost([this] { HandleLost(); });
            healthy_ = true;
            ++recoveries_;
            Log("info", "device re-acquired on attempt " + std::to_string(attempt) +
                            " (recovery #" + std::to_string(recoveries_) + ")");
            if (on_ready_) {
                on_ready_();
            }
            return true;
        }

        Log("warn", "device re-acquisition attempt " + std::to_string(attempt) +
                        " of " + std::to_string(kMaxRecoveryAttempts) + " failed");

        // Yield between attempts rather than sleeping. A driver mid-reset needs
        // a moment, and on the browser this is the ONLY safe way to wait — a
        // sleep would block the event loop that has to deliver the new device.
        platform::Yield();
    }

    healthy_ = false;
    return false;
}

void DeviceSession::ForceLossForTest() {
    if (ctx_.device == nullptr) {
        return;
    }
    Log("warn", "forcing device loss (test)");

    // Genuinely destroys the device: subsequent GPU work on it must fail. This
    // is stronger than simulating a callback, because it proves the recovery
    // produced a *working* device rather than merely running some code.
    wgpuDeviceDestroy(ctx_.device);

    // Flag it synchronously. The device-lost callback is asynchronous and will
    // not have run yet, and anything that submits work before it does would
    // abort the process (D-0022).
    platform::MarkDeviceLost();
    healthy_ = false;
}

void DeviceSession::SimulateGenuineLossForTest() {
    if (ctx_.device == nullptr) {
        return;
    }
    Log("warn", "simulating genuine device loss (test)");
    wgpuDeviceDestroy(ctx_.device);
    platform::MarkDeviceLost();

    // Drive the same entry point the seam's callback would. HandleLost expects
    // to be called while still nominally healthy, since that is how a real loss
    // arrives — so do NOT clear the flag first.
    HandleLost();
}

}  // namespace p2pgpu::worker

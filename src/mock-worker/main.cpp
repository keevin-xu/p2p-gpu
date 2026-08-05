// The chaos harness — most of this project's evidence comes from here.
//
// Full protocol, no GPU. E1 (scaling), E3 (fault tolerance), E4 (Byzantine),
// and E5 (stragglers) are all produced by this binary. It turns week-long
// fleet experiments into 30-second runs, so it is the fast path, not a detour.
//
// Usage:
//   mock-worker --count 200 --coordinator ws://localhost:8080/ws \
//               --chaos byzantine_10pct --seed 42
//
// ── WHAT THIS BINARY IS AND IS NOT EVIDENCE FOR ──────────────────────────
// Honest workers compute the REAL answer on the CPU and then sleep for their
// simulated duration (D-0042), so replication has something true to compare
// against and a liar is a real deviation rather than a different flavour of
// fiction. But simulated speed is decoupled from real compute, and task sizes
// are bounded by what the CPU reference can do — so **nothing here is GPU
// throughput evidence.** These runs measure SCHEDULING.

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "profiles.hpp"
#include "virtual_worker.hpp"

int main(int argc, char** argv) {
    std::uint32_t count = 10;
    std::string url = "ws://localhost:8080/ws";
    std::string chaos = "default";
    std::uint64_t seed = 42;
    std::uint32_t run_seconds = 0;
    bool list_profiles = false;
    double nominal_ms = 600.0;
    double liar_fraction = -1.0;
    bool collude = false;
    std::uint32_t sleeper_after = 0;

    CLI::App app{"p2pgpu mock worker fleet"};
    app.add_option("--liar-fraction", liar_fraction,
                   "override the profile's share of lying workers (E4 sweep)");
    app.add_option("--sleeper-after", sleeper_after,
                   "liars behave for N tasks first, then defect (3.18)");
    app.add_flag("--collude", collude,
                 "liars return IDENTICAL wrong answers, defeating naive quorum (3.16)");
    app.add_option("--ms-per-mega-unit", nominal_ms,
                   "simulated device speed; see virtual_worker.cpp for the "
                   "fleet-size constraint")
        ->capture_default_str();
    app.add_option("--count", count, "virtual workers in this process")
        ->capture_default_str();
    app.add_option("--coordinator", url, "coordinator WebSocket URL")
        ->capture_default_str();
    app.add_option("--chaos", chaos, "chaos profile name")->capture_default_str();
    app.add_option("--seed", seed, "run seed — same seed replays the same fleet")
        ->capture_default_str();
    app.add_option("--seconds", run_seconds, "stop after N seconds (0 = run until killed)")
        ->capture_default_str();
    app.add_flag("--list-profiles", list_profiles, "print the chaos profiles and exit");
    CLI11_PARSE(app, argc, argv);

    spdlog::set_pattern("[%Y-%m-%dT%H:%M:%S.%e%z] [%^%l%$] %v");

    if (list_profiles) {
        for (const auto* p : p2pgpu::mock::AllProfiles()) {
            std::printf("  %-18s %s\n", std::string(p->name).c_str(),
                        std::string(p->description).c_str());
        }
        return 0;
    }

    // REFUSE an unknown profile rather than falling back to `default`. An
    // experiment that silently ran the wrong fleet produces numbers that look
    // perfectly fine, which is the worst possible failure for a measurement
    // instrument.
    const auto* profile = p2pgpu::mock::FindProfile(chaos);
    if (profile == nullptr) {
        spdlog::error("unknown chaos profile: {}. --list-profiles to see them.", chaos);
        return 1;
    }

    p2pgpu::mock::SetNominalMsPerMegaUnit(nominal_ms);
    // Checked BEFORE any worker connects, so an oversubscribed run says so up
    // front rather than being diagnosed from a suspiciously low task count an
    // hour later.
    (void)p2pgpu::mock::WarnIfOversubscribed(count);

    spdlog::info("fleet: count={} profile={} seed={} coordinator={}", count,
                 std::string(profile->name), seed, url);
    spdlog::warn("mock workers simulate device speed; these runs measure SCHEDULING, "
                 "never GPU throughput");

    std::vector<std::unique_ptr<p2pgpu::mock::VirtualWorker>> fleet;
    fleet.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        p2pgpu::mock::Dice dice(seed, i);
        auto behaviors = profile->assign(i, count, dice);

        // E4 sweep overrides (3.14/3.16), applied AFTER the profile so the
        // profile stays the single description of a fleet's character and the
        // sweep only varies the one axis it is measuring.
        if (liar_fraction >= 0.0) {
            // By INDEX, not by coin flip, so "20%" is exactly 20% and the same
            // workers lie on every replay — a detection rate computed against
            // an approximate liar count is not a rate.
            const auto liars =
                static_cast<std::uint32_t>(std::llround(liar_fraction * count));
            behaviors.lies_probabilistically = (i < liars) ? 1.0 : 0.0;
        }
        if (sleeper_after > 0 && behaviors.lies_probabilistically > 0.0) {
            behaviors.honest_tasks_before_lying = sleeper_after;
        }
        if (collude && behaviors.lies_probabilistically > 0.0) {
            // One key for the whole fleet: every liar fabricates identically.
            behaviors.collusion_key = 0x5EEDC0111DE5ULL ^ seed;
        }
        fleet.push_back(std::make_unique<p2pgpu::mock::VirtualWorker>(
            i, url, behaviors, seed));
        fleet.back()->Start();
    }

    // ONE loop for the whole fleet — N workers, not N processes (step 2.2).
    // Poll() is non-blocking for exactly this reason: one slow worker must not
    // stall the other 199.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(run_seconds == 0 ? 86400 : run_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto& w : fleet) {
            w->Poll();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    p2pgpu::mock::WorkerStats total;
    for (auto& w : fleet) {
        const auto& s = w->stats();
        total.tasks_completed += s.tasks_completed;
        total.tasks_lied_about += s.tasks_lied_about;
        total.tasks_abandoned += s.tasks_abandoned;
        total.reconnects += s.reconnects;
        total.malformed_sent += s.malformed_sent;
        total.tasks_revoked += s.tasks_revoked;
        w->Stop();
    }
    spdlog::info("fleet done: completed={} lied={} abandoned={} reconnects={} "
                 "malformed={} revoked={}",
                 total.tasks_completed, total.tasks_lied_about, total.tasks_abandoned,
                 total.reconnects, total.malformed_sent, total.tasks_revoked);
    return 0;
}

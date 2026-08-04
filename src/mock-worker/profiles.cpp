// Named chaos profiles — step 2.4. See profiles.hpp.
//
//   default · heterogeneous · byzantine_10pct · flaky_network
//   mass_departure · malformed_frames

#include "profiles.hpp"

#include <algorithm>
#include <array>

namespace p2pgpu::mock {
namespace {

// ── default ──────────────────────────────────────────────────────────────
// Everyone honest, mild speed spread. The control group: anything interesting
// here is a bug in the coordinator or the harness, not a finding about chaos.
Behaviors Default(std::uint32_t, std::uint32_t, Dice& dice) {
    Behaviors b;
    b.slow_factor = std::clamp(dice.lognormal(0.3), 0.5, 3.0);
    return b;
}

// ── heterogeneous ────────────────────────────────────────────────────────
// The realistic fleet, and what E5 (stragglers) is measured on. Log-normal
// because that is the shape real volunteer fleets have: a few fast machines and
// a long tail of slow ones. sigma 1.0 gives roughly the 20x spread step 2.2
// asks for across a hundred workers.
//
// Everyone is still HONEST. Mixing dishonesty in here would make E5's straggler
// numbers inseparable from validation overhead.
Behaviors Heterogeneous(std::uint32_t, std::uint32_t, Dice& dice) {
    Behaviors b;
    b.slow_factor = std::clamp(dice.lognormal(1.0), 0.25, 25.0);
    return b;
}

// ── byzantine_10pct ──────────────────────────────────────────────────────
// THE E4 INSTRUMENT. Exactly 10% lie, chosen by INDEX rather than by coin flip,
// so "10%" is exact and the same workers lie on every replay.
//
// They lie on 30% of their tasks, not all. A worker that lies every time is
// caught by the first replica; the interesting adversary is intermittent,
// because that is what defeats "check a newcomer once, then trust it". Phase 3's
// reputation design has to survive this profile specifically.
Behaviors Byzantine10(std::uint32_t index, std::uint32_t fleet, Dice& dice) {
    Behaviors b;
    b.slow_factor = std::clamp(dice.lognormal(0.5), 0.4, 6.0);
    if (fleet > 0 && index < (fleet + 9) / 10) {
        b.lies_probabilistically = 0.3;
    }
    return b;
}

// ── flaky_network ────────────────────────────────────────────────────────
// Nobody malicious, nobody broken; the network is bad. Separating this from
// `byzantine_10pct` is the whole point — the coordinator must not confuse a
// peer on hotel wifi with an attacker, and step 3.11 keeps connection hygiene
// apart from task reputation precisely so it cannot.
Behaviors FlakyNetwork(std::uint32_t index, std::uint32_t, Dice& dice) {
    Behaviors b;
    b.slow_factor = std::clamp(dice.lognormal(0.6), 0.4, 8.0);
    b.high_latency_ms = static_cast<std::uint32_t>(50 + (dice.next() % 450));
    b.flaps = 0.15;
    if (index % 7 == 0) {
        // STALLED, not merely slow. `never_renews_lease` on its own cannot have
        // an effect: a worker that finishes in 16 ms renews nothing because it
        // never needs to, and no sane lease expires in that window. The flag
        // was set and inert until this was noticed.
        //
        // Composing two orthogonal behaviours is exactly how this is meant to
        // work — "hung worker" is `slow` plus `never_renews_lease`, not a third
        // behaviour that secretly means both.
        b.never_renews_lease = true;
        b.slow_factor = 60.0;
    }
    return b;
}

// ── mass_departure ───────────────────────────────────────────────────────
// Half the fleet leaves mid-task. E3's headline claim is **zero tasks lost**.
// Every departure must return to the queue with no reputation penalty (R8):
// absence is not malice, and the machines we most want to reach are exactly the
// ones most likely to close the tab.
Behaviors MassDeparture(std::uint32_t index, std::uint32_t fleet, Dice& dice) {
    Behaviors b;
    b.slow_factor = std::clamp(dice.lognormal(0.4), 0.5, 4.0);
    if (fleet > 0 && index >= fleet / 2) {
        b.dies_mid_task = 0.5;
    }
    return b;
}

// ── stragglers ───────────────────────────────────────────────────────────
// THE E5 INSTRUMENT. A fifth of the fleet reports a score 20x its real speed,
// so the coordinator sizes tasks for a machine that no longer exists.
//
// This profile exists because `heterogeneous` does NOT produce stragglers:
// adaptive sizing gives every worker a task scaled to its own measured speed,
// so a slow-but-honest machine finishes in the same ~2 s as a fast one. Measured
// — a 12-worker heterogeneous run completed 1108 tasks with speculation never
// firing once.
//
// What speculation actually insures against is a worker whose speed CHANGES
// after the grant: another app starts, the laptop unplugs, a phone throttles.
// That is what this models, and E5 measures what the window costs before the
// EWMA correction catches up.
Behaviors Stragglers(std::uint32_t index, std::uint32_t fleet, Dice& dice) {
    Behaviors b;
    b.slow_factor = std::clamp(dice.lognormal(0.5), 0.5, 4.0);
    if (fleet > 0 && index < (fleet + 4) / 5) {
        b.score_inflation = 20.0;
    }
    return b;
}

// ── malformed_frames ─────────────────────────────────────────────────────
// THE HOSTILE PROFILE, step 2.5. Live-fire counterpart to the Phase 1 fuzzer:
// fuzzing proved the parser is safe in isolation; this proves the SERVER stays
// up under sustained abuse arriving on a real socket with real connection state
// behind it.
//
// A quarter of the fleet, so honest traffic interleaves with it. A coordinator
// that survives pure garbage but drops good frames alongside it has not passed.
Behaviors Malformed(std::uint32_t index, std::uint32_t fleet, Dice& dice) {
    Behaviors b;
    b.slow_factor = std::clamp(dice.lognormal(0.3), 0.5, 3.0);
    if (fleet > 0 && index < (fleet + 3) / 4) {
        b.malformed_frames = true;
    }
    return b;
}

constexpr std::array<Profile, 7> kProfiles{{
    {"default", "all honest, mild speed spread — the control group", &Default},
    {"heterogeneous", "log-normal speeds, ~20x spread, all honest (E5)", &Heterogeneous},
    {"byzantine_10pct", "10% of workers lie on 30% of their tasks (E4)", &Byzantine10},
    {"flaky_network", "high latency, flapping, some never renew — nobody malicious",
     &FlakyNetwork},
    {"mass_departure", "half the fleet leaves mid-task (E3)", &MassDeparture},
    {"stragglers", "a fifth report a 20x-inflated score, so their tasks run long (E5)",
     &Stragglers},
    {"malformed_frames", "a quarter send hostile frames; the server must stay up (2.5)",
     &Malformed},
}};

}  // namespace

const Profile* FindProfile(std::string_view name) {
    const auto it = std::ranges::find(kProfiles, name, &Profile::name);
    return it == kProfiles.end() ? nullptr : &*it;
}

std::vector<const Profile*> AllProfiles() {
    std::vector<const Profile*> out;
    out.reserve(kProfiles.size());
    for (const auto& p : kProfiles) {
        out.push_back(&p);
    }
    return out;
}

}  // namespace p2pgpu::mock

#pragma once
//
// Named chaos profiles — step 2.4.
//
// Experiments reference a profile BY NAME so a run is reproducible from
// `--chaos <name> --seed <n>` alone. Tuning numbers inline in an experiment is
// how two runs that claim to be the same stop being the same.
//
// A profile maps a worker's index within the fleet to its behaviours, so
// "10% Byzantine" means a specific, replayable 10% rather than a coin flip per
// worker per run.

#include <cstdint>
#include <string_view>
#include <vector>

#include "behaviors.hpp"

namespace p2pgpu::mock {

/// Assigns behaviours to worker `index` of `fleet_size`.
struct Profile {
    // string_view, not string: the profile table is `constexpr`, and a
    // std::string member makes the type non-literal. These all point at string
    // literals with static storage.
    std::string_view name;
    std::string_view description;

    /// Called once per worker at construction. `dice` is that worker's own RNG.
    Behaviors (*assign)(std::uint32_t index, std::uint32_t fleet_size, Dice& dice);
};

/// Look up by name. Returns nullptr for an unknown profile — the caller must
/// refuse to start rather than silently running `default`, because an
/// experiment that quietly ran the wrong fleet produces numbers that look fine.
[[nodiscard]] const Profile* FindProfile(std::string_view name);

[[nodiscard]] std::vector<const Profile*> AllProfiles();

}  // namespace p2pgpu::mock

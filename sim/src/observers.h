#pragma once
// Read-only observers (story 4.7; suppression.md §5). They take const views and nothing else:
// no model, no suppression handle — the scoreboard reads the world, it never writes it.
#include <cstdint>

#include "interface.h"

namespace embersim {

struct World;

struct Metrics {
    int32_t containment_permyriad = 0;  // 0..10000
    uint32_t burning = 0;
    uint32_t burned = 0;
    uint32_t perimeter = 0;             // active perimeter cells
    uint32_t contained_perimeter = 0;
    int32_t structures_lost = -1;       // -1 = no structures layer
    int32_t structures_threatened = -1;
    // filled by the runner from other read-only sources:
    int64_t cost_cents = 0;
    uint32_t busy_resources = 0;
    int32_t wind_u_cms = 0, wind_v_cms = 0, m10 = 0;
};

// Containment / area / perimeter / structure outcomes from a fire state view.
// `retardant_secure_permille`: coverage at which an unburned neighbour counts as protected.
Metrics compute_metrics(const FireStateView& fire, const World& world, uint16_t retardant_secure_permille);

}  // namespace embersim

// Read-only observers (docs/sim/suppression.md §5). Const views in, numbers out.
#include "observers.h"

#include "worldpack.h"

namespace embersim {

Metrics compute_metrics(const FireStateView& fire, const World& world, uint16_t secure) {
    Metrics m;
    const uint32_t nx = fire.nx, ny = fire.ny;
    const uint8_t* ph = fire.phase;
    auto ret = [&](size_t j) -> uint16_t { return fire.retardant ? fire.retardant[j] : 0; };
    const bool has_structures = world.has_structures() && world.structures.size() == fire.ncells();

    for (uint32_t y = 0; y < ny; ++y) {
        for (uint32_t x = 0; x < nx; ++x) {
            size_t i = static_cast<size_t>(y) * nx + x;
            uint8_t p = ph[i];
            if (p == 2) ++m.burning;
            else if (p == 3) ++m.burned;
            if (p != 2 && p != 3) continue;
            // 4-neighbour scan: is this a perimeter cell, and is every unburned neighbour protected?
            bool perimeter = false, all_protected = true;
            auto look = [&](int64_t xx, int64_t yy) {
                if (xx < 0 || yy < 0 || xx >= nx || yy >= ny) return;
                size_t j = static_cast<size_t>(yy) * nx + static_cast<size_t>(xx);
                if (ph[j] == 1) {
                    perimeter = true;
                    if (ret(j) < secure) all_protected = false;
                }
            };
            look(static_cast<int64_t>(x) + 1, y);
            look(static_cast<int64_t>(x) - 1, y);
            look(x, static_cast<int64_t>(y) + 1);
            look(x, static_cast<int64_t>(y) - 1);
            if (!perimeter) continue;
            ++m.perimeter;
            if (p == 3 || all_protected) ++m.contained_perimeter;
        }
    }
    if (m.perimeter == 0) m.containment_permyriad = (m.burning + m.burned) > 0 ? 10000 : 0;
    else m.containment_permyriad = static_cast<int32_t>(static_cast<int64_t>(m.contained_perimeter) * 10000 / m.perimeter);

    if (has_structures) {
        m.structures_lost = 0;
        m.structures_threatened = 0;
        for (uint32_t y = 0; y < ny; ++y) {
            for (uint32_t x = 0; x < nx; ++x) {
                size_t i = static_cast<size_t>(y) * nx + x;
                if (!world.structures[i]) continue;
                uint8_t p = ph[i];
                if (p == 2 || p == 3) { ++m.structures_lost; continue; }
                if (p != 1) continue;
                bool near = false;
                for (int64_t dy = -2; dy <= 2 && !near; ++dy)
                    for (int64_t dx = -2; dx <= 2; ++dx) {
                        int64_t xx = static_cast<int64_t>(x) + dx, yy = static_cast<int64_t>(y) + dy;
                        if (xx < 0 || yy < 0 || xx >= nx || yy >= ny) continue;
                        if (ph[static_cast<size_t>(yy) * nx + static_cast<size_t>(xx)] == 2) { near = true; break; }
                    }
                if (near) ++m.structures_threatened;
            }
        }
    }
    return m;
}

}  // namespace embersim

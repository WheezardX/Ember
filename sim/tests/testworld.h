#pragma once
// In-memory synthetic worlds for C++ tests (plan B2, C++ side). No files, no store access.
// Mirrors ember/sim/worldpack.py's synthetic kinds: flat, ramp, barrier, checker.
#include <cstdint>
#include <string>
#include <vector>

#include "params.h"
#include "worldpack.h"

namespace embersim::test {

struct WorldSpec {
    uint32_t nx = 64, ny = 64;
    double cell_size_m = 30.0;
    uint8_t fuel = 102;          // GR2 by default
    int32_t base_elev_cm = 100000;
    int32_t slope_pct_x = 0;     // ramp: elevation rises along +x by this percent
    int32_t slope_pct_y = 0;     // ... and along +y (south)
    int32_t barrier_x = -1;      // barrier: column of fbfm40=99 at this x
    uint8_t cc_pct = 0;
    uint16_t ch_dm = 0, cbh_dm = 0, cbd_gm3 = 0;
    bool with_greenness = false;
    uint8_t greenness = 64;
    bool with_structures = false;
};

inline World make_world(const WorldSpec& s) {
    World w;
    w.name = "test";
    w.grid.nx = s.nx;
    w.grid.ny = s.ny;
    w.grid.cell_size_m = s.cell_size_m;
    w.grid.cell_mm = static_cast<uint32_t>(s.cell_size_m * 1000.0 + 0.5);
    w.grid.crs = "LOCAL";
    size_t n = w.ncells();
    w.elevation_cm.resize(n);
    w.fbfm40.assign(n, s.fuel);
    w.cc_pct.assign(n, s.cc_pct);
    w.ch_dm.assign(n, s.ch_dm);
    w.cbh_dm.assign(n, s.cbh_dm);
    w.cbd_gm3.assign(n, s.cbd_gm3);
    w.evt.assign(n, 7000);
    for (uint32_t y = 0; y < s.ny; ++y)
        for (uint32_t x = 0; x < s.nx; ++x) {
            int64_t e = s.base_elev_cm;
            e += static_cast<int64_t>(x) * s.cell_size_m * 100.0 * s.slope_pct_x / 100;
            e += static_cast<int64_t>(y) * s.cell_size_m * 100.0 * s.slope_pct_y / 100;
            w.elevation_cm[y * s.nx + x] = static_cast<int32_t>(e);
            if (s.barrier_x >= 0 && static_cast<int32_t>(x) == s.barrier_x) w.fbfm40[y * s.nx + x] = 99;
        }
    if (s.with_greenness) w.greenness.assign(n, s.greenness);
    if (s.with_structures) w.structures.assign(n, 0);
    w.pack_sha256 = "test";
    return w;
}

// The committed default CA params pack (sim/packs/ca_params.v1.toml).
inline ParamsPack default_ca_params() {
    return load_params(std::string(EMBERSIM_REPO_DIR) + "/sim/packs/ca_params.v1.toml");
}
inline ParamsPack default_production_pack() {
    return load_params(std::string(EMBERSIM_REPO_DIR) + "/sim/packs/production_rates.nwcg.toml");
}

// Count cells by phase in a view.
inline size_t count_phase(const FireStateView& v, uint8_t phase) {
    size_t c = 0;
    for (size_t i = 0; i < v.ncells(); ++i) c += (v.phase[i] == phase);
    return c;
}

}  // namespace embersim::test

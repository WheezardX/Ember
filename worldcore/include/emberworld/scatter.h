// Epic 2 deterministic vegetation scatter v2 (Terrain ADR 0007: per-EVT palettes, stand
// structure), ported bit-for-bit (EPIC_5_PLAN C4, D5).
//
// Reference implementation: Terrain `terrain/veg/{hashing,scatter,palette}.py` (the "spec is the
// code", EPIC_5_PLAN D10). Contract:
//   * DECISIONS (does an instance exist, which species) are integer-only: splitmix64 hashing,
//     `% 10000` against round-half-even(cc% * 100), integer weighted pick. These must match
//     exactly on every compiler.
//   * Cosmetic attributes (position jitter, height, yaw, scale) are IEEE doubles evaluated in
//     the same order as the Python; with no fused multiply-add they match exactly too.
//   * Cells are independent: cell_seed = hash64(tile_seed, r, c) over GLOBAL canonical-grid
//     indices, so any window of the grid can be scattered alone (per render tile) and the
//     union equals Terrain's whole-AOI scatter, instance for instance and in the same
//     per-cell order. (Terrain's max_instances truncation is global; the render world is
//     baked with a limit that never truncates.)
#pragma once

#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "emberworld/api.h"

namespace emberworld::scatter {

EMBERWORLD_CORE_API uint64_t splitmix64(uint64_t x);
EMBERWORLD_CORE_API uint64_t hash64(std::initializer_list<uint64_t> values);
EMBERWORLD_CORE_API double u01(uint64_t h);
EMBERWORLD_CORE_API int weighted_pick(uint64_t h, std::span<const int64_t> weights);

struct Species {
    std::string key;
    int64_t weight = 1;
    double height_min_m = 1.0;
    double height_max_m = 1.0;
    double crown_ratio = 0.25;    // crown diameter / tree height
};

// Stand-structure tier: height = h_lo..h_hi x the cell's canopy-top height.
struct CrownClass {
    std::string name;
    int64_t weight = 1;
    double h_lo = 1.0;
    double h_hi = 1.0;
};

struct Group {
    std::string name;
    std::vector<int64_t> evt_codes;  // exact codes: win over any range
    bool has_range = false;
    int64_t evt_min = 0;
    int64_t evt_max = 0;
    std::vector<Species> species;
    std::vector<int64_t> weights;  // derived from species
    int offset = 0;                // index of species[0] in the flat species index
};

struct EMBERWORLD_CORE_API Palette {
    std::string name;
    std::string default_group;
    double crown_base_m = 0.0;           // crown diameter = base + crown_ratio x height
    std::vector<CrownClass> structure;
    std::vector<int64_t> class_weights;  // derived from structure
    std::vector<Group> groups;

    const Group& group_for_evt(int64_t evt) const;  // exact code, then first range, else default
    std::vector<const Species*> species_index() const;
    void finalize();                                // fill weights/offsets after edits
};

struct PaletteResult {
    Palette palette;
    std::string error;
    bool ok() const { return error.empty(); }
};

// Load a Terrain palette TOML (e.g. terrain/packs/palettes/pnw_conifer.toml).
EMBERWORLD_CORE_API PaletteResult load_palette(const std::string& path);

struct Params {
    uint64_t tile_seed = 0;       // hash64(world_seed, layer_version, base_lod, 0, 0) upstream
    double cell_size = 10.0;
    double x0 = 0.0;              // canonical grid west edge (world metres)
    double y_top = 0.0;           // canonical grid north edge
    int candidates_per_cell = 4;
    double cc_nodata = -9999.0;
    double dem_nodata = -9999.0;
    double height_nodata = -9999.0;
    bool ground_tree = false;     // U12 ground_z = "tree": z = DEM bilinear at the tree, not the cell
};

struct Instance {
    double x = 0, y = 0, z = 0;   // world metres (z: see Params::ground_tree; 0 where DEM is nodata)
    int species = 0;              // index into Palette::species_index()
    double height_m = 0;
    double yaw_rad = 0;
    double scale = 0;
    double radius_m = 0;          // crown radius: 0.5 x (crown_base_m + crown_ratio x height) x 0.85..1.15
};

// Scatter a window of the canonical grid. Arrays are window-local, row-major (row 0 = north),
// `rows * cols` long; `r0`/`c0` are the window's global row/col (used in hashing and position).
// Appends in Terrain's order (row, col, attempt). `dem` may carry `dem_halo` extra cells on
// every side ((rows + 2h) x (cols + 2h)): the neighbours the per-tree bilinear needs at the edge.
EMBERWORLD_CORE_API void scatter_window(const Palette& palette, const Params& p, int r0, int c0,
                                        int rows, int cols, std::span<const float> cc,
                                        std::span<const float> height, std::span<const int32_t> evt,
                                        std::span<const float> dem, std::vector<Instance>& out,
                                        int dem_halo = 0);

// Ground under a tree at jitter (jx, jy) in DEM-array cell (rr, cc): bilinear between the four
// cell centres around it; `z_cell` where a neighbour is outside the array or nodata. Mirrors
// terrain/veg/scatter.py::tree_ground term for term (U12).
EMBERWORLD_CORE_API double tree_ground(std::span<const float> dem, int rows, int cols, int rr, int cc,
                                       double jx, double jy, double z_cell, double dem_nodata);

}  // namespace emberworld::scatter

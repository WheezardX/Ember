// Per-tile vegetation for the renderer (EPIC_5_PLAN C4): scatter one Terrain tile straight
// from its finest-LOD layer rasters. The finest-LOD tile layers are bit-identical copies of the
// canonical rasters Terrain scattered from (verified on teanaway_dev), and cells scatter
// independently, so per-tile output == Terrain's veg/instances.npy restricted to the tile.
#pragma once

#include <string>
#include <vector>

#include "emberworld/api.h"
#include "emberworld/region.h"
#include "emberworld/scatter.h"

namespace emberworld {

// veg/scatter.input.json, written by Terrain's vegetation stage.
struct ScatterInput {
    scatter::Params params;
    std::string palette_ref;     // as recorded, e.g. "packs/palettes/pnw_conifer.toml"
    std::string height_layer;    // tile layer holding scatter height: canopy_chm | fuels_ch
    int base_lod = 0;
};

struct ScatterInputResult {
    ScatterInput input;
    std::string error;
    bool ok() const { return error.empty(); }
};

EMBERWORLD_CORE_API ScatterInputResult load_scatter_input(const Region& region);

// Resolve a palette reference: absolute/relative paths as-is if they exist, else
// "packs/<x>" against <terrain_repo>/terrain/packs/<x> where terrain_repo = region/../..
EMBERWORLD_CORE_API std::string resolve_palette(const Region& region, const std::string& ref);

struct TileScatterResult {
    std::vector<scatter::Instance> instances;
    int r0 = 0, c0 = 0;          // global canonical cell of the tile's content NW pixel
    std::string error;
    bool ok() const { return error.empty(); }
};

// Scatter the content pixels of a finest-LOD tile.
EMBERWORLD_CORE_API TileScatterResult scatter_tile(const Region& region, const TileEntry& tile,
                                                   const scatter::Palette& palette,
                                                   const ScatterInput& in);

}  // namespace emberworld

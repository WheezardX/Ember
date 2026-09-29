// Terrain look (EPIC_5_PLAN B2): store layers -> per-tile albedo, engine-free and testable.
// The look itself is data (viz/looks/*.toml); see terrain_default.toml for the rules.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "emberworld/api.h"
#include "emberworld/region.h"

namespace emberworld {

struct Rgb {
    float r = 0, g = 0, b = 0;  // linear 0..1
};

enum class LandClass : uint8_t {
    Unknown, Urban, Snow, Agriculture, Water, Barren, Grass, GrassShrub, Shrub,
    TimberUnderstory, TimberLitter, SlashBlowdown, Count
};

EMBERWORLD_CORE_API LandClass classify_fbfm40(int code);
EMBERWORLD_CORE_API bool is_vegetated(LandClass c);

struct TerrainLook {
    std::string name;
    std::array<Rgb, static_cast<size_t>(LandClass::Count)> classes{};
    Rgb green, rock;
    double ndvi_low = 0.25, ndvi_high = 0.85, green_strength = 0.55, canopy_darken = 0.45;
    double slope_start_deg = 35.0, slope_full_deg = 55.0;
    // [detail] — breaking the 10 m pixel grid (HCP1: hard class squares read as digital camo)
    int supersample = 4;            // output texels per source pixel, per axis
    double boundary_warp_px = 0.6;  // world-aligned domain warp of class lookups (source px)
    double blur_radius_px = 0.5;    // box-blur radius in SOURCE pixels (2 passes ~ gaussian); 0 = off
    // [ground] (ground plane v1, EPIC_5_PLAN 8f): what is on the ground, per class, as weights of
    // the four near-camera detail sets {litter, grass, rock, shrub}; the remainder is plain soil.
    // Steep slopes move weight to rock like the colour; canopy cover turns grass/shrub to litter.
    bool has_ground = false;
    std::array<std::array<float, 4>, static_cast<size_t>(LandClass::Count)> ground{};
    double canopy_to_litter = 0.8;  // at 100 % cover, this fraction of grass + shrub becomes litter
};

enum GroundSet { kLitter = 0, kGrass = 1, kRock = 2, kShrub = 3 };

struct LookResult {
    TerrainLook look;
    std::string error;
    bool ok() const { return error.empty(); }
};

EMBERWORLD_CORE_API LookResult load_look(const std::string& path);

// sRGB hex "#RRGGBB" <-> linear.
EMBERWORLD_CORE_API bool parse_srgb_hex(const std::string& hex, Rgb& out);
EMBERWORLD_CORE_API uint8_t linear_to_srgb8(float v);

// Per-pixel inputs for one tile raster (apron included: (tile_px + 2 * overlap_px)^2).
struct LookInputs {
    int width = 0, height = 0;
    double pixel_m = 10.0;
    std::vector<float> dem;        // metres; nodata -> NaN
    std::vector<int32_t> fbfm40;   // 0 = nodata
    std::vector<float> cc;         // canopy cover %, <0 = nodata
    std::vector<float> ndvi;       // may be empty (no season layer); NaN = nodata
    double origin_x_m = 0.0;       // world coords of the raster's NW corner (for world-aligned
    double origin_y_m = 0.0;       //   noise, so tile edges match)
};

struct Albedo {
    int width = 0, height = 0;
    std::vector<uint8_t> bgra;     // sRGB-encoded BGRA8, row 0 = north; alpha 0 where DEM nodata
    // Ground mix (when the look has [ground]): linear RGBA8 weights R litter, G grass, B rock,
    // A shrub, same size and texel alignment as bgra; 0 where DEM nodata. Empty otherwise.
    std::vector<uint8_t> mix;
};

EMBERWORLD_CORE_API Albedo compose_albedo(const TerrainLook& look, const LookInputs& in);

// Full mip chain (level 0 = the input) down to 1x1, 2x2 box filter, alpha-weighted so nodata
// (alpha 0) never darkens valid texels; odd sizes floor (the last row/column folds in).
// Found at HCP1 work: a single-mip transient texture sampled at distance returns garbage.
EMBERWORLD_CORE_API std::vector<Albedo> build_mips(const Albedo& level0);

// Mip chain for the ground mix (level 0 = level0.mix): plain 2x2 box average of all four
// channels (weights, not colour: no alpha weighting). Returned as Albedo with .bgra = the level.
EMBERWORLD_CORE_API std::vector<Albedo> build_mix_mips(const Albedo& level0);

// Read the tile's layers (height + fuels_fbfm40 + fuels_cc + season_greenness if present).
EMBERWORLD_CORE_API bool load_look_inputs(const Region& region, const TileEntry& tile, LookInputs& out,
                                          std::string& error);

}  // namespace emberworld

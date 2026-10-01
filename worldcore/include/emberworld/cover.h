// Ground cover (ground plane v1, EPIC_5_PLAN 8f GP4): near-camera grass tufts, ferns, shrubs,
// rocks, logs and stumps, placed deterministically from the look's ground mix. Engine-free and
// testable; the renderer (AEmberGroundCoverActor) streams it in cells around the camera.
//
// Placement lives on the GLOBAL 1 m lattice: each lattice square gets `candidates_per_m2`
// hashed candidates, each accepted with probability density / candidates, so any partition of
// the world into cells yields exactly the same instances.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "emberworld/api.h"

namespace emberworld {

// How an item meets the ground (8g, Brad: logs ignored the slope and stuck out of hills).
//   Upright  stands on the lowest point under its footprint (rocks, stumps, plants, snags)
//   Conform  lies along the surface: pitched / rolled to the ground under both ends, resting on
//            humps, partly buried or propped by a per-instance hash (logs)
//   Leaner   a fallen stem hung up in the canopy: foot on the ground, pitched up 20-40 deg
enum class CoverPose { Upright, Conform, Leaner };

struct CoverItem {
    std::string key;                    // rule name
    std::string mesh;                   // mesh family (Cover/SM_Cover_<mesh>_v*, or "grass"); = key by default
    std::array<float, 4> per_100m2{};   // density on each ground set {litter, grass, rock, shrub}
    float soil_per_100m2 = 0.0f;        // density on plain soil (weight left after the four sets)
    double height_min_m = 0.3, height_max_m = 0.6;
    int variants = 3;
    float consume = 0.0f;    // fire: burned plants collapse to stubble (M_Veg Consume)
    float smoulder = 0.0f;   // fire: wood glows for hours after the front (M_Veg Smoulder)
    CoverPose pose = CoverPose::Upright;   // look key `pose` = "upright" | "conform" | "leaner"
    // Clumping (EPIC_5_PLAN, Brad: "little tufts evenly dispersed"; GW2 broke it up with Perlin
    // noise): density x max(0, 1 + 4 clump (n - 0.5)) for a world-space fbm n in [0, 1] (scale
    // clump_m, two octaves) - patches and bare gaps, about the same average. Items with the same
    // clump_group share a field (green plants in the same light gaps); clump_anti reads it
    // inverted (grass thins under fern clumps). clump_size grows plants toward clump centres.
    float clump = 0.0f;          // 0 = uniform (as before)
    double clump_m = 8.0;
    std::string clump_group;     // "" = the item's own field (seeded by its key)
    bool clump_anti = false;
    float clump_size = 0.0f;     // height x (1 + clump_size x (density multiplier - 1) / 2), >= 0.5
    // Conform pose only: >= 0 lays the stem axis this far above the ground (m, x 0.6-1.4 per
    // instance, x the instance scale) instead of burying a fraction of the mesh height - a whole
    // downed tree rests on its limbs (its mesh height is the crown, not the stem). -1 = log rule.
    double rest_m = -1.0;
};

struct CoverRules {
    std::vector<CoverItem> items;
    int candidates_per_m2 = 2;
    uint64_t seed = 0x6C0FE7;
};

struct CoverRulesResult {
    CoverRules rules;
    bool present = false;   // the look has a [cover] table
    std::string error;
    bool ok() const { return error.empty(); }
};

// Read [cover] from a look file (viz/looks/*.toml). Absent -> present = false, no error.
EMBERWORLD_CORE_API CoverRulesResult load_cover_rules(const std::string& look_path);

struct CoverInstance {
    double x = 0, y = 0;    // world metres (projected CRS)
    int item = 0;           // index into CoverRules::items
    int variant = 0;
    double height_m = 0;
    double yaw_rad = 0;
};

// Ground weights at a world point: {litter, grass, rock, shrub}; false = no ground there.
using GroundMixFn = std::function<bool(double x, double y, std::array<float, 4>& w)>;

// Every instance whose position lies in [x0, x1) x [y0, y1), appended in lattice order.
EMBERWORLD_CORE_API void scatter_cover(const CoverRules& rules, double x0, double y0, double x1, double y1,
                                       const GroundMixFn& mix_at, std::vector<CoverInstance>& out);

}  // namespace emberworld

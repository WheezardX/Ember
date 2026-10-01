// Near-ground micro relief (ground feel, EPIC_5_PLAN "ground still flat - no volume", option 3):
// the hummocks, duff mounds and lumps a 13 m terrain mesh cannot carry. The renderer
// (AEmberGroundRelief) draws it as a dense patch around the camera on top of the terrain.
//
// Height in metres ABOVE the terrain surface (>= 0, so anything placed on the plain surface is at
// worst partly buried, never floating), driven by world position only: deterministic and
// independent of how the world is partitioned. Amplitude follows the ground mix - the forest
// floor (litter) is lumpiest, grass and soil smoothest.
#pragma once

#include <array>
#include <functional>

#include "emberworld/api.h"

namespace emberworld {

struct ReliefParams {
    double mound_m = 3.0;        // hummock feature size (m)
    double lump_m = 1.1;         // small lumps (m)
    // Peak height per ground set {litter, grass, rock, shrub} and plain soil (m)
    std::array<double, 4> amp_m{0.32, 0.10, 0.15, 0.20};   // (0.24 litter did not read under canopy light)
    double soil_amp_m = 0.05;
};

// w = ground mix {litter, grass, rock, shrub} at (x, y) (the rest is soil).
EMBERWORLD_CORE_API double micro_relief(double x, double y, const std::array<float, 4>& w,
                                        const ReliefParams& p = ReliefParams());

// Burn-mosaic survival field (EPIC_5_PLAN, Brad: "single unaffected trees inside large scars are not
// realistic - survivors come in pockets"): 0..1 at a world point, high = a tree there is more likely
// to come through. Spatially coherent (two-octave value noise at ~150 m / 50 m: patches, not single
// trees) and biased by terrain shelter - lower than its surroundings (draws, valley floors: the
// ground against a 75 m ring) and north-facing slopes. Spread out to roughly uniform over a
// landscape, so the renderer's per-class outcome proportions hold. `height(x, y, z)` samples the
// ground (metres); false = no data (the field falls back to the noise alone).
using HeightFn = std::function<bool(double x, double y, double& z)>;
EMBERWORLD_CORE_API double survival_field(double x, double y, const HeightFn& height);

}  // namespace emberworld

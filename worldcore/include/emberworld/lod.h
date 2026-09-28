// Camera-driven tile selection over the Terrain quadtree (EPIC_5_PLAN C3, D4).
//
// Start from every coarsest tile; refine a tile into its (up to 4) children while the camera
// is closer than `refine_factor * span` to the tile and a finer level exists. The result covers
// the region with no overlaps: each selected tile's content area is disjoint from every other.
// Children that Terrain skipped (all-nodata) are simply absent. Cracks between neighbours of
// different LOD are hidden by skirts (heightfield.h).
#pragma once

#include <vector>

#include "emberworld/api.h"
#include "emberworld/region.h"

namespace emberworld {

struct LodOptions {
    double refine_factor = 1.5;  // refine while distance < factor * tile span
    int max_lod = -1;            // clamp refinement (-1 = finest available)
};

// Camera position in region-CRS metres (z = elevation). Returns tiles coarse-to-fine order.
EMBERWORLD_CORE_API std::vector<const TileEntry*> select_tiles(const Region& region, double cam_x,
                                                               double cam_y, double cam_z,
                                                               const LodOptions& opt = {});

// Distance from a point to a tile's content box (horizontal) combined with height above the
// region's z range - the metric select_tiles uses.
EMBERWORLD_CORE_API double tile_distance(const Region& region, const TileEntry& t, double x,
                                         double y, double z);

}  // namespace emberworld

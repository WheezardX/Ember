// Terrain tile -> render mesh (EPIC_5_PLAN C2), engine-free.
//
// Vertices sit on content-pixel *corners*: (tile_px + 1)^2 per tile. A corner's height is the
// mean of the four pixel centres around it, which the apron makes available at tile edges.
// Two tiles of the same LOD therefore compute identical edge vertices from identical inputs
// in identical order — same-LOD seams are exact by construction. Normals use central
// differences over an extra ring of corners (also from the apron), so shading is seamless
// too. Cracks between *different* LODs are hidden by skirts.
//
// Output frame is UE's: centimetres, X = east, Y = south, Z = up, relative to an anchor.
#pragma once

#include "emberworld/api.h"

#include <cstdint>
#include <string>
#include <vector>

#include "emberworld/region.h"
#include "emberworld/tiff.h"

namespace emberworld {

struct Vec3f {
    float x = 0, y = 0, z = 0;
};
struct Vec2f {
    float u = 0, v = 0;
};

// Maps region-CRS metres to the UE frame.
struct Frame {
    double anchor_x = 0, anchor_y = 0, anchor_z = 0;  // world metres placed at UE origin
    Vec3f to_ue(double wx, double wy, double wz) const {
        return {static_cast<float>((wx - anchor_x) * 100.0), static_cast<float>((anchor_y - wy) * 100.0),
                static_cast<float>((wz - anchor_z) * 100.0)};
    }
};

// Anchor at the centre of the region's extent, at z_min.
EMBERWORLD_CORE_API Frame region_frame(const Region& r);

struct MeshOptions {
    double skirt_depth_m = -1.0;  // < 0: max(4 * pixel size, 1 m) — Terrain's own rule
    bool skirts = true;
    bool flip_winding = false;    // default winding is UE's (see heightfield.cpp); flip for others
};

struct TileMesh {
    std::vector<Vec3f> positions;
    std::vector<Vec3f> normals;
    std::vector<Vec2f> uv0;       // 0..1 across the tile's content (u east, v south)
    std::vector<Vec2f> uv1;       // region-extent fraction (u east, v south) for region-wide layers
    std::vector<uint32_t> indices;
    int grid_n = 0;               // corners per side (tile_px + 1)
    int surface_triangles = 0;
    int skirt_triangles = 0;
    int nodata_corners = 0;       // corners with no valid neighbouring pixel (no triangles touch them)
    double min_z = 0, max_z = 0;  // metres, over valid corners
    Bounds valid_bounds;          // world metres covered by valid corners (if any)
    bool has_valid = false;
};

struct MeshResult {
    TileMesh mesh;
    std::string error;
    bool ok() const { return error.empty(); }
};

EMBERWORLD_CORE_API MeshResult build_tile_mesh(const Region& region, const TileEntry& tile, const Raster& height,
                           const Frame& frame, const MeshOptions& opt = {});

// Convenience: read the tile's height.tif and build.
EMBERWORLD_CORE_API MeshResult load_tile_mesh(const Region& region, const TileEntry& tile, const Frame& frame,
                          const MeshOptions& opt = {});

// Bilinear-free ground height at a world point from the finest LOD's corner rule (for camera
// targets / probes). Returns false outside the region or on read failure.
EMBERWORLD_CORE_API bool sample_height(const Region& region, double wx, double wy, double& out_z);

// The rendered surface of one tile: bilinear over the same corner heights the mesh uses, so
// objects placed with it sit exactly on the triangles' plane at grid corners and within the
// quad's bilinear patch elsewhere (vegetation grounding; Terrain's scatter z is the raw DEM
// cell value, which floats/sinks by metres on slopes).
class EMBERWORLD_CORE_API SurfaceSampler {
public:
    bool build(const Region& region, const TileEntry& tile, const Raster& height);
    // World metres in, metres out; false outside the tile content or on invalid corners.
    bool height_at(double wx, double wy, double& out_z) const;

private:
    Bounds content_;
    double px_ = 10.0;
    int n_ = 0;                       // corners per side
    std::vector<double> z_;           // n_ x n_, row 0 = north
    std::vector<uint8_t> valid_;
};

// Bounds of the valid data (same corner-validity rule as the mesh: a corner is valid when any
// of its four pixels is), scanned over the finest LOD without building meshes. Tiles can
// extend past the AOI; this is what bookmarks address.
EMBERWORLD_CORE_API bool data_extent(const Region& region, Bounds& out, std::string& error);

}  // namespace emberworld

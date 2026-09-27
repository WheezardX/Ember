// Terrain tile-store region (Epic 1 manifest schema v2) — the canonical render world
// (EPIC_5_PLAN D10). A region directory holds manifest.json plus tiles/z{lod}/x{col}/y{row}/.
//
// Tile addressing (Terrain adr/0004, terrain/tiling/scheme.py): quadtree, finest level =
// max(lods); x grows east, y grows NORTH from the DEM's lower-left corner. Each stored raster
// covers apron_bounds (content + overlap_px on every side), row 0 = north.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace emberworld {

struct Bounds {
    double min_x = 0, min_y = 0, max_x = 0, max_y = 0;  // world metres (region CRS)
    double width() const { return max_x - min_x; }
    double height() const { return max_y - min_y; }
};

struct TileEntry {
    int lod = 0, x = 0, y = 0;
    Bounds content;
    Bounds apron;
    std::string height_tif;   // region-relative, forward slashes
    std::string heightmap_png;
    std::string mesh_glb;
    std::string hash;
};

struct LayerTile {
    int lod = 0, x = 0, y = 0;
    std::string path;
    std::string hash;
};

struct Layer {
    std::string name;
    std::string unit;
    bool categorical = false;
    std::vector<LayerTile> tiles;
    const LayerTile* find(int lod, int x, int y) const;
};

struct HeightQuant {
    int bits = 16;
    double scale = 1.0, offset = 0.0, z_min = 0.0, z_max = 0.0;
};

struct Region {
    std::string root;          // absolute directory, forward slashes, no trailing slash
    std::string name;          // directory name
    int schema_version = 0;
    std::string crs;
    int tile_px = 0;
    int overlap_px = 0;
    std::vector<int> lods;     // as listed (finest first in Terrain's writer)
    HeightQuant heightmap;
    std::vector<TileEntry> tiles;
    std::map<std::string, Layer> layers;

    int finest_lod() const;
    int coarsest_lod() const;
    const TileEntry* find(int lod, int x, int y) const;
    std::vector<const TileEntry*> tiles_at(int lod) const;
    Bounds extent() const;     // union of content bounds at the finest LOD
    std::string path(const std::string& rel) const { return root + "/" + rel; }
};

struct RegionResult {
    std::optional<Region> region;
    std::string error;
    explicit operator bool() const { return region.has_value(); }
};

// dir: the region directory (containing manifest.json). Never throws.
RegionResult load_region(const std::string& dir);

}  // namespace emberworld

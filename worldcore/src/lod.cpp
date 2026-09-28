#include "emberworld/lod.h"

#include <algorithm>
#include <cmath>

namespace emberworld {

double tile_distance(const Region& region, const TileEntry& t, double x, double y, double z) {
    const double dx = std::max({t.content.min_x - x, 0.0, x - t.content.max_x});
    const double dy = std::max({t.content.min_y - y, 0.0, y - t.content.max_y});
    const double dz = std::max({region.heightmap.z_min - z, 0.0, z - region.heightmap.z_max});
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

namespace {

void visit(const Region& r, const TileEntry& t, double x, double y, double z, const LodOptions& opt,
           int max_lod, std::vector<const TileEntry*>& out) {
    const bool can_refine = t.lod < max_lod;
    if (can_refine && tile_distance(r, t, x, y, z) < opt.refine_factor * t.content.width()) {
        std::vector<const TileEntry*> kids;
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i)
                if (const TileEntry* k = r.find(t.lod + 1, 2 * t.x + i, 2 * t.y + j)) kids.push_back(k);
        if (!kids.empty()) {
            for (const TileEntry* k : kids) visit(r, *k, x, y, z, opt, max_lod, out);
            return;
        }
    }
    out.push_back(&t);
}

}  // namespace

std::vector<const TileEntry*> select_tiles(const Region& region, double cam_x, double cam_y,
                                           double cam_z, const LodOptions& opt) {
    const int max_lod = opt.max_lod < 0 ? region.finest_lod() : std::min(opt.max_lod, region.finest_lod());
    std::vector<const TileEntry*> out;
    for (const TileEntry* t : region.tiles_at(region.coarsest_lod()))
        visit(region, *t, cam_x, cam_y, cam_z, opt, max_lod, out);
    return out;
}

}  // namespace emberworld

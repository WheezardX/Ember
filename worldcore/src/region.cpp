#include "emberworld/region.h"

#include <algorithm>
#include <fstream>
#include <limits>

#include "json.hpp"

namespace emberworld {

namespace {

std::string slashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

// Optional string field: absent OR explicit null -> "" (Terrain writes "mesh": null when glTF
// tile export is off; nlohmann's value() throws on null).
std::string str_or_empty(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    return (it == j.end() || it->is_null()) ? std::string{} : it->get<std::string>();
}

Bounds bounds_of(const nlohmann::json& a) {
    Bounds b;
    b.min_x = a.at(0).get<double>();
    b.min_y = a.at(1).get<double>();
    b.max_x = a.at(2).get<double>();
    b.max_y = a.at(3).get<double>();
    return b;
}

}  // namespace

const LayerTile* Layer::find(int lod, int x, int y) const {
    for (const auto& t : tiles)
        if (t.lod == lod && t.x == x && t.y == y) return &t;
    return nullptr;
}

int Region::finest_lod() const { return lods.empty() ? 0 : *std::max_element(lods.begin(), lods.end()); }
int Region::coarsest_lod() const { return lods.empty() ? 0 : *std::min_element(lods.begin(), lods.end()); }

const TileEntry* Region::find(int lod, int x, int y) const {
    for (const auto& t : tiles)
        if (t.lod == lod && t.x == x && t.y == y) return &t;
    return nullptr;
}

std::vector<const TileEntry*> Region::tiles_at(int lod) const {
    std::vector<const TileEntry*> out;
    for (const auto& t : tiles)
        if (t.lod == lod) out.push_back(&t);
    return out;
}

Bounds Region::extent() const {
    Bounds b{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
             std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
    for (const auto* t : tiles_at(finest_lod())) {
        b.min_x = std::min(b.min_x, t->content.min_x);
        b.min_y = std::min(b.min_y, t->content.min_y);
        b.max_x = std::max(b.max_x, t->content.max_x);
        b.max_y = std::max(b.max_y, t->content.max_y);
    }
    return b;
}

RegionResult load_region(const std::string& dir_in) {
    RegionResult out;
    const std::string dir = slashes(dir_in);
    const std::string mpath = dir + "/manifest.json";
    std::ifstream f(mpath, std::ios::binary);
    if (!f) {
        out.error = "cannot open " + mpath;
        return out;
    }
    try {
        const auto j = nlohmann::json::parse(f);
        Region r;
        r.root = dir;
        const auto slash = dir.find_last_of('/');
        r.name = slash == std::string::npos ? dir : dir.substr(slash + 1);
        r.schema_version = j.at("manifest_schema_version").get<int>();
        if (r.schema_version != 2) {
            out.error = mpath + ": manifest_schema_version " + std::to_string(r.schema_version) +
                        " unsupported (want 2)";
            return out;
        }
        r.crs = j.at("crs").get<std::string>();
        r.tile_px = j.at("tile_px").get<int>();
        r.overlap_px = j.at("overlap_px").get<int>();
        r.lods = j.at("lods").get<std::vector<int>>();
        const auto& hm = j.at("heightmap");
        r.heightmap.bits = hm.value("bits", 16);
        r.heightmap.scale = hm.at("scale").get<double>();
        r.heightmap.offset = hm.at("offset").get<double>();
        r.heightmap.z_min = hm.at("z_min").get<double>();
        r.heightmap.z_max = hm.at("z_max").get<double>();
        for (const auto& t : j.at("tiles")) {
            TileEntry e;
            e.lod = t.at("lod").get<int>();
            e.x = t.at("x").get<int>();
            e.y = t.at("y").get<int>();
            e.content = bounds_of(t.at("content_bounds"));
            e.apron = bounds_of(t.at("apron_bounds"));
            e.height_tif = slashes(str_or_empty(t, "height_tif"));
            e.heightmap_png = slashes(str_or_empty(t, "heightmap"));
            e.mesh_glb = slashes(str_or_empty(t, "mesh"));
            e.hash = str_or_empty(t, "hash");
            r.tiles.push_back(std::move(e));
        }
        if (j.contains("layers")) {
            for (const auto& [name, lj] : j.at("layers").items()) {
                Layer l;
                l.name = name;
                l.unit = str_or_empty(lj, "unit");
                l.categorical = lj.value("categorical", false);
                for (const auto& t : lj.at("tiles")) {
                    LayerTile lt;
                    lt.lod = t.at("lod").get<int>();
                    lt.x = t.at("x").get<int>();
                    lt.y = t.at("y").get<int>();
                    lt.path = slashes(t.at("path").get<std::string>());
                    lt.hash = str_or_empty(t, "hash");
                    l.tiles.push_back(std::move(lt));
                }
                r.layers.emplace(name, std::move(l));
            }
        }
        if (r.tiles.empty()) {
            out.error = mpath + ": no tiles";
            return out;
        }
        out.region = std::move(r);
    } catch (const std::exception& e) {
        out.error = mpath + ": " + e.what();
    }
    return out;
}

}  // namespace emberworld

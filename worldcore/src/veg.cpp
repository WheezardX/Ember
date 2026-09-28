#include "emberworld/veg.h"

#include <cmath>
#include <filesystem>
#include <fstream>

#include "emberworld/tiff.h"
#include "json.hpp"

namespace emberworld {

ScatterInputResult load_scatter_input(const Region& region) {
    ScatterInputResult res;
    const std::string path = region.path("veg/scatter.input.json");
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        res.error = "cannot open " + path + " (region baked without [vegetation]?)";
        return res;
    }
    try {
        const auto j = nlohmann::json::parse(f);
        ScatterInput& in = res.input;
        in.params.tile_seed = j.at("tile_seed").get<uint64_t>();
        in.params.candidates_per_cell = j.at("candidates_per_cell").get<int>();
        in.params.cell_size = j.at("cell_size_m").get<double>();
        in.params.x0 = j.at("origin_xy").at(0).get<double>();
        in.params.y_top = j.at("origin_xy").at(1).get<double>();
        in.base_lod = j.at("tile_addr").at(0).get<int>();
        in.palette_ref = j.at("palette").get<std::string>();
        std::string h = j.at("rasters").at("height").get<std::string>();
        for (char& ch : h)
            if (ch == '\\') ch = '/';
        in.height_layer = h.find("chm") != std::string::npos ? "canopy_chm" : "fuels_ch";
    } catch (const std::exception& e) {
        res.error = path + ": " + e.what();
    }
    return res;
}

std::string resolve_palette(const Region& region, const std::string& ref) {
    namespace fs = std::filesystem;
    if (fs::exists(ref)) return ref;
    const fs::path terrain_repo = fs::path(region.root).parent_path().parent_path();
    if (ref.rfind("packs/", 0) == 0) {
        const fs::path p = terrain_repo / "terrain" / ref;
        if (fs::exists(p)) return p.generic_string();
    }
    return (terrain_repo / ref).generic_string();
}

namespace {

// Content pixels (tile_px^2) of a layer tile as float, or error.
bool content_pixels(const Region& region, const std::string& rel, std::vector<float>& out,
                    std::string& err) {
    TiffResult t = read_tiff(region.path(rel));
    if (!t) {
        err = t.error.message;
        return false;
    }
    const Raster& r = *t.raster;
    const int tp = region.tile_px, ov = region.overlap_px;
    if (r.width != tp + 2 * ov || r.height != tp + 2 * ov) {
        err = rel + ": unexpected size";
        return false;
    }
    out.resize(static_cast<size_t>(tp) * tp);
    for (int y = 0; y < tp; ++y)
        for (int x = 0; x < tp; ++x) out[static_cast<size_t>(y) * tp + x] = static_cast<float>(r.at(x + ov, y + ov));
    return true;
}

}  // namespace

TileScatterResult scatter_tile(const Region& region, const TileEntry& tile,
                               const scatter::Palette& palette, const ScatterInput& in) {
    TileScatterResult res;
    auto find = [&](const char* layer) -> const LayerTile* {
        auto it = region.layers.find(layer);
        return it == region.layers.end() ? nullptr : it->second.find(tile.lod, tile.x, tile.y);
    };
    const LayerTile* lcc = find("fuels_cc");
    const LayerTile* levt = find("fuels_evt");
    const LayerTile* lh = find(in.height_layer.c_str());
    if (!lcc || !levt || !lh) {
        res.error = "tile z" + std::to_string(tile.lod) + "/x" + std::to_string(tile.x) + "/y" +
                    std::to_string(tile.y) + ": missing fuels_cc / fuels_evt / " + in.height_layer;
        return res;
    }
    std::vector<float> cc, evtf, h, dem;
    if (!content_pixels(region, lcc->path, cc, res.error) ||
        !content_pixels(region, levt->path, evtf, res.error) ||
        !content_pixels(region, lh->path, h, res.error) ||
        !content_pixels(region, tile.height_tif, dem, res.error))
        return res;
    std::vector<int32_t> evt(evtf.size());
    for (size_t i = 0; i < evt.size(); ++i) evt[i] = static_cast<int32_t>(evtf[i]);

    const double cs = in.params.cell_size;
    res.c0 = static_cast<int>(std::lround((tile.content.min_x - in.params.x0) / cs));
    res.r0 = static_cast<int>(std::lround((in.params.y_top - tile.content.max_y) / cs));
    const int tp = region.tile_px;
    scatter::scatter_window(palette, in.params, res.r0, res.c0, tp, tp, cc, h, evt, dem, res.instances);
    return res;
}

}  // namespace emberworld

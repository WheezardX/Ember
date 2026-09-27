// Test helpers: a tiny TIFF writer and a synthetic tile-store region with an analytic
// height function, so seam/format tests run in CI without Terrain's store.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace testutil {

namespace fs = std::filesystem;

inline fs::path temp_dir(const std::string& name) {
    fs::path p = fs::temp_directory_path() / ("emberworld_test_" + name);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

// Writes an uncompressed little-endian float32 (sample_format 3) or uint16 (1) TIFF with
// GeoTIFF pixel scale + tiepoint and a GDAL_NODATA string. rows_per_strip < h -> multi-strip;
// tile > 0 -> tiled layout instead of strips.
inline void write_tiff(const fs::path& path, int w, int h, const std::vector<double>& vals,
                       int sample_format, int bits, double ox, double oy, double px,
                       const char* nodata, int rows_per_strip, int tile = 0) {
    const int bps = bits / 8;
    std::vector<uint8_t> pix;
    auto put = [&](double v) {
        uint8_t b[8];
        if (sample_format == 3 && bits == 32) { float f = static_cast<float>(v); std::memcpy(b, &f, 4); }
        else if (bits == 16) { uint16_t u = static_cast<uint16_t>(v); std::memcpy(b, &u, 2); }
        else { uint8_t u = static_cast<uint8_t>(v); b[0] = u; }
        pix.insert(pix.end(), b, b + bps);
    };
    std::vector<std::pair<uint32_t, uint32_t>> chunks;  // (offset-in-pix, bytes)
    if (tile > 0) {
        const int across = (w + tile - 1) / tile, down = (h + tile - 1) / tile;
        for (int ty = 0; ty < down; ++ty)
            for (int tx = 0; tx < across; ++tx) {
                const uint32_t start = static_cast<uint32_t>(pix.size());
                for (int y = 0; y < tile; ++y)
                    for (int x = 0; x < tile; ++x) {
                        const int gx = tx * tile + x, gy = ty * tile + y;
                        put(gx < w && gy < h ? vals[static_cast<size_t>(gy) * w + gx] : 0.0);
                    }
                chunks.push_back({start, static_cast<uint32_t>(pix.size()) - start});
            }
    } else {
        for (int y0 = 0; y0 < h; y0 += rows_per_strip) {
            const uint32_t start = static_cast<uint32_t>(pix.size());
            for (int y = y0; y < std::min(h, y0 + rows_per_strip); ++y)
                for (int x = 0; x < w; ++x) put(vals[static_cast<size_t>(y) * w + x]);
            chunks.push_back({start, static_cast<uint32_t>(pix.size()) - start});
        }
    }
    // Layout: header(8) | pixels | offsets[] | counts[] | doubles(scale 3, tie 6) | nodata | IFD
    std::vector<uint8_t> out(8);
    out[0] = 'I'; out[1] = 'I'; out[2] = 42; out[3] = 0;
    const uint32_t pix_off = 8;
    out.insert(out.end(), pix.begin(), pix.end());
    auto align = [&]() { while (out.size() % 4) out.push_back(0); };
    auto u32v = [&](uint32_t v) { uint8_t b[4]; std::memcpy(b, &v, 4); out.insert(out.end(), b, b + 4); };
    auto f64v = [&](double v) { uint8_t b[8]; std::memcpy(b, &v, 8); out.insert(out.end(), b, b + 8); };
    align();
    const uint32_t offs_at = static_cast<uint32_t>(out.size());
    for (auto& c : chunks) u32v(pix_off + c.first);
    const uint32_t cnts_at = static_cast<uint32_t>(out.size());
    for (auto& c : chunks) u32v(c.second);
    const uint32_t scale_at = static_cast<uint32_t>(out.size());
    f64v(px); f64v(px); f64v(0);
    const uint32_t tie_at = static_cast<uint32_t>(out.size());
    f64v(0); f64v(0); f64v(0); f64v(ox); f64v(oy); f64v(0);
    const uint32_t nd_at = static_cast<uint32_t>(out.size());
    const std::string nd = std::string(nodata) + '\0';
    out.insert(out.end(), nd.begin(), nd.end());
    align();
    const uint32_t ifd_at = static_cast<uint32_t>(out.size());
    std::memcpy(out.data() + 4, &ifd_at, 4);
    struct E { uint16_t tag, type; uint32_t count, value; };
    const uint32_t n = static_cast<uint32_t>(chunks.size());
    std::vector<E> es = {{256, 4, 1, (uint32_t)w}, {257, 4, 1, (uint32_t)h}, {258, 3, 1, (uint32_t)bits},
                         {259, 3, 1, 1}, {262, 3, 1, 1}, {277, 3, 1, 1}, {284, 3, 1, 1},
                         {339, 3, 1, (uint32_t)sample_format}};
    if (tile > 0) {
        es.push_back({322, 3, 1, (uint32_t)tile});
        es.push_back({323, 3, 1, (uint32_t)tile});
        es.push_back({324, 4, n, n == 1 ? pix_off + chunks[0].first : offs_at});
        es.push_back({325, 4, n, n == 1 ? chunks[0].second : cnts_at});
    } else {
        es.push_back({273, 4, n, n == 1 ? pix_off + chunks[0].first : offs_at});
        es.push_back({278, 4, 1, (uint32_t)rows_per_strip});
        es.push_back({279, 4, n, n == 1 ? chunks[0].second : cnts_at});
    }
    es.push_back({33550, 12, 3, scale_at});
    es.push_back({33922, 12, 6, tie_at});
    es.push_back({42113, 2, (uint32_t)nd.size(), (uint32_t)nd_at});
    std::sort(es.begin(), es.end(), [](const E& a, const E& b) { return a.tag < b.tag; });
    uint16_t cnt = static_cast<uint16_t>(es.size());
    uint8_t b2[2]; std::memcpy(b2, &cnt, 2); out.insert(out.end(), b2, b2 + 2);
    for (auto& e : es) {
        uint8_t b[12];
        std::memcpy(b, &e.tag, 2); std::memcpy(b + 2, &e.type, 2); std::memcpy(b + 4, &e.count, 4);
        if (e.type == 2 && e.count <= 4) {  // inline ASCII
            std::memset(b + 8, 0, 4);
            std::memcpy(b + 8, nd.data(), e.count);
        } else if (e.type == 3 && e.count == 1) {
            uint16_t s = static_cast<uint16_t>(e.value); std::memset(b + 8, 0, 4); std::memcpy(b + 8, &s, 2);
        } else {
            std::memcpy(b + 8, &e.value, 4);
        }
        out.insert(out.end(), b, b + 12);
    }
    u32v(0);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

// A synthetic region: 2x2 tiles at lod 5 + 1 tile at lod 4, tile_px 8, overlap 3, 10 m at
// lod 5, heights from `hf(x, y)` in world metres. Apron pixels outside the region are nodata.
inline fs::path write_synth_region(const std::string& name,
                                   const std::function<double(double, double)>& hf) {
    const fs::path root = temp_dir(name);
    const int tp = 8, ov = 3;
    const double ox = 500000.0, oy = 5200000.0;  // lower-left
    const double region_w = 2 * tp * 10.0;       // 160 m square
    std::string tiles_json, first = "";
    auto add_tile = [&](int lod, int x, int y, double res) {
        const double span = tp * res;
        const double cminx = ox + x * span, cminy = oy + y * span;
        const double aminx = cminx - ov * res, amaxy = cminy + span + ov * res;
        const int n = tp + 2 * ov;
        std::vector<double> v(static_cast<size_t>(n) * n);
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) {
                const double wx = aminx + (c + 0.5) * res, wy = amaxy - (r + 0.5) * res;
                const bool inside = wx > ox && wx < ox + region_w && wy > oy && wy < oy + region_w;
                v[static_cast<size_t>(r) * n + c] = inside ? hf(wx, wy) : -9999.0;
            }
        const fs::path dir = root / "tiles" / ("z" + std::to_string(lod)) / ("x" + std::to_string(x)) /
                             ("y" + std::to_string(y));
        fs::create_directories(dir);
        write_tiff(dir / "height.tif", n, n, v, 3, 32, aminx, amaxy, res, "-9999", 5);
        char buf[1024];
        std::snprintf(buf, sizeof buf,
                      "%s{\"lod\":%d,\"x\":%d,\"y\":%d,\"content_bounds\":[%.6f,%.6f,%.6f,%.6f],"
                      "\"apron_bounds\":[%.6f,%.6f,%.6f,%.6f],"
                      "\"height_tif\":\"tiles\\\\z%d\\\\x%d\\\\y%d\\\\height.tif\",\"hash\":\"h\"}",
                      first.c_str(), lod, x, y, cminx, cminy, cminx + span, cminy + span, aminx,
                      cminy - ov * res, cminx + span + ov * res, amaxy, lod, x, y);
        tiles_json += buf;
        first = ",";
    };
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) add_tile(5, x, y, 10.0);
    add_tile(4, 0, 0, 20.0);
    std::ofstream m(root / "manifest.json");
    m << "{\"manifest_schema_version\":2,\"crs\":\"EPSG:32610\",\"tile_px\":" << tp
      << ",\"overlap_px\":" << ov << ",\"lods\":[5,4],\"tile_count\":5,"
      << "\"heightmap\":{\"bits\":16,\"scale\":0.01,\"offset\":0,\"z_min\":0,\"z_max\":500},"
      << "\"layers\":{},\"tiles\":[" << tiles_json << "]}";
    return root;
}

}  // namespace testutil

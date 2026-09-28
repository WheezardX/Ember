#include "emberworld/look.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "emberworld/scatter.h"
#include "emberworld/tiff.h"
#include "toml.hpp"

namespace emberworld {

LandClass classify_fbfm40(int c) {
    if (c == 91) return LandClass::Urban;
    if (c == 92) return LandClass::Snow;
    if (c == 93) return LandClass::Agriculture;
    if (c == 98) return LandClass::Water;
    if (c == 99) return LandClass::Barren;
    if (c >= 101 && c <= 109) return LandClass::Grass;
    if (c >= 121 && c <= 124) return LandClass::GrassShrub;
    if (c >= 141 && c <= 149) return LandClass::Shrub;
    if (c >= 161 && c <= 165) return LandClass::TimberUnderstory;
    if (c >= 181 && c <= 189) return LandClass::TimberLitter;
    if (c >= 201 && c <= 204) return LandClass::SlashBlowdown;
    return LandClass::Unknown;
}

bool is_vegetated(LandClass c) {
    switch (c) {
        case LandClass::Grass: case LandClass::GrassShrub: case LandClass::Shrub:
        case LandClass::TimberUnderstory: case LandClass::TimberLitter:
        case LandClass::SlashBlowdown: case LandClass::Agriculture:
            return true;
        default:
            return false;
    }
}

namespace {

float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

Rgb lerp(const Rgb& a, const Rgb& b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

const char* kClassKeys[] = {"unknown", "urban", "snow", "agriculture", "water", "barren", "grass",
                            "grass_shrub", "shrub", "timber_understory", "timber_litter",
                            "slash_blowdown"};

}  // namespace

bool parse_srgb_hex(const std::string& hex, Rgb& out) {
    if (hex.size() != 7 || hex[0] != '#') return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(hex.c_str() + 1, &end, 16);
    if (end != hex.c_str() + 7) return false;
    out.r = srgb_to_linear(((v >> 16) & 0xFF) / 255.0f);
    out.g = srgb_to_linear(((v >> 8) & 0xFF) / 255.0f);
    out.b = srgb_to_linear((v & 0xFF) / 255.0f);
    return true;
}

uint8_t linear_to_srgb8(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
}

LookResult load_look(const std::string& path) {
    LookResult res;
    try {
        toml::table t = toml::parse_file(path);
        if (t["look_version"].value_or(0) != 1) {
            res.error = path + ": look_version must be 1";
            return res;
        }
        TerrainLook& L = res.look;
        L.name = t["name"].value_or(std::string{});
        for (size_t i = 0; i < static_cast<size_t>(LandClass::Count); ++i) {
            const std::string hex = t["classes"][kClassKeys[i]].value_or(std::string{});
            if (!parse_srgb_hex(hex, L.classes[i])) {
                res.error = path + ": classes." + kClassKeys[i] + " missing or not #RRGGBB";
                return res;
            }
        }
        auto veg = t["vegetation"];
        if (!parse_srgb_hex(veg["green"].value_or(std::string{}), L.green)) {
            res.error = path + ": vegetation.green";
            return res;
        }
        L.ndvi_low = veg["ndvi_low"].value_or(L.ndvi_low);
        L.ndvi_high = veg["ndvi_high"].value_or(L.ndvi_high);
        L.green_strength = veg["green_strength"].value_or(L.green_strength);
        L.canopy_darken = veg["canopy_darken"].value_or(L.canopy_darken);
        auto sl = t["slope"];
        if (!parse_srgb_hex(sl["rock"].value_or(std::string{}), L.rock)) {
            res.error = path + ": slope.rock";
            return res;
        }
        L.slope_start_deg = sl["start_deg"].value_or(L.slope_start_deg);
        L.slope_full_deg = sl["full_deg"].value_or(L.slope_full_deg);
        auto de = t["detail"];
        L.supersample = static_cast<int>(std::clamp<int64_t>(de["supersample"].value_or(int64_t{L.supersample}), 1, 16));
        L.boundary_warp_px = de["boundary_warp_px"].value_or(L.boundary_warp_px);
        L.blur_radius_px = de["blur_radius_px"].value_or(L.blur_radius_px);
    } catch (const std::exception& e) {
        res.error = path + ": " + e.what();
    }
    return res;
}

namespace {

// Per source pixel: colour before slope (class + NDVI + canopy) and rock blend factor.
struct SourcePx {
    Rgb c;
    float rock = 0;
    bool valid = false;
};

// Smooth value noise in [-1, 1] on a unit lattice, world-aligned (x, y in source-pixel units).
double value_noise(double x, double y, uint64_t salt) {
    const double fx = std::floor(x), fy = std::floor(y);
    const double tx = x - fx, ty = y - fy;
    const double sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
    auto lat = [&](double ix, double iy) {
        const uint64_t h = scatter::hash64({salt, static_cast<uint64_t>(static_cast<int64_t>(ix)),
                                            static_cast<uint64_t>(static_cast<int64_t>(iy))});
        return scatter::u01(h) * 2.0 - 1.0;
    };
    const double a = lat(fx, fy), b = lat(fx + 1, fy), c = lat(fx, fy + 1), d = lat(fx + 1, fy + 1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

}  // namespace

static Albedo compose_albedo_native(const TerrainLook& L, const LookInputs& in, std::vector<SourcePx>* src_out);

Albedo compose_albedo(const TerrainLook& L, const LookInputs& in) {
    const int s = std::max(1, L.supersample);
    std::vector<SourcePx> src;
    Albedo native = compose_albedo_native(L, in, &src);
    if (s == 1 && L.boundary_warp_px <= 0 && L.blur_radius_px <= 0) return native;
    const int W = in.width, H = in.height, FW = W * s, FH = H * s;
    Albedo fine;
    fine.width = FW;
    fine.height = FH;
    std::vector<Rgb> col(static_cast<size_t>(FW) * FH);
    std::vector<uint8_t> valid(static_cast<size_t>(FW) * FH, 0);
    const double ox = in.origin_x_m / in.pixel_m, oy = -in.origin_y_m / in.pixel_m;  // world px, y south
    auto rock_at = [&](double x, double y) {  // bilinear over source pixel centres, valid only
        const double fx = std::clamp(x, 0.0, W - 1.0), fy = std::clamp(y, 0.0, H - 1.0);
        const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
        const int x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
        const double tx = fx - x0, ty = fy - y0;
        double acc = 0, w = 0;
        const int xs[2] = {x0, x1}, ys[2] = {y0, y1};
        const double wx[2] = {1 - tx, tx}, wy[2] = {1 - ty, ty};
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const SourcePx& p = src[static_cast<size_t>(ys[j]) * W + xs[i]];
                if (!p.valid) continue;
                acc += p.rock * wx[i] * wy[j];
                w += wx[i] * wy[j];
            }
        return w > 0 ? static_cast<float>(acc / w) : 0.0f;
    };
    for (int fy = 0; fy < FH; ++fy)
        for (int fx = 0; fx < FW; ++fx) {
            const double sx = (fx + 0.5) / s - 0.5, sy = (fy + 0.5) / s - 0.5;  // source px coords
            // alpha follows the un-warped DEM validity so geometry and texture agree
            const int nx = std::clamp(static_cast<int>(std::lround(sx)), 0, W - 1);
            const int ny = std::clamp(static_cast<int>(std::lround(sy)), 0, H - 1);
            if (!src[static_cast<size_t>(ny) * W + nx].valid) continue;
            const double wxn = ox + sx, wyn = oy + sy;
            const double dx = L.boundary_warp_px * (value_noise(wxn * 0.9, wyn * 0.9, 11) * 0.7 +
                                                    value_noise(wxn * 2.3, wyn * 2.3, 12) * 0.3);
            const double dy = L.boundary_warp_px * (value_noise(wxn * 0.9, wyn * 0.9, 13) * 0.7 +
                                                    value_noise(wxn * 2.3, wyn * 2.3, 14) * 0.3);
            int wx = std::clamp(static_cast<int>(std::lround(sx + dx)), 0, W - 1);
            int wy = std::clamp(static_cast<int>(std::lround(sy + dy)), 0, H - 1);
            if (!src[static_cast<size_t>(wy) * W + wx].valid) {
                wx = nx;
                wy = ny;
            }
            const SourcePx& p = src[static_cast<size_t>(wy) * W + wx];
            const size_t k = static_cast<size_t>(fy) * FW + fx;
            col[k] = lerp(p.c, L.rock, rock_at(sx, sy));
            valid[k] = 1;
        }
    const int radius = static_cast<int>(std::lround(L.blur_radius_px * s));
    if (radius > 0) {
        // Separable box blur over valid texels only (nodata never bleeds in), two passes.
        std::vector<Rgb> tmp(col.size());
        auto pass = [&](bool horizontal, const std::vector<Rgb>& from, std::vector<Rgb>& to) {
            for (int fy = 0; fy < FH; ++fy)
                for (int fx = 0; fx < FW; ++fx) {
                    const size_t k = static_cast<size_t>(fy) * FW + fx;
                    if (!valid[k]) continue;
                    Rgb acc{0, 0, 0};
                    int n = 0;
                    for (int d = -radius; d <= radius; ++d) {
                        const int xx = horizontal ? fx + d : fx, yy = horizontal ? fy : fy + d;
                        if (xx < 0 || yy < 0 || xx >= FW || yy >= FH) continue;
                        const size_t q = static_cast<size_t>(yy) * FW + xx;
                        if (!valid[q]) continue;
                        acc.r += from[q].r;
                        acc.g += from[q].g;
                        acc.b += from[q].b;
                        ++n;
                    }
                    to[k] = {acc.r / n, acc.g / n, acc.b / n};
                }
        };
        for (int it = 0; it < 2; ++it) {
            pass(true, col, tmp);
            pass(false, tmp, col);
        }
    }
    fine.bgra.assign(static_cast<size_t>(FW) * FH * 4, 0);
    for (int fy = 0; fy < FH; ++fy)
        for (int fx = 0; fx < FW; ++fx) {
            const size_t k = static_cast<size_t>(fy) * FW + fx;
            if (!valid[k]) continue;
            const Rgb c = col[k];
            uint8_t* o = &fine.bgra[k * 4];
            o[0] = linear_to_srgb8(c.b);
            o[1] = linear_to_srgb8(c.g);
            o[2] = linear_to_srgb8(c.r);
            o[3] = 255;
        }
    return fine;
}

static Albedo compose_albedo_native(const TerrainLook& L, const LookInputs& in, std::vector<SourcePx>* src_out) {
    Albedo out;
    out.width = in.width;
    out.height = in.height;
    const int W = in.width, H = in.height;
    out.bgra.assign(static_cast<size_t>(W) * H * 4, 0);
    if (src_out) src_out->assign(static_cast<size_t>(W) * H, SourcePx{});
    auto dem = [&](int x, int y) -> float {
        x = std::clamp(x, 0, W - 1);
        y = std::clamp(y, 0, H - 1);
        return in.dem[static_cast<size_t>(y) * W + x];
    };
    const double deg = 180.0 / 3.141592653589793;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t k = static_cast<size_t>(y) * W + x;
            const float z = in.dem[k];
            if (!std::isfinite(z)) continue;  // alpha stays 0
            const LandClass cls = classify_fbfm40(in.fbfm40[k]);
            Rgb c = L.classes[static_cast<size_t>(cls)];
            if (is_vegetated(cls)) {
                if (!in.ndvi.empty() && std::isfinite(in.ndvi[k])) {
                    const double t = std::clamp((in.ndvi[k] - L.ndvi_low) / (L.ndvi_high - L.ndvi_low), 0.0, 1.0);
                    c = lerp(c, L.green, static_cast<float>(t * L.green_strength));
                }
                const float cc = in.cc[k];
                if (cc > 0) {
                    const float f = 1.0f - static_cast<float>(L.canopy_darken) * std::min(cc, 100.0f) / 100.0f;
                    c = {c.r * f, c.g * f, c.b * f};
                }
            }
            const Rgb pre_rock = c;
            float rock_t = 0;
            if (cls != LandClass::Water && cls != LandClass::Snow) {
                // Central differences; neighbours that are nodata fall back to this pixel.
                auto zz = [&](int xx, int yy) {
                    const float v = dem(xx, yy);
                    return std::isfinite(v) ? v : z;
                };
                const double gx = (zz(x + 1, y) - zz(x - 1, y)) / (2.0 * in.pixel_m);
                const double gy = (zz(x, y + 1) - zz(x, y - 1)) / (2.0 * in.pixel_m);
                const double slope = std::atan(std::sqrt(gx * gx + gy * gy)) * deg;
                const double t = std::clamp((slope - L.slope_start_deg) / (L.slope_full_deg - L.slope_start_deg), 0.0, 1.0);
                rock_t = static_cast<float>(t);
                c = lerp(c, L.rock, rock_t);
            }
            if (src_out) (*src_out)[k] = SourcePx{pre_rock, rock_t, true};
            uint8_t* p = &out.bgra[k * 4];
            p[0] = linear_to_srgb8(c.b);
            p[1] = linear_to_srgb8(c.g);
            p[2] = linear_to_srgb8(c.r);
            p[3] = 255;
        }
    return out;
}

bool load_look_inputs(const Region& region, const TileEntry& tile, LookInputs& out, std::string& error) {
    auto read = [&](const std::string& rel, Raster& r) {
        TiffResult t = read_tiff(region.path(rel));
        if (!t) {
            error = t.error.message;
            return false;
        }
        r = std::move(*t.raster);
        return true;
    };
    auto layer = [&](const char* name) -> const LayerTile* {
        auto it = region.layers.find(name);
        return it == region.layers.end() ? nullptr : it->second.find(tile.lod, tile.x, tile.y);
    };
    Raster h;
    if (!read(tile.height_tif, h)) return false;
    out.width = h.width;
    out.height = h.height;
    out.pixel_m = tile.content.width() / region.tile_px;
    out.origin_x_m = tile.apron.min_x;
    out.origin_y_m = tile.apron.max_y;
    const size_t n = static_cast<size_t>(h.width) * h.height;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    out.dem.resize(n);
    for (int y = 0; y < h.height; ++y)
        for (int x = 0; x < h.width; ++x) {
            const double v = h.at(x, y);
            out.dem[static_cast<size_t>(y) * h.width + x] = h.is_nodata(v) ? nan : static_cast<float>(v);
        }
    out.fbfm40.assign(n, 0);
    out.cc.assign(n, -1.0f);
    out.ndvi.clear();
    Raster r;
    auto same_size = [&](const Raster& q) { return q.width == h.width && q.height == h.height; };
    if (const LayerTile* lt = layer("fuels_fbfm40"); lt && read(lt->path, r) && same_size(r))
        for (size_t i = 0; i < n; ++i) out.fbfm40[i] = static_cast<int32_t>(r.at(static_cast<int>(i % h.width), static_cast<int>(i / h.width)));
    if (const LayerTile* lt = layer("fuels_cc"); lt && read(lt->path, r) && same_size(r))
        for (size_t i = 0; i < n; ++i) {
            const double v = r.at(static_cast<int>(i % h.width), static_cast<int>(i / h.width));
            out.cc[i] = r.is_nodata(v) ? -1.0f : static_cast<float>(v);
        }
    if (const LayerTile* lt = layer("season_greenness"); lt && read(lt->path, r) && same_size(r)) {
        out.ndvi.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const double v = r.at(static_cast<int>(i % h.width), static_cast<int>(i / h.width));
            out.ndvi[i] = r.is_nodata(v) ? nan : static_cast<float>(v);
        }
    }
    error.clear();  // optional layers may have failed to read; the DEM is what is required
    return true;
}

}  // namespace emberworld

namespace emberworld {

std::vector<Albedo> build_mips(const Albedo& level0) {
    std::vector<Albedo> mips{level0};
    while (mips.back().width > 1 || mips.back().height > 1) {
        const Albedo& s = mips.back();
        Albedo d;
        d.width = std::max(1, s.width / 2);
        d.height = std::max(1, s.height / 2);
        d.bgra.assign(static_cast<size_t>(d.width) * d.height * 4, 0);
        for (int y = 0; y < d.height; ++y)
            for (int x = 0; x < d.width; ++x) {
                // Source footprint; the last destination row/column absorbs an odd remainder.
                const int x0 = 2 * x, x1 = (x == d.width - 1) ? s.width : std::min(s.width, 2 * x + 2);
                const int y0 = 2 * y, y1 = (y == d.height - 1) ? s.height : std::min(s.height, 2 * y + 2);
                double acc[3] = {0, 0, 0}, wsum = 0;
                int n = 0;
                for (int yy = y0; yy < y1; ++yy)
                    for (int xx = x0; xx < x1; ++xx) {
                        const uint8_t* p = &s.bgra[(static_cast<size_t>(yy) * s.width + xx) * 4];
                        const double w = p[3] / 255.0;
                        acc[0] += p[0] * w;
                        acc[1] += p[1] * w;
                        acc[2] += p[2] * w;
                        wsum += w;
                        ++n;
                    }
                uint8_t* q = &d.bgra[(static_cast<size_t>(y) * d.width + x) * 4];
                if (wsum > 0) {
                    for (int c = 0; c < 3; ++c) q[c] = static_cast<uint8_t>(std::lround(acc[c] / wsum));
                    q[3] = static_cast<uint8_t>(std::lround(255.0 * wsum / n));
                }
            }
        mips.push_back(std::move(d));
    }
    return mips;
}

}  // namespace emberworld

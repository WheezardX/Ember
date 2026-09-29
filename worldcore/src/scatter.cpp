#include "emberworld/scatter.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <exception>

#include "toml.hpp"

// Cosmetic attributes must evaluate exactly like CPython doubles: no fused multiply-add.
// (GCC ignores the STDC pragma; worldcore's CMake passes -ffp-contract=off instead.)
#if defined(_MSC_VER) && !defined(__clang__)
#pragma fp_contract(off)
#elif defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

namespace emberworld::scatter {

namespace {
constexpr uint64_t kGolden = 0x9E3779B97F4A7C15ULL;
constexpr double kPi = 3.141592653589793;  // == Python math.pi
// Attempt sub-keys (terrain/veg/scatter.py).
constexpr uint64_t kAccept = 1, kSpecies = 2, kJx = 3, kJy = 4, kHeight = 5, kYaw = 6;
constexpr uint64_t kClass = 7, kCrown = 8;
}  // namespace

uint64_t splitmix64(uint64_t x) {
    uint64_t z = x + kGolden;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

uint64_t hash64(std::initializer_list<uint64_t> values) {
    uint64_t h = 0;
    for (uint64_t v : values) h = splitmix64(h ^ v);
    return h;
}

double u01(uint64_t h) { return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0); }

int weighted_pick(uint64_t h, std::span<const int64_t> weights) {
    uint64_t total = 0;
    for (int64_t w : weights) total += static_cast<uint64_t>(w);
    if (total == 0) return 0;
    const uint64_t r = h % total;
    uint64_t acc = 0;
    for (size_t i = 0; i < weights.size(); ++i) {
        acc += static_cast<uint64_t>(weights[i]);
        if (r < acc) return static_cast<int>(i);
    }
    return static_cast<int>(weights.size()) - 1;
}

const Group& Palette::group_for_evt(int64_t evt) const {
    for (const Group& g : groups)
        for (int64_t code : g.evt_codes)
            if (code == evt) return g;
    for (const Group& g : groups)
        if (g.has_range && g.evt_min <= evt && evt <= g.evt_max) return g;
    for (const Group& g : groups)
        if (g.name == default_group) return g;
    return groups.front();
}

std::vector<const Species*> Palette::species_index() const {
    std::vector<const Species*> out;
    for (const Group& g : groups)
        for (const Species& s : g.species) out.push_back(&s);
    return out;
}

void Palette::finalize() {
    class_weights.clear();
    for (const CrownClass& c : structure) class_weights.push_back(c.weight);
    int off = 0;
    for (Group& g : groups) {
        g.offset = off;
        g.weights.clear();
        for (const Species& s : g.species) g.weights.push_back(s.weight);
        off += static_cast<int>(g.species.size());
    }
}

PaletteResult load_palette(const std::string& path) {
    PaletteResult res;
    try {
        toml::table t = toml::parse_file(path);
        Palette& p = res.palette;
        p.name = t["name"].value_or(std::string{});
        p.default_group = t["default_group"].value_or(std::string{});
        p.crown_base_m = t["crown_base_m"].value_or(0.0);
        if (const toml::array* st = t["structure"].as_array()) {
            for (const auto& cn : *st) {
                const toml::table* ct = cn.as_table();
                if (!ct) continue;
                CrownClass c;
                c.name = (*ct)["name"].value_or(std::string{});
                c.weight = (*ct)["weight"].value_or(int64_t{1});
                c.h_lo = (*ct)["h_lo"].value_or(1.0);
                c.h_hi = (*ct)["h_hi"].value_or(1.0);
                p.structure.push_back(std::move(c));
            }
        }
        if (p.structure.empty()) {
            res.error = path + ": palette has no [[structure]] (schema 2 / scatter v2)";
            return res;
        }
        const toml::array* groups = t["groups"].as_array();
        if (!groups || groups->empty()) {
            res.error = path + ": palette has no [[groups]]";
            return res;
        }
        for (const auto& gn : *groups) {
            const toml::table* gt = gn.as_table();
            if (!gt) continue;
            Group g;
            g.name = (*gt)["name"].value_or(std::string{});
            if (const toml::array* codes = (*gt)["evt_codes"].as_array())
                for (const auto& cn : *codes) g.evt_codes.push_back(cn.value_or(int64_t{-1}));
            const auto lo = (*gt)["evt_min"].value<int64_t>();
            const auto hi = (*gt)["evt_max"].value<int64_t>();
            g.has_range = lo.has_value() && hi.has_value();
            g.evt_min = lo.value_or(0);
            g.evt_max = hi.value_or(0);
            if (const toml::array* sp = (*gt)["species"].as_array()) {
                for (const auto& sn : *sp) {
                    const toml::table* st = sn.as_table();
                    if (!st) continue;
                    Species s;
                    s.key = (*st)["key"].value_or(std::string{});
                    s.weight = (*st)["weight"].value_or(int64_t{1});
                    s.height_min_m = (*st)["height_min_m"].value_or(1.0);
                    s.height_max_m = (*st)["height_max_m"].value_or(1.0);
                    s.crown_ratio = (*st)["crown_ratio"].value_or(0.25);
                    g.species.push_back(std::move(s));
                }
            }
            if (g.species.empty()) {
                res.error = path + ": group '" + g.name + "' has no species";
                return res;
            }
            p.groups.push_back(std::move(g));
        }
        p.finalize();
    } catch (const std::exception& e) {
        res.error = path + ": " + e.what();
    }
    return res;
}

double tree_ground(std::span<const float> dem, int rows, int cols, int rr, int cc, double jx,
                   double jy, double z_cell, double dem_nodata) {
    int dc, dr;
    double tx, ty;
    if (jx < 0.5) { dc = -1; tx = jx + 0.5; } else { dc = 0; tx = jx - 0.5; }
    if (jy < 0.5) { dr = -1; ty = jy + 0.5; } else { dr = 0; ty = jy - 0.5; }
    const int ra = rr + dr, ca = cc + dc;
    if (ra < 0 || ca < 0 || ra + 1 >= rows || ca + 1 >= cols) return z_cell;
    auto at = [&](int r, int c) { return static_cast<double>(dem[static_cast<size_t>(r) * cols + c]); };
    const double z00 = at(ra, ca), z01 = at(ra, ca + 1), z10 = at(ra + 1, ca), z11 = at(ra + 1, ca + 1);
    if (z00 == dem_nodata || z01 == dem_nodata || z10 == dem_nodata || z11 == dem_nodata) return z_cell;
    // same order of operations as Python (no contraction: each product rounds on its own)
    const double top = z00 * (1.0 - tx) + z01 * tx;
    const double bot = z10 * (1.0 - tx) + z11 * tx;
    return top * (1.0 - ty) + bot * ty;
}

void scatter_window(const Palette& palette, const Params& p, int r0, int c0, int rows, int cols,
                    std::span<const float> cc, std::span<const float> height,
                    std::span<const int32_t> evt, std::span<const float> dem,
                    std::vector<Instance>& out, int dem_halo) {
    const int drows = rows + 2 * dem_halo, dcols = cols + 2 * dem_halo;
    // Python's round() is round-half-even; nearbyint under the default rounding mode matches.
    const int saved_round = std::fegetround();
    std::fesetround(FE_TONEAREST);
    for (int rr = 0; rr < rows; ++rr) {
        const uint64_t r = static_cast<uint64_t>(r0 + rr);
        for (int cc_i = 0; cc_i < cols; ++cc_i) {
            const size_t k = static_cast<size_t>(rr) * cols + cc_i;
            const double cc_val = cc[k];
            if (cc_val <= 0 || cc_val == p.cc_nodata) continue;
            const int64_t cc_permyriad =
                std::min<int64_t>(10000, static_cast<int64_t>(std::nearbyint(cc_val * 100)));
            const Group& group = palette.group_for_evt(evt[k]);
            const uint64_t c = static_cast<uint64_t>(c0 + cc_i);
            const uint64_t cell_seed = hash64({p.tile_seed, r, c});
            const double hv = height[k];
            const double zc =
                dem[static_cast<size_t>(rr + dem_halo) * dcols + (cc_i + dem_halo)];
            const double z = (zc == p.dem_nodata) ? 0.0 : zc;

            for (int a_i = 0; a_i < p.candidates_per_cell; ++a_i) {
                const uint64_t a = static_cast<uint64_t>(a_i);
                if (static_cast<int64_t>(hash64({cell_seed, a, kAccept}) % 10000) >= cc_permyriad)
                    continue;
                const int sp_local = weighted_pick(hash64({cell_seed, a, kSpecies}), group.weights);
                const Species& sp = group.species[static_cast<size_t>(sp_local)];

                const double jx = u01(hash64({cell_seed, a, kJx}));
                const double jy = u01(hash64({cell_seed, a, kJy}));
                Instance in;
                in.x = p.x0 + (static_cast<double>(c) + jx) * p.cell_size;
                in.y = p.y_top - (static_cast<double>(r) + jy) * p.cell_size;
                in.z = p.ground_tree ? tree_ground(dem, drows, dcols, rr + dem_halo,
                                                   cc_i + dem_halo, jx, jy, z, p.dem_nodata)
                                     : z;

                // Stand structure: crown class -> fraction of the canopy-top height.
                const CrownClass& cls = palette.structure[static_cast<size_t>(
                    weighted_pick(hash64({cell_seed, a, kClass}), palette.class_weights))];
                const double hfrac = u01(hash64({cell_seed, a, kHeight}));
                double h_m;
                if (hv > 0 && hv != p.height_nodata) {
                    const double ceiling = std::min(hv, sp.height_max_m);
                    h_m = ceiling * (cls.h_lo + (cls.h_hi - cls.h_lo) * hfrac);
                } else {
                    h_m = sp.height_min_m + hfrac * (sp.height_max_m - sp.height_min_m);
                }
                h_m = std::max(h_m, sp.height_min_m);

                in.yaw_rad = u01(hash64({cell_seed, a, kYaw})) * 2.0 * kPi;
                in.scale = h_m / (0.5 * (sp.height_min_m + sp.height_max_m));
                const double crown = 0.85 + 0.30 * u01(hash64({cell_seed, a, kCrown}));
                in.radius_m = 0.5 * (palette.crown_base_m + sp.crown_ratio * h_m) * crown;
                in.height_m = h_m;
                in.species = group.offset + sp_local;
                out.push_back(in);
            }
        }
    }
    std::fesetround(saved_round);
}

}  // namespace emberworld::scatter

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
        if (g.evt_min <= evt && evt <= g.evt_max) return g;
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
            g.evt_min = (*gt)["evt_min"].value_or(int64_t{0});
            g.evt_max = (*gt)["evt_max"].value_or(int64_t{0});
            if (const toml::array* sp = (*gt)["species"].as_array()) {
                for (const auto& sn : *sp) {
                    const toml::table* st = sn.as_table();
                    if (!st) continue;
                    Species s;
                    s.key = (*st)["key"].value_or(std::string{});
                    s.weight = (*st)["weight"].value_or(int64_t{1});
                    s.height_min_m = (*st)["height_min_m"].value_or(1.0);
                    s.height_max_m = (*st)["height_max_m"].value_or(1.0);
                    s.radius_m = (*st)["radius_m"].value_or(1.0);
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

void scatter_window(const Palette& palette, const Params& p, int r0, int c0, int rows, int cols,
                    std::span<const float> cc, std::span<const float> height,
                    std::span<const int32_t> evt, std::span<const float> dem,
                    std::vector<Instance>& out) {
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
            const double zc = dem[k];
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
                in.z = z;

                const double hfrac = u01(hash64({cell_seed, a, kHeight}));
                double h_m;
                if (hv > 0 && hv != p.cc_nodata) {  // sic: upstream compares height to cc_nodata
                    h_m = std::min(std::max(hv, sp.height_min_m), sp.height_max_m);
                    h_m *= 0.85 + 0.30 * hfrac;
                } else {
                    h_m = sp.height_min_m + hfrac * (sp.height_max_m - sp.height_min_m);
                }
                h_m = std::min(std::max(h_m, sp.height_min_m), sp.height_max_m);

                in.yaw_rad = u01(hash64({cell_seed, a, kYaw})) * 2.0 * kPi;
                in.scale = h_m / (0.5 * (sp.height_min_m + sp.height_max_m));
                in.height_m = h_m;
                in.species = group.offset + sp_local;
                in.radius_m = sp.radius_m;
                out.push_back(in);
            }
        }
    }
    std::fesetround(saved_round);
}

}  // namespace emberworld::scatter

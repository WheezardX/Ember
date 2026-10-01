#include "emberworld/cover.h"

#include <algorithm>
#include <cmath>

#include "emberworld/scatter.h"
#include "toml.hpp"

namespace emberworld {

namespace {

constexpr const char* kCoverSets[4] = {"litter", "grass", "rock", "shrub"};
constexpr uint64_t kCoverCand = 0xC0DE01, kCoverItem = 0xC0DE02, kCoverJx = 0xC0DE03, kCoverJy = 0xC0DE04,
                   kCoverVar = 0xC0DE05, kCoverH = 0xC0DE06, kCoverYaw = 0xC0DE07;

// Value noise in [0, 1] on the integer lattice of (x, y) / scale, smoothstep-interpolated.
double value_noise(uint64_t seed, double x, double y) {
    const double fx = std::floor(x), fy = std::floor(y);
    const int64_t ix = static_cast<int64_t>(fx), iy = static_cast<int64_t>(fy);
    double tx = x - fx, ty = y - fy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    auto h = [&](int64_t a, int64_t b) {
        return scatter::u01(scatter::hash64({seed, static_cast<uint64_t>(a), static_cast<uint64_t>(b)}));
    };
    const double a = h(ix, iy) + (h(ix + 1, iy) - h(ix, iy)) * tx;
    const double b = h(ix, iy + 1) + (h(ix + 1, iy + 1) - h(ix, iy + 1)) * tx;
    return a + (b - a) * ty;
}

// Two-octave fbm in [0, 1] at a feature scale (m); rescaled so its spread is close to uniform's
// (two summed octaves bunch around 0.5).
double clump_field(uint64_t seed, double x, double y, double scale_m) {
    const double n = (value_noise(seed, x / scale_m, y / scale_m) * 2.0 +
                      value_noise(seed ^ 0x5bd1e995ULL, x / (scale_m * 0.37), y / (scale_m * 0.37))) / 3.0;
    return std::clamp(0.5 + (n - 0.5) * 1.8, 0.0, 1.0);
}

uint64_t key_seed(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;  // FNV-1a
    for (unsigned char ch : s) h = (h ^ ch) * 1099511628211ULL;
    return h;
}

double num(const toml::node* n, double fallback) {
    if (!n) return fallback;
    if (auto v = n->value<double>()) return *v;
    if (auto v = n->value<int64_t>()) return static_cast<double>(*v);
    return fallback;
}

}  // namespace

CoverRulesResult load_cover_rules(const std::string& look_path) {
    CoverRulesResult res;
    try {
        toml::table t = toml::parse_file(look_path);
        const toml::table* cov = t["cover"].as_table();
        if (!cov) return res;
        res.present = true;
        CoverRules& R = res.rules;
        R.candidates_per_m2 = static_cast<int>(std::clamp(num(cov->get("candidates_per_m2"), 2.0), 1.0, 16.0));
        const toml::table* items = (*cov)["items"].as_table();
        if (!items) {
            res.error = look_path + ": [cover] needs [cover.items.<key>] tables";
            return res;
        }
        for (const auto& [k, node] : *items) {
            const toml::table* it = node.as_table();
            if (!it) continue;
            CoverItem c;
            c.key = std::string(k.str());
            c.mesh = it->get("mesh") ? it->get("mesh")->value_or(c.key) : c.key;
            for (int s = 0; s < 4; ++s) c.per_100m2[s] = static_cast<float>(num(it->get(kCoverSets[s]), 0.0));
            c.soil_per_100m2 = static_cast<float>(num(it->get("soil"), 0.0));
            c.height_min_m = num(it->get("height_min_m"), c.height_min_m);
            c.height_max_m = num(it->get("height_max_m"), c.height_max_m);
            c.variants = static_cast<int>(std::max(1.0, num(it->get("variants"), c.variants)));
            c.consume = static_cast<float>(num(it->get("consume"), 0.0));
            c.smoulder = static_cast<float>(num(it->get("smoulder"), 0.0));
            c.clump = static_cast<float>(std::max(0.0, num(it->get("clump"), 0.0)));
            c.clump_m = std::max(0.5, num(it->get("clump_m"), c.clump_m));
            c.clump_group = it->get("clump_group") ? it->get("clump_group")->value_or(std::string()) : std::string();
            c.clump_anti = it->get("clump_anti") ? it->get("clump_anti")->value_or(false) : false;
            c.clump_size = static_cast<float>(std::max(0.0, num(it->get("clump_size"), 0.0)));
            c.rest_m = num(it->get("rest_m"), -1.0);
            const std::string pose = it->get("pose") ? it->get("pose")->value_or(std::string("upright")) : "upright";
            if (pose == "upright") c.pose = CoverPose::Upright;
            else if (pose == "conform") c.pose = CoverPose::Conform;
            else if (pose == "leaner") c.pose = CoverPose::Leaner;
            else {
                res.error = look_path + ": [cover.items." + c.key + "] pose must be upright | conform | leaner, not '" + pose + "'";
                return res;
            }
            R.items.push_back(std::move(c));
        }
        // Keys in file order are not guaranteed by the TOML table; sort for determinism.
        std::sort(R.items.begin(), R.items.end(), [](const CoverItem& a, const CoverItem& b) { return a.key < b.key; });
    } catch (const std::exception& e) {
        res.error = look_path + ": " + e.what();
    }
    return res;
}

void scatter_cover(const CoverRules& R, double x0, double y0, double x1, double y1, const GroundMixFn& mix_at,
                   std::vector<CoverInstance>& out) {
    if (R.items.empty()) return;
    const int K = std::max(1, R.candidates_per_m2);
    const int64_t ix0 = static_cast<int64_t>(std::floor(x0)), ix1 = static_cast<int64_t>(std::ceil(x1));
    const int64_t iy0 = static_cast<int64_t>(std::floor(y0)), iy1 = static_cast<int64_t>(std::ceil(y1));
    std::vector<double> dens(R.items.size()), mult(R.items.size(), 1.0);
    std::array<float, 4> w{};
    // Clump fields: one per (group, scale), shared by the items that name the same group.
    struct Field { uint64_t seed; double scale; };
    std::vector<Field> fields;
    std::vector<int> item_field(R.items.size(), -1);
    for (size_t i = 0; i < R.items.size(); ++i) {
        const CoverItem& c = R.items[i];
        if (c.clump <= 0.0f) continue;
        const uint64_t s = key_seed(c.clump_group.empty() ? c.key : "group:" + c.clump_group);
        int f = 0;
        for (; f < static_cast<int>(fields.size()); ++f)
            if (fields[f].seed == s && fields[f].scale == c.clump_m) break;
        if (f == static_cast<int>(fields.size())) fields.push_back({s, c.clump_m});
        item_field[i] = f;
    }
    std::vector<double> fval(fields.size());
    for (int64_t iy = iy0; iy < iy1; ++iy)
        for (int64_t ix = ix0; ix < ix1; ++ix)
            for (int a = 0; a < K; ++a) {
                const uint64_t cell = scatter::hash64({R.seed, static_cast<uint64_t>(ix), static_cast<uint64_t>(iy),
                                                       static_cast<uint64_t>(a)});
                const double x = static_cast<double>(ix) + scatter::u01(scatter::hash64({cell, kCoverJx}));
                const double y = static_cast<double>(iy) + scatter::u01(scatter::hash64({cell, kCoverJy}));
                if (x < x0 || x >= x1 || y < y0 || y >= y1) continue;
                if (!mix_at(x, y, w)) continue;
                const float soil = std::max(0.0f, 1.0f - (w[0] + w[1] + w[2] + w[3]));
                for (size_t f = 0; f < fields.size(); ++f) fval[f] = clump_field(fields[f].seed, x, y, fields[f].scale);
                double total = 0;
                for (size_t i = 0; i < R.items.size(); ++i) {
                    const CoverItem& c = R.items[i];
                    double d = soil * c.soil_per_100m2;
                    for (int s = 0; s < 4; ++s) d += w[s] * c.per_100m2[s];
                    if (item_field[i] >= 0) {
                        const double n = c.clump_anti ? 1.0 - fval[item_field[i]] : fval[item_field[i]];
                        mult[i] = std::max(0.0, 1.0 + 4.0 * c.clump * (n - 0.5));
                        d *= mult[i];
                    }
                    dens[i] = d;
                    total += d;
                }
                // accept with probability (total per m2) / K
                const double p = total / 100.0 / K;
                if (scatter::u01(scatter::hash64({cell, kCoverCand})) >= p) continue;
                double pick = scatter::u01(scatter::hash64({cell, kCoverItem})) * total;
                size_t item = 0;
                for (; item + 1 < R.items.size(); ++item) {
                    if (pick < dens[item]) break;
                    pick -= dens[item];
                }
                const CoverItem& c = R.items[item];
                CoverInstance in;
                in.x = x;
                in.y = y;
                in.item = static_cast<int>(item);
                in.variant = static_cast<int>(scatter::hash64({cell, kCoverVar}) % static_cast<uint64_t>(c.variants));
                in.height_m = c.height_min_m + scatter::u01(scatter::hash64({cell, kCoverH})) * (c.height_max_m - c.height_min_m);
                if (c.clump_size > 0.0f && item_field[item] >= 0)  // bigger toward clump centres
                    in.height_m *= std::clamp(1.0 + c.clump_size * (mult[item] - 1.0) * 0.5, 0.5, 1.8);
                in.yaw_rad = scatter::u01(scatter::hash64({cell, kCoverYaw})) * 2.0 * 3.141592653589793;
                out.push_back(in);
            }
}

}  // namespace emberworld

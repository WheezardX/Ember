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
    std::vector<double> dens(R.items.size());
    std::array<float, 4> w{};
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
                double total = 0;
                for (size_t i = 0; i < R.items.size(); ++i) {
                    const CoverItem& c = R.items[i];
                    double d = soil * c.soil_per_100m2;
                    for (int s = 0; s < 4; ++s) d += w[s] * c.per_100m2[s];
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
                in.yaw_rad = scatter::u01(scatter::hash64({cell, kCoverYaw})) * 2.0 * 3.141592653589793;
                out.push_back(in);
            }
}

}  // namespace emberworld

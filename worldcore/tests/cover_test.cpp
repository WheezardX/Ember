#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

#include "doctest.h"
#include "emberworld/cover.h"

using namespace emberworld;

namespace {

std::string look_path() { return std::string(EMBERWORLD_REPO_DIR) + "/viz/looks/terrain_default.toml"; }

CoverRules two_items() {
    CoverRules R;
    CoverItem grass{"grass", "grass", {0, 200, 0, 0}, 0, 0.3, 0.6, 4};
    CoverItem rock{"rock", "rock", {0, 0, 20, 0}, 2, 0.3, 1.0, 3};
    R.items = {grass, rock};
    return R;
}

bool all_grass(double, double, std::array<float, 4>& w) {
    w = {0, 1, 0, 0};
    return true;
}

}  // namespace

TEST_CASE("cover: density follows the ground mix") {
    const CoverRules R = two_items();
    std::vector<CoverInstance> out;
    scatter_cover(R, 0, 0, 50, 50, all_grass, out);   // 2500 m2 of grass at 200 / 100 m2
    CHECK(out.size() > 4500);
    CHECK(out.size() < 5500);
    CHECK(std::all_of(out.begin(), out.end(), [](const CoverInstance& c) { return c.item == 0; }));
    out.clear();
    scatter_cover(R, 0, 0, 50, 50, [](double, double, std::array<float, 4>& w) { w = {0, 0, 0, 0}; return true; }, out);
    CHECK(out.size() > 25);   // soil: rocks only, 2 / 100 m2 -> ~50
    CHECK(out.size() < 80);
    out.clear();
    scatter_cover(R, 0, 0, 50, 50, [](double, double, std::array<float, 4>&) { return false; }, out);
    CHECK(out.empty());       // no ground, nothing
}

TEST_CASE("cover: any partition of the world gives the same instances") {
    const CoverRules R = two_items();
    auto key = [](const CoverInstance& c) { return std::make_tuple(c.x, c.y, c.item, c.variant, c.height_m); };
    std::vector<CoverInstance> whole, parts;
    scatter_cover(R, -13.5, 7.25, 40.0, 61.0, all_grass, whole);
    for (double x : {-13.5, 3.0, 22.7})
        for (double y : {7.25, 30.0}) {
            const double xe = x == 22.7 ? 40.0 : (x == -13.5 ? 3.0 : 22.7);
            const double ye = y == 30.0 ? 61.0 : 30.0;
            scatter_cover(R, x, y, xe, ye, all_grass, parts);
        }
    std::vector<std::tuple<double, double, int, int, double>> a, b;
    for (const auto& c : whole) a.push_back(key(c));
    for (const auto& c : parts) b.push_back(key(c));
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    CHECK(a == b);
}

TEST_CASE("cover: the default look's [cover] loads") {
    CoverRulesResult r = load_cover_rules(look_path());
    REQUIRE_MESSAGE(r.ok(), r.error);
    REQUIRE(r.present);
    CHECK(r.rules.items.size() >= 5);
    for (const CoverItem& c : r.rules.items) CHECK(c.height_max_m >= c.height_min_m);
}

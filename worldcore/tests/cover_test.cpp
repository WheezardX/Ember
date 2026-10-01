#include <algorithm>
#include <cstdio>
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

TEST_CASE("cover: pose per item (logs conform, leaners, bad values rejected)") {
    CoverRulesResult r = load_cover_rules(look_path());
    REQUIRE_MESSAGE(r.ok(), r.error);
    auto find = [&](const std::string& k) {
        return std::find_if(r.rules.items.begin(), r.rules.items.end(), [&](const CoverItem& c) { return c.key == k; });
    };
    REQUIRE(find("log") != r.rules.items.end());
    CHECK(find("log")->pose == CoverPose::Conform);
    REQUIRE(find("leaner") != r.rules.items.end());
    CHECK(find("leaner")->pose == CoverPose::Leaner);
    CHECK(find("rock")->pose == CoverPose::Upright);
    const std::string bad = std::string(EMBERWORLD_REPO_DIR) + "/worldcore/build/cover_bad_pose.toml";
    {
        std::FILE* f = std::fopen(bad.c_str(), "w");
        REQUIRE(f);
        std::fputs("[cover]\n[cover.items.log]\nlitter = 1.0\npose = \"sideways\"\n", f);
        std::fclose(f);
    }
    CoverRulesResult b = load_cover_rules(bad);
    CHECK_FALSE(b.ok());
    CHECK(b.error.find("sideways") != std::string::npos);
    std::remove(bad.c_str());
}

TEST_CASE("cover: clumping makes patches and gaps at about the same average") {
    CoverRules R = two_items();
    R.candidates_per_m2 = 8;   // clumps need head-room: up to 3x the mean density per square
    std::vector<CoverInstance> flat, clumped;
    scatter_cover(R, 0, 0, 120, 120, all_grass, flat);
    R.items[0].clump = 1.0f;
    R.items[0].clump_m = 8.0;
    scatter_cover(R, 0, 0, 120, 120, all_grass, clumped);
    const double ratio = static_cast<double>(clumped.size()) / static_cast<double>(flat.size());
    CHECK(ratio > 0.8);
    CHECK(ratio < 1.3);
    // per 4 m square counts: uniform scatter keeps every square near the mean; clumping empties
    // some and doubles others
    auto squares = [](const std::vector<CoverInstance>& v) {
        std::vector<int> n(30 * 30, 0);
        for (const CoverInstance& c : v) n[static_cast<int>(c.y / 4) * 30 + static_cast<int>(c.x / 4)]++;
        return n;
    };
    const std::vector<int> a = squares(flat), b = squares(clumped);
    const int empty_flat = static_cast<int>(std::count(a.begin(), a.end(), 0));
    const int empty_clumped = static_cast<int>(std::count(b.begin(), b.end(), 0));
    CHECK(empty_flat == 0);
    CHECK(empty_clumped > 60);
    CHECK(*std::max_element(b.begin(), b.end()) > 1.4 * *std::max_element(a.begin(), a.end()));
}

TEST_CASE("cover: clumped placement is still partition-independent; anti reads the field inverted") {
    CoverRules R = two_items();
    R.items[0].clump = 1.0f;
    R.items[0].clump_group = "gap";
    R.items[0].clump_size = 0.6f;
    std::vector<CoverInstance> whole, parts;
    scatter_cover(R, 0, 0, 40, 40, all_grass, whole);
    for (int y = 0; y < 40; y += 10)
        for (int x = 0; x < 40; x += 20) scatter_cover(R, x, y, x + 20, y + 10, all_grass, parts);
    auto key = [](const CoverInstance& c) { return std::make_tuple(c.x, c.y, c.item, c.variant, c.height_m); };
    std::vector<std::tuple<double, double, int, int, double>> ka, kb;
    for (const CoverInstance& c : whole) ka.push_back(key(c));
    for (const CoverInstance& c : parts) kb.push_back(key(c));
    std::sort(ka.begin(), ka.end());
    std::sort(kb.begin(), kb.end());
    CHECK(ka == kb);
    // an anti item in the same group fills the gaps: few squares hold both
    CoverRules A = R;
    A.items[0].clump_anti = true;
    std::vector<CoverInstance> anti;
    scatter_cover(A, 0, 0, 40, 40, all_grass, anti);
    std::vector<int> n1(100, 0), n2(100, 0);
    for (const CoverInstance& c : whole) n1[static_cast<int>(c.y / 4) * 10 + static_cast<int>(c.x / 4)]++;
    for (const CoverInstance& c : anti) n2[static_cast<int>(c.y / 4) * 10 + static_cast<int>(c.x / 4)]++;
    int both_dense = 0;
    for (int i = 0; i < 100; ++i) both_dense += n1[i] > 40 && n2[i] > 40;
    CHECK(both_dense < 5);
}

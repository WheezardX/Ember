// C4 conformance: the C++ scatter must reproduce Terrain's reference implementation exactly.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include "doctest.h"
#include "emberworld/region.h"
#include "emberworld/scatter.h"
#include "emberworld/veg.h"
#include "json.hpp"

using namespace emberworld;
using namespace emberworld::scatter;
namespace fs = std::filesystem;

namespace {

std::string data(const char* name) { return std::string(EMBERWORLD_REPO_DIR) + "/worldcore/tests/data/" + name; }

std::string teanaway_dir() {
    if (const char* s = std::getenv("EMBER_TERRAIN_STORE")) return std::string(s) + "/teanaway_dev";
    return std::string(EMBERWORLD_REPO_DIR) + "/../Terrain/store/teanaway_dev";
}

// Python round(v, nd) == g, allowing only representation noise.
bool rounds_to(double v, double g, int nd) {
    const double q = std::pow(10.0, nd);
    return std::fabs(std::nearbyint(v * q) / q - g) < 0.5 / q;
}

}  // namespace

TEST_CASE("scatter: hash conformance constants (terrain tests/test_veg.py)") {
    CHECK(splitmix64(0) == 16294208416658607535ULL);
    CHECK(splitmix64(1) == 10451216379200822465ULL);
    CHECK(hash64({1, 2, 3}) == 15020427595393229491ULL);
    CHECK(hash64({3, 2, 1}) == 15432287184251514724ULL);
    CHECK(u01(hash64({1, 2, 3})) == 0.814259011529328);
    CHECK(hash64({1337, 1, 14, 0, 0}) == 14401203105444160908ULL);
    const int64_t w[] = {5, 4, 2};
    CHECK(weighted_pick(5, w) == 1);
    CHECK(weighted_pick(10, w) == 2);
    CHECK(weighted_pick(0, w) == 0);
}

TEST_CASE("scatter: palette loads and maps EVT like the Python") {
    auto pr = load_palette(data("pnw_conifer.toml"));
    REQUIRE_MESSAGE(pr.ok(), pr.error);
    const Palette& p = pr.palette;
    CHECK(p.group_for_evt(7050).name == "conifer_forest");
    CHECK(p.group_for_evt(5000).name == "shrub_grass");
    CHECK(p.group_for_evt(9999).name == p.default_group);
    CHECK(p.species_index().size() == 5);
    CHECK(p.groups[1].offset == 3);
    REQUIRE(p.structure.size() == 4);                     // scatter v2 stand structure
    CHECK(p.class_weights == std::vector<int64_t>{2, 3, 2, 3});
}

TEST_CASE("scatter: exact EVT codes win over ranges (scatter v2 palettes)") {
    // Same resolution rule as terrain/veg/palette.py::group_for_evt.
    Palette p;
    p.default_group = "d";
    Group range{"range", {}, true, 7000, 7999, {Species{"a"}}};
    Group exact{"exact", {7139, 7292}, false, 0, 0, {Species{"b"}}};
    Group dflt{"d", {}, false, 0, 0, {Species{"c"}}};
    p.groups = {range, exact, dflt};
    p.structure = {CrownClass{"all", 1, 1.0, 1.0}};
    p.finalize();
    CHECK(p.group_for_evt(7139).name == "exact");   // exact code listed after the range
    CHECK(p.group_for_evt(7042).name == "range");
    CHECK(p.group_for_evt(5000).name == "d");
}

TEST_CASE("scatter: matches Terrain's golden vectors (synthetic 8x8)") {
    auto pr = load_palette(data("pnw_conifer.toml"));
    REQUIRE(pr.ok());
    std::ifstream gf(data("scatter_pnw.json"));
    REQUIRE(gf);
    const auto golden = nlohmann::json::parse(gf);

    const int n = 8;
    std::vector<float> cc(n * n), height(n * n), dem(n * n, 800.0f);
    std::vector<int32_t> evt(n * n);
    for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c) {
            const int k = r * n + c;
            cc[k] = static_cast<float>(static_cast<double>(r * n + c) * 70.0 / (n * n));  // numpy f8 -> f4
            evt[k] = r < n / 2 ? 7050 : 5000;
            height[k] = evt[k] >= 7000 ? 20.0f : 1.0f;
        }
    Params prm;
    prm.tile_seed = golden.at("tile_seed").get<uint64_t>();
    prm.cell_size = 10.0;
    prm.x0 = 500000.0;
    prm.y_top = 5200000.0;
    prm.candidates_per_cell = 4;
    std::vector<Instance> out;
    scatter_window(pr.palette, prm, 0, 0, n, n, cc, height, evt, dem, out);
    REQUIRE(out.size() == golden.at("count").get<size_t>());
    const auto& first = golden.at("first20");
    for (size_t i = 0; i < first.size(); ++i) {
        const auto& g = first[i];
        const Instance& in = out[i];
        INFO("row " << i);
        CHECK(rounds_to(in.x, g[0].get<double>(), 3));
        CHECK(rounds_to(in.y, g[1].get<double>(), 3));
        CHECK(rounds_to(in.z, g[2].get<double>(), 3));
        CHECK(in.species == g[3].get<int>());
        CHECK(rounds_to(in.height_m, g[4].get<double>(), 3));
        CHECK(rounds_to(in.yaw_rad, g[5].get<double>(), 4));
        CHECK(rounds_to(in.scale, g[6].get<double>(), 4));
        CHECK(rounds_to(in.radius_m, g[7].get<double>(), 3));
    }
    std::vector<std::string> keys;
    for (const Species* s : pr.palette.species_index()) keys.push_back(s->key);
    CHECK(keys == golden.at("species_index").get<std::vector<std::string>>());
}

TEST_CASE("scatter: windows are independent (per-tile == whole grid)") {
    auto pr = load_palette(data("pnw_conifer.toml"));
    REQUIRE(pr.ok());
    const int n = 16;
    std::vector<float> cc(n * n), height(n * n, 25.0f), dem(n * n, 1000.0f);
    std::vector<int32_t> evt(n * n, 7050);
    for (int k = 0; k < n * n; ++k) cc[k] = static_cast<float>((k * 37) % 90);
    Params prm;
    prm.tile_seed = hash64({1337, 1, 14, 0, 0});
    std::vector<Instance> whole;
    scatter_window(pr.palette, prm, 0, 0, n, n, cc, height, evt, dem, whole);
    std::vector<Instance> parts;
    for (int r0 = 0; r0 < n; r0 += 8)
        for (int c0 = 0; c0 < n; c0 += 8) {
            std::vector<float> wcc(64), wh(64, 25.0f), wd(64, 1000.0f);
            std::vector<int32_t> we(64, 7050);
            for (int r = 0; r < 8; ++r)
                for (int c = 0; c < 8; ++c) wcc[r * 8 + c] = cc[(r0 + r) * n + (c0 + c)];
            scatter_window(pr.palette, prm, r0, c0, 8, 8, wcc, wh, we, wd, parts);
        }
    auto key = [](const Instance& i) { return std::tie(i.x, i.y); };
    auto less = [&](const Instance& a, const Instance& b) { return key(a) < key(b); };
    std::sort(whole.begin(), whole.end(), less);
    std::sort(parts.begin(), parts.end(), less);
    REQUIRE(whole.size() == parts.size());
    for (size_t i = 0; i < whole.size(); ++i) {
        CHECK(whole[i].x == parts[i].x);
        CHECK(whole[i].height_m == parts[i].height_m);
        CHECK(whole[i].species == parts[i].species);
    }
}

namespace {

struct NpyRow {
    double x, y;
    float z;
    int32_t species;
    float height, yaw, scale, radius;
};

// Terrain's veg/instances.npy: structured, little-endian, 40-byte records.
bool read_instances_npy(const std::string& path, std::vector<NpyRow>& rows, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    char magic[6];
    f.read(magic, 6);
    if (std::memcmp(magic, "\x93NUMPY", 6) != 0) {
        err = "not npy";
        return false;
    }
    uint8_t ver[2];
    f.read(reinterpret_cast<char*>(ver), 2);
    uint32_t hlen = 0;
    if (ver[0] == 1) {
        uint16_t h16;
        f.read(reinterpret_cast<char*>(&h16), 2);
        hlen = h16;
    } else {
        f.read(reinterpret_cast<char*>(&hlen), 4);
    }
    std::string header(hlen, '\0');
    f.read(header.data(), hlen);
    const char* want[] = {"'x', '<f8'", "'y', '<f8'", "'z', '<f4'", "'species', '<i4'",
                          "'height', '<f4'", "'yaw', '<f4'", "'scale', '<f4'", "'radius', '<f4'"};
    for (const char* w : want)
        if (header.find(w) == std::string::npos) {
            err = std::string("npy header lacks ") + w + ": " + header;
            return false;
        }
    const auto sp = header.find("'shape': (");
    const size_t count = std::stoull(header.substr(sp + 10));
    rows.resize(count);
    static_assert(sizeof(NpyRow) == 40);
    f.read(reinterpret_cast<char*>(rows.data()), static_cast<std::streamsize>(count * sizeof(NpyRow)));
    return static_cast<bool>(f);
}

}  // namespace

std::string store_region(const char* name) {
    if (const char* s = std::getenv("EMBER_TERRAIN_STORE")) return std::string(s) + "/" + name;
    return std::string(EMBERWORLD_REPO_DIR) + "/../Terrain/store/" + name;
}

// Per-tile C++ scatter over every finest tile == Terrain's whole-AOI veg/instances.npy, exactly
// (positions bit-equal, float32 attributes bit-equal), compared as sets.
void check_region_oracle(const char* name) {
    const std::string dir = store_region(name);
    if (!fs::exists(dir + "/veg/instances.npy")) {
        MESSAGE(std::string(name) << " veg not found - skipped");
        return;
    }
    auto rr = load_region(dir);
    REQUIRE_MESSAGE(rr, rr.error);
    const Region& R = *rr.region;
    auto si = load_scatter_input(R);
    REQUIRE_MESSAGE(si.ok(), si.error);
    auto pr = load_palette(resolve_palette(R, si.input.palette_ref));
    REQUIRE_MESSAGE(pr.ok(), pr.error);

    std::vector<Instance> got;
    for (const TileEntry* t : R.tiles_at(R.finest_lod())) {
        auto ts = scatter_tile(R, *t, pr.palette, si.input);
        REQUIRE_MESSAGE(ts.ok(), ts.error);
        got.insert(got.end(), ts.instances.begin(), ts.instances.end());
    }
    std::vector<NpyRow> want;
    std::string err;
    REQUIRE_MESSAGE(read_instances_npy(R.path("veg/instances.npy"), want, err), err);
    MESSAGE(std::string(name) << ": " << got.size() << " C++ instances vs " << want.size() << " Terrain");
    REQUIRE(got.size() == want.size());

    std::sort(got.begin(), got.end(), [](const Instance& a, const Instance& b) {
        return std::tie(a.x, a.y) < std::tie(b.x, b.y);
    });
    std::sort(want.begin(), want.end(), [](const NpyRow& a, const NpyRow& b) {
        return std::tie(a.x, a.y) < std::tie(b.x, b.y);
    });
    size_t mismatches = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        const Instance& g = got[i];
        const NpyRow& w = want[i];
        const bool same = g.x == w.x && g.y == w.y && static_cast<float>(g.z) == w.z &&
                          g.species == w.species && static_cast<float>(g.height_m) == w.height &&
                          static_cast<float>(g.yaw_rad) == w.yaw && static_cast<float>(g.scale) == w.scale &&
                          static_cast<float>(g.radius_m) == w.radius;
        if (!same && mismatches++ < 5)
            MESSAGE("row " << i << ": got (" << g.x << ", " << g.y << ", sp " << g.species << ", h "
                           << g.height_m << ") want (" << w.x << ", " << w.y << ", sp " << w.species
                           << ", h " << w.height << ")");
    }
    CHECK(mismatches == 0);
}

TEST_CASE("scatter: teanaway_dev per-tile scatter == Terrain's instances.npy, exactly") {
    check_region_oracle("teanaway_dev");  // 74,595 instances, candidates_per_cell 12
}

TEST_CASE("scatter: three_queens_2026 per-tile scatter == Terrain's instances.npy, exactly") {
    check_region_oracle("three_queens_2026");  // ~5.86 M instances, candidates_per_cell 4
}

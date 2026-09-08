#include <filesystem>
#include <fstream>
#include <string>

#include "../third_party/json.hpp"
#include "doctest.h"
#include "weather.h"
#include "worldpack.h"

using namespace embersim;
namespace fs = std::filesystem;

static fs::path tiny() { return fs::path(EMBERSIM_TEST_DATA_DIR) / "tiny.ewp"; }

static fs::path copy_tiny(const std::string& tag) {
    fs::path dst = fs::temp_directory_path() / ("embersim_loader_" + tag);
    fs::remove_all(dst);
    fs::copy(tiny(), dst, fs::copy_options::recursive);
    return dst;
}

TEST_CASE("tiny.ewp loads with the documented layout") {
    World w = load_world(tiny());
    CHECK(w.grid.nx == 8);
    CHECK(w.grid.ny == 8);
    CHECK(w.grid.cell_mm == 30000);
    CHECK(w.grid.crs == "LOCAL");
    CHECK(w.t0_unix == 0);
    CHECK(w.elevation_cm[0] == 100000);
    CHECK(fuel_class_of(w.fbfm40[0]) == FuelClass::GR);
    CHECK(fuel_class_of(w.fbfm40[7]) == FuelClass::NB);  // east column is barren (99)
    CHECK(burnable(w.fbfm40[3 * 8 + 3]));
    REQUIRE(w.has_arrival());
    CHECK(w.arrival_s[3 * 8 + 3] == 0);
    CHECK(w.arrival_s[4 * 8 + 4] == 1200);
    CHECK(w.arrival_s[0] == -1);
    REQUIRE(w.has_confidence());
    CHECK(w.confidence[3 * 8 + 3] == 1);
    CHECK_FALSE(w.has_greenness());
    CHECK_FALSE(w.has_structures());
    CHECK_FALSE(w.has_hillshade());
    REQUIRE(w.weather.has_value());
    CHECK(w.weather->nx == 2);
    CHECK(w.weather->ny == 1);
    CHECK(w.weather->num_steps == 3);
    CHECK(w.weather->step_s == 3600);
    CHECK(w.weather->held_from[0] == -1);
    CHECK(w.weather->held_from[1] == 0);
    CHECK(w.weather->precip_present);
    CHECK(w.weather->step_var(0, WX_U)[1] == 600);

    // pack_sha256 recomputed at load matches the manifest's pack_hash.
    std::ifstream f(tiny() / "world.json");
    nlohmann::json m = nlohmann::json::parse(f);
    CHECK(w.pack_sha256 == m["pack_hash"].get<std::string>());
    CHECK(w.pack_sha256.size() == 64);
}

TEST_CASE("loader errors name the layer") {
    fs::path d = copy_tiny("missing");
    fs::remove(d / "fbfm40.bin");
    CHECK_THROWS_WITH_AS(load_world(d), doctest::Contains("fbfm40"), std::runtime_error);

    fs::path d2 = copy_tiny("short");
    {
        std::ofstream t(d2 / "cc_pct.bin", std::ios::binary | std::ios::trunc);
        t << "abc";
    }
    CHECK_THROWS_WITH_AS(load_world(d2), doctest::Contains("cc_pct"), std::runtime_error);
    CHECK_THROWS_WITH_AS(load_world(d2), doctest::Contains("bytes"), std::runtime_error);

    CHECK_THROWS_WITH_AS(load_world(fs::temp_directory_path() / "embersim_no_such_pack"), doctest::Contains("world.json"),
                         std::runtime_error);
}

TEST_CASE("weather sampler: nearest cell, Q16 lerp, held steps, tail hold") {
    World w = load_world(tiny());
    WeatherSampler ws = WeatherSampler::from_pack(*w.weather, w.grid, w.t0_unix);
    CHECK(ws.wcells() == 2);
    CHECK(ws.wcell_of(0, 0) == 0);  // x < 4 -> west cell (dx 120 m / 30 m cells)
    CHECK(ws.wcell_of(5, 7) == 1);

    ws.set_time(0);
    CHECK(ws.step_started());
    CHECK(ws.current_step() == 0);
    CHECK(ws.held_steps() == 0);
    CHECK(ws.at_cell(0, 0).u_cms == 300);
    CHECK(ws.at_cell(6, 0).u_cms == 600);  // east cell windier
    CHECK(ws.at_cell(0, 0).t2_dk == 2981);
    CHECK(ws.at_cell(0, 0).precip_cmm == 0);

    ws.set_time(1800);
    CHECK_FALSE(ws.step_started());
    CHECK(ws.at_cell(0, 0).u_cms == 300);  // step 1 == step 0 (held), so no change midway

    ws.set_time(3600);  // step 1 starts: it was held from step 0
    CHECK(ws.step_started());
    CHECK(ws.current_step() == 1);
    CHECK(ws.held_steps() == 1);

    ws.set_time(5400);  // midway between step 1 (u=300, v=0, t2=2981, rh=250) and step 2 (0, -400, 2931, 400)
    CHECK_FALSE(ws.step_started());
    CHECK(ws.at_cell(0, 0).u_cms == 150);
    CHECK(ws.at_cell(0, 0).v_cms == -200);
    CHECK(ws.at_cell(0, 0).t2_dk == 2956);
    CHECK(ws.at_cell(0, 0).rh2_dpct == 325);
    CHECK(ws.at_cell(0, 0).precip_cmm == 0);  // step 1's accumulation
    CHECK(ws.held_steps() == 1);

    ws.set_time(9000);  // past the last step start (7200): hold the tail
    CHECK(ws.step_started());
    CHECK(ws.current_step() == 2);
    CHECK(ws.at_cell(0, 0).u_cms == 0);
    CHECK(ws.at_cell(0, 0).precip_cmm == 150);
    CHECK(ws.held_tail_s() == 1800);
    CHECK(ws.held_steps() == 1);
    WeatherSample m = ws.mean();
    CHECK(m.v_cms == -400);

    // Starting before the timeline is refused (ADR 0009 C).
    WeatherSampler early = WeatherSampler::from_pack(*w.weather, w.grid, w.t0_unix - 7200);
    CHECK_THROWS(early.set_time(0));
}

TEST_CASE("constant weather sampler") {
    WeatherSample s{123, -45, 3000, 200, 7};
    WeatherSampler ws = WeatherSampler::constant(s);
    ws.set_time(0);
    CHECK(ws.step_started());
    ws.set_time(60);
    CHECK_FALSE(ws.step_started());
    CHECK(ws.at_cell(1000, 1000).u_cms == 123);
    CHECK(ws.mean().v_cms == -45);
    CHECK(ws.is_constant());
    CHECK(ws.held_steps() == 0);
}

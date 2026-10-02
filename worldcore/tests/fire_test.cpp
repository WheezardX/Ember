// Fire state player core vs Epic 4's own reader (ember/sim/stream.py iter_frames) as the oracle.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "doctest.h"
#include "emberworld/firestate.h"
#include "json.hpp"

using namespace emberworld::fire;

namespace {
std::string data(const char* name) { return std::string(EMBERWORLD_REPO_DIR) + "/worldcore/tests/data/" + name; }
std::string repo_path(const std::string& rel) { return std::string(EMBERWORLD_REPO_DIR) + "/" + rel; }

uint64_t fnv1a(const void* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ULL;
    return h;
}
}  // namespace

TEST_CASE("fire: small synthetic stream - every tick, between ticks, final arrival") {
    Stream s;
    REQUIRE(s.open(data("fire_small.ess")) == "");
    CHECK(s.header().nx == 6);
    CHECK(s.header().ny == 5);
    CHECK(s.header().model_id == "test-model");
    CHECK(s.ticks() == 7);
    std::ifstream f(data("fire_small.expected.json"));
    const auto exp = nlohmann::json::parse(f);
    for (const auto& [t, fr] : exp.at("frames").items()) {
        const int32_t ts = std::stoi(t);
        for (int32_t probe : {ts, ts + 1799}) {  // exact tick time, and half a tick later
            const State st = s.at(probe);
            INFO("t=" << probe);
            CHECK(st.phase == fr.at("phase").get<std::vector<uint8_t>>());
            CHECK(st.intensity == fr.at("intensity").get<std::vector<uint8_t>>());
            CHECK(st.metrics.burned == fr.at("burned").get<uint32_t>());
        }
    }
    CHECK(s.final_arrival() == exp.at("final_arrival").get<std::vector<int32_t>>());
    // seeking backwards gives the same answer as seeking forwards
    const State late = s.at(7 * 3600), early = s.at(2 * 3600), again = s.at(7 * 3600);
    CHECK(late.phase == again.phase);
    CHECK(early.phase != late.phase);
}

TEST_CASE("fire: Jolly Mountain CP2 playback stream matches Epic 4's reader") {
    std::ifstream f(data("fire_jolly.expected.json"));
    const auto exp = nlohmann::json::parse(f);
    const std::string path = repo_path(exp.at("stream").get<std::string>());
    if (!std::filesystem::exists(path)) {
        MESSAGE("CP2 stream not present - skipped (" << path << ")");
        return;
    }
    Stream s;
    REQUIRE(s.open(path) == "");
    CHECK(s.header().cells() == exp.at("cells").get<size_t>());
    CHECK(s.end_s() == exp.at("end_s").get<int32_t>());
    for (const auto& [t, want] : exp.at("at").items()) {
        const State st = s.at(std::stoi(t));
        INFO("t=" << t);
        CHECK(st.tick == want.at("tick").get<uint32_t>());
        CHECK(std::to_string(fnv1a(st.phase.data(), st.phase.size())) == want.at("phase_fnv1a64").get<std::string>());
        for (int k = 0; k < 4; ++k)
            CHECK(std::count(st.phase.begin(), st.phase.end(), k) == want.at("counts")[k].get<int64_t>());
    }
    const auto& a = s.final_arrival();
    CHECK(std::to_string(fnv1a(a.data(), a.size() * 4)) == exp.at("final_arrival_fnv1a64").get<std::string>());
}

TEST_CASE("fire: Jolly CA run spot fires match Epic 4's reader") {
    std::ifstream f(data("fire_jolly_ca_spots.expected.json"));
    const auto exp = nlohmann::json::parse(f);
    const std::string path = repo_path(exp.at("stream").get<std::string>());
    if (!std::filesystem::exists(path)) {
        MESSAGE("CP5 CA stream not present - skipped (" << path << ")");
        return;
    }
    Stream s;
    REQUIRE(s.open(path) == "");
    const std::vector<Spot>& sp = s.spots();
    REQUIRE(sp.size() == exp.at("count").get<size_t>());
    int64_t landed = 0, ignited = 0, src = 0, dst = 0, launch = 0, land = 0;
    for (const Spot& x : sp) {
        landed += x.landed;
        ignited += x.ignited;
        src += x.src;
        dst += x.dst;
        launch += x.launch_s;
        land += x.land_s;
    }
    CHECK(landed == exp.at("landed").get<int64_t>());
    CHECK(ignited == exp.at("ignited").get<int64_t>());
    CHECK(src == exp.at("sum_src").get<int64_t>());
    CHECK(dst == exp.at("sum_dst").get<int64_t>());
    CHECK(launch == exp.at("sum_launch_s").get<int64_t>());
    CHECK(land == exp.at("sum_land_s").get<int64_t>());
    for (const auto& [which, x] : {std::pair{"first", sp.front()}, std::pair{"last", sp.back()}}) {
        const auto& e = exp.at(which);
        INFO(which);
        CHECK(x.src == e.at("src").get<uint32_t>());
        CHECK(x.dst == e.at("dst").get<uint32_t>());
        CHECK(x.launch_s == e.at("launch_s").get<int32_t>());
        CHECK(x.land_s == e.at("land_s").get<int32_t>());
        CHECK(x.ignited == (e.at("ignited").get<int>() != 0));
    }
}

TEST_CASE("fire: channels sidecar matches the Python writer (ADR 0010)") {
    // fixture: ember.external.channels.write_fixture (synthetic, no PyreCast data)
    const Channels ch = read_channels(data("fire_channels.channels.json"));
    REQUIRE(ch.ok());
    CHECK(ch.nx == 4);
    CHECK(ch.ny == 3);
    REQUIRE(ch.list.size() == 3);
    std::ifstream f(data("fire_channels.expected.json"));
    const auto exp = nlohmann::json::parse(f);
    for (const char* name : {"flame_length_m", "spread_rate_mh", "crown_class"}) {
        const Channel* c = ch.find(name);
        REQUIRE(c != nullptr);
        const auto& want = exp.at(name);
        for (size_t i = 0; i < want.size(); ++i) {
            INFO(name << " cell " << i);
            if (want[i].is_null()) {
                CHECK_FALSE(c->speaks(i));
            } else {
                REQUIRE(c->speaks(i));
                CHECK(c->value(i) == doctest::Approx(want[i].get<double>()).epsilon(1e-6));
            }
        }
    }
    CHECK(ch.find("no_such_channel") == nullptr);
    CHECK_FALSE(read_channels(data("missing.channels.json")).ok());
}

TEST_CASE("fire: replay points at its stream and world grid") {
    const std::string path = repo_path("runs/cp2/cp2-jolly-playback.replay.json");
    if (!std::filesystem::exists(path)) return;
    const ReplayInfo r = read_replay(path);
    REQUIRE_MESSAGE(r.ok(), r.error);
    CHECK(r.grid.cell_m == 30.0);
    CHECK(r.grid.crs == "EPSG:32610");
    CHECK(r.model_id == "arrival-playback");
    CHECK(std::filesystem::exists(r.stream_path));
}

TEST_CASE("fire: spread rate from the arrival field") {
    // A plane front moving east at 30 m per 120 s = 900 m/h; an unburned column stays 0.
    const uint32_t nx = 6, ny = 4;
    std::vector<int32_t> a(nx * ny);
    for (uint32_t y = 0; y < ny; ++y)
        for (uint32_t x = 0; x < nx; ++x) a[y * nx + x] = x == 5 ? -1 : static_cast<int32_t>(x * 120);
    const std::vector<float> r = spread_rate_mh(a, nx, ny, 30.0);
    for (uint32_t y = 0; y < ny; ++y)
        for (uint32_t x = 0; x < 5; ++x) CHECK(r[y * nx + x] == doctest::Approx(900.0).epsilon(1e-6));
    CHECK(r[5] == 0.f);
    // Diagonal front: arrival = 60 s x (x + y) -> |grad| = 60 sqrt 2 s per cell.
    for (uint32_t y = 0; y < ny; ++y)
        for (uint32_t x = 0; x < nx; ++x) a[y * nx + x] = static_cast<int32_t>(60 * (x + y));
    CHECK(spread_rate_mh(a, nx, ny, 30.0)[nx + 2] == doctest::Approx(30.0 * 3600.0 / (60.0 * std::sqrt(2.0))));
    // Same-tick ignition (a spot patch) caps; a lone cell has no neighbours -> 0.
    std::fill(a.begin(), a.end(), 500);
    CHECK(spread_rate_mh(a, nx, ny, 30.0, 2000.f)[nx + 1] == 2000.f);
    // A burned line (one row): only the along-line slope is known -> 30 m per 60 s = 1800 m/h.
    std::fill(a.begin(), a.end(), -1);
    for (uint32_t x = 0; x < nx; ++x) a[nx + x] = static_cast<int32_t>(60 * x);
    CHECK(spread_rate_mh(a, nx, ny, 30.0)[nx + 2] == doctest::Approx(1800.0).epsilon(1e-6));
    std::fill(a.begin(), a.end(), -1);
    a[nx + 1] = 10;
    CHECK(spread_rate_mh(a, nx, ny, 30.0)[nx + 1] == 0.f);
}

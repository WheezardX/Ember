#include <filesystem>
#include <fstream>
#include <string>

#include "../third_party/json.hpp"
#include "doctest.h"
#include "embersim/embersim.h"
#include "replay.h"
#include "runner.h"
#include "scenario.h"
#include "session.h"

using namespace embersim;
namespace fs = std::filesystem;

namespace {
fs::path tiny() { return fs::path(EMBERSIM_TEST_DATA_DIR) / "tiny.ewp"; }

fs::path write_scenario(const std::string& tag, const std::string& model, int duration, int dt, const std::string& extra = "") {
    fs::path dir = fs::temp_directory_path() / ("embersim_runner_" + tag);
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path f = dir / (tag + ".scenario.toml");
    std::ofstream o(f);
    o << "scenario_version = 1\n[scenario]\nname = \"" << tag << "\"\nworld = \"" << tiny().generic_string()
      << "\"\nduration_s = " << duration << "\ndt_s = " << dt << "\nseed = 5\n[model]\nid = \"" << model
      << "\"\n[weather]\nmode = \"pack\"\n[[ignitions]]\nt_s = 0\ncells = [[3, 3], [2, 2]]\n[output]\ndir = \"out\"\nkeyframe_every = 4\n"
      << extra;
    return f;
}
}  // namespace

TEST_CASE("runner: null model run writes replay + stream and re-verifies") {
    fs::path f = write_scenario("null10", "null", 600, 60);
    Scenario s = load_scenario(f);
    RunOptions o;
    o.quiet = true;
    RunResult r = run_scenario(s, o);
    CHECK(r.ticks == 10);
    CHECK(fs::exists(r.replay_path));
    CHECK(fs::exists(r.stream_path));
    CHECK(r.replay_path.filename() == "null10.replay.json");
    // checkpoints: tick 0, 4, 8, 10
    REQUIRE(r.checkpoints.size() == 4);
    CHECK(r.checkpoints[0].first == 0);
    CHECK(r.checkpoints[1].first == 4);
    CHECK(r.checkpoints[3].first == 10);
    CHECK(r.checkpoints[3].second == r.final_hash);
    CHECK(r.diagnostics["weather_mode_effective"] == "pack");

    nlohmann::json j = load_replay(r.replay_path);
    CHECK(j["model"]["id"] == "null");
    CHECK(j["world"]["pack_sha256"].get<std::string>().size() == 64);
    CHECK(j["commands"].size() == 0);
    CHECK(j["result"]["ticks"] == 10);
    CHECK(j["stream"] == "null10.ess");
    CHECK(j["scenario"]["raw_toml"].get<std::string>().find("scenario_version") != std::string::npos);

    std::string report;
    CHECK(replay_verify(r.replay_path, report, true) == 0);
    CHECK(report.find("OK") != std::string::npos);

    // Tamper the world pin -> refused, naming the field.
    j["world"]["pack_sha256"] = "deadbeef";
    {
        std::ofstream w(r.replay_path);
        w << j.dump(2);
    }
    CHECK(replay_verify(r.replay_path, report, true) == 2);
    CHECK(report.find("pack_sha256") != std::string::npos);
    CHECK(report.find("deadbeef") != std::string::npos);

    // Tamper a checkpoint hash -> mismatch at that tick.
    j = load_replay(r.replay_path);
    j["world"]["pack_sha256"] = load_world(tiny()).pack_sha256;
    j["result"]["checkpoints"][1]["hash"] = "0x0000000000000001";
    {
        std::ofstream w(r.replay_path);
        w << j.dump(2);
    }
    CHECK(replay_verify(r.replay_path, report, true) == 1);
    CHECK(report.find("tick 4") != std::string::npos);
}

TEST_CASE("runner: arrival-playback follows the tiny arrival raster") {
    fs::path f = write_scenario("pb3", "arrival-playback", 1800, 600);
    Scenario s = load_scenario(f);
    Session sess(s);
    CHECK(sess.total_ticks() == 3);
    CHECK(sess.caps().supports_rewind);
    // t=0: cell (3,3) arrival 0 is burning already
    FireStateView v = sess.state();
    CHECK(v.phase[3 * 8 + 3] == 2);
    const TickResult& t1 = sess.step();  // t=600: (3,4),(4,3) join
    CHECK(t1.metrics.burning == 3);
    CHECK(t1.out.rejected.size() == 1);  // the scenario ignition was refused (accepts none)
    CHECK(t1.out.rejected[0].kind == DeltaKind::IgnitionForced);
    CHECK(t1.out.rejected[0].count == 2);
    const TickResult& t2 = sess.step();  // t=1200: (4,4)
    CHECK(t2.metrics.burning == 4);
    CHECK(t2.metrics.burned == 0);
    const TickResult& t3 = sess.step();
    CHECK(t3.metrics.burning == 4);
    CHECK(sess.done());
    CHECK(sess.diagnostics()["rejected_deltas"]["IgnitionForced"] == 2);
    CHECK(sess.diagnostics()["weather_held_steps"] == 0);
}

TEST_CASE("runner: errors are actionable") {
    fs::path f = write_scenario("badmodel", "no-such-model", 600, 60);
    CHECK_THROWS_WITH_AS(Session(load_scenario(f)), doctest::Contains("unknown model id"), std::runtime_error);
    fs::path f2 = write_scenario("ca", "ember-ca", 600, 60);
    // In this worktree the CA factory is stubbed to null; either way the error names the model.
    CHECK_THROWS_WITH_AS(Session(load_scenario(f2)), doctest::Contains("ember-ca"), std::runtime_error);
    CHECK_THROWS(resolve_pack("C:/no/such/pack.toml", "x"));
    CHECK(fs::exists(resolve_pack({}, "ca_params.v1.toml")));
}

TEST_CASE("runner: C API stepping mirrors the runner") {
    fs::path f = write_scenario("capi", "arrival-playback", 1800, 600);
    char err[512] = {0};
    es_sim* sim = es_sim_create(f.string().c_str(), err, sizeof err);
    REQUIRE(sim != nullptr);
    uint32_t nx = 0, ny = 0, mm = 0;
    es_sim_grid(sim, &nx, &ny, &mm);
    CHECK(nx == 8);
    CHECK(mm == 30000);
    CHECK(es_sim_step(sim, 2, err, sizeof err) == 2);
    CHECK(es_sim_tick(sim) == 2);
    CHECK(es_sim_time_s(sim) == 1200);
    CHECK(es_sim_burning_cells(sim) == 4);
    CHECK(es_sim_phase(sim)[4 * 8 + 4] == 2);
    CHECK(es_sim_step(sim, 10, err, sizeof err) == 1);  // only one tick left
    CHECK(es_sim_issue_command(sim, "kind = \"cut_line\"", err, sizeof err) != 0);
    CHECK(std::string(err).find("not supported") != std::string::npos);
    es_sim_destroy(sim);
    CHECK(es_sim_create("C:/no/such.toml", err, sizeof err) == nullptr);
    CHECK(std::string(es_interface_version()) == INTERFACE_VERSION);
    CHECK(es_run_scenario(f.string().c_str(), nullptr, err, sizeof err) == 0);
    CHECK(es_replay_verify((f.parent_path() / "out" / "capi.replay.json").string().c_str(), err, sizeof err) == 0);
}

TEST_CASE("dead_fuel_m10 follows the spec cartoon") {
    CHECK(dead_fuel_m10(250, 2981) == 60);   // 25 % RH, 25 C -> 6 %
    CHECK(dead_fuel_m10(1000, 2981) == 263); // 100 % -> 26.3 %
    CHECK(dead_fuel_m10(250, 2800) == 80);   // cold: +2 %
    CHECK(dead_fuel_m10(250, 3100) == 50);   // hot: -1 %
}

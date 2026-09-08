#include <filesystem>
#include <string>

#include "doctest.h"
#include "scenario.h"
#include "testworld.h"
#include "worldpack.h"

using namespace embersim;
namespace fs = std::filesystem;

static const char* EXAMPLE = R"(
scenario_version = 1
[scenario]
name = "cp3-point-ignition"
world = "../store/sim/hist-jolly-mountain-2017.ewp"
t_start_s = 0
duration_s = 86400
dt_s = 60
seed = 20260907

[model]
id = "ember-ca"
params = "../sim/packs/ca_params.v1.toml"
[model.overrides]
"spotting.enabled" = false
"class.GR.base_rate_mms" = 30

[weather]
mode = "constant"
[weather.constant]
wind10_u_cms = 350
wind10_v_cms = 0
t2_dk = 2981
rh2_dpct = 250
precip_cmm = 0

[[ignitions]]
t_s = 0
cells = [[400, 300]]
cause = "scenario"

[[resources]]
id = "hc-1"
type = "hand_t1"
[[resources]]
id = "at-1"
type = "airtanker_large"

[[commands]]
t_s = 7200
kind = "burnout"
resource_id = "hc-1"
anchor_path = [[380, 280], [420, 280]]

[[commands]]
t_s = 3600
kind = "cut_line"
resource_id = "hc-1"
path = [[380, 280], [420, 280]]
method = "hand"

[[commands]]
t_s = 3600
kind = "air_drop"
resource_id = "at-1"
target = [[390, 290]]
agent = "retardant"
volume_class = 2

[output]
dir = "runs/cp3-point-ignition"
keyframe_every = 30
stream = true
)";

TEST_CASE("scenario: the formats.md example parses and resolves paths") {
    fs::path as_if = fs::path("C:/proj/scenarios/cp3.scenario.toml");
    Scenario s = scenario_from_text(EXAMPLE, as_if);
    CHECK(s.name == "cp3-point-ignition");
    CHECK(s.world_path.generic_string() == "C:/proj/store/sim/hist-jolly-mountain-2017.ewp");
    CHECK(s.params_path.generic_string() == "C:/proj/sim/packs/ca_params.v1.toml");
    CHECK(s.output_dir.generic_string() == "C:/proj/scenarios/runs/cp3-point-ignition");
    CHECK(s.duration_s == 86400);
    CHECK(s.dt_s == 60);
    CHECK(s.seed == 20260907u);
    CHECK(s.model_id == "ember-ca");
    CHECK(s.overrides.at("spotting.enabled") == "false");
    CHECK(s.overrides.at("class.GR.base_rate_mms") == "30");
    CHECK(s.weather_mode == "constant");
    CHECK(s.weather_constant.u_cms == 350);
    REQUIRE(s.ignitions.size() == 1);
    CHECK(s.ignitions[0].cells[0] == std::make_pair(400, 300));
    CHECK(s.ignitions[0].cause == IgnitionCause::Scenario);
    REQUIRE(s.resources.size() == 2);
    REQUIRE(s.commands.size() == 3);
    // sorted by (t_s, order): cut_line (3600, order 1), air_drop (3600, order 2), burnout (7200, order 0)
    CHECK(s.commands[0].kind == "cut_line");
    CHECK(s.commands[0].order == 1);
    CHECK(s.commands[1].kind == "air_drop");
    CHECK(s.commands[1].volume_class == 2);
    CHECK(s.commands[1].agent == "retardant");
    CHECK(s.commands[2].kind == "burnout");
    CHECK(s.commands[2].points.size() == 2);
    CHECK(s.keyframe_every == 30);
    CHECK(s.stream);
    CHECK(s.raw_toml == std::string(EXAMPLE));

    Delta d = ignition_delta(s.ignitions[0], 800);
    CHECK(d.kind == DeltaKind::IgnitionForced);
    CHECK(d.cells[0] == 300u * 800 + 400);
}

TEST_CASE("scenario: structural errors are named") {
    fs::path f = "x.toml";
    CHECK_THROWS_WITH_AS(scenario_from_text("scenario_version = 2\n[scenario]\nname='a'\nworld='w'\nduration_s=1", f),
                         doctest::Contains("scenario_version"), std::runtime_error);
    CHECK_THROWS_WITH_AS(scenario_from_text("scenario_version = 1\n[scenario]\nworld='w'\nduration_s=1", f),
                         doctest::Contains("name"), std::runtime_error);
    CHECK_THROWS_WITH_AS(scenario_from_text("scenario_version = 1\n[scenario]\nname='a'\nworld='w'", f),
                         doctest::Contains("duration_s"), std::runtime_error);
    CHECK_THROWS_WITH_AS(scenario_from_text("scenario_version = 1\n[scenario]\nname='a'\nworld='w'\nduration_s=1\n"
                                            "[[commands]]\nkind='hold'\nresource_id='x'", f),
                         doctest::Contains("hold"), std::runtime_error);
    CHECK_THROWS_WITH_AS(scenario_from_text("scenario_version = 1\n[scenario]\nname='a'\nworld='w'\nduration_s=1\n"
                                            "[[commands]]\nkind='dig'\nresource_id='x'", f),
                         doctest::Contains("unknown kind"), std::runtime_error);
    CHECK_THROWS_WITH_AS(scenario_from_text("scenario_version = 1\n[scenario]\nname='a'\nworld='w'\nduration_s=1\n"
                                            "[[ignitions]]\ncells=[[1,2,3]]", f),
                         doctest::Contains("[x, y]"), std::runtime_error);
    CHECK_THROWS_WITH_AS(scenario_from_text("this is not = toml =", f), doctest::Contains("x.toml"), std::runtime_error);
}

TEST_CASE("scenario: grid-dependent validation") {
    test::WorldSpec spec;
    spec.nx = 16;
    spec.ny = 16;
    World w = test::make_world(spec);
    auto base = [](const std::string& extra) {
        return std::string("scenario_version = 1\n[scenario]\nname='a'\nworld='w'\nduration_s=600\ndt_s=60\n") + extra;
    };
    CHECK_NOTHROW(validate_scenario(scenario_from_text(base("[[ignitions]]\ncells=[[15,15]]"), "x.toml"), w));
    CHECK_THROWS_WITH_AS(validate_scenario(scenario_from_text(base("[[ignitions]]\ncells=[[16,0]]"), "x.toml"), w),
                         doctest::Contains("off the 16x16 grid"), std::runtime_error);
    CHECK_THROWS_WITH_AS(validate_scenario(scenario_from_text(base("dt_s=0"), "x.toml"), w), doctest::Contains("dt_s"),
                         std::runtime_error);
    CHECK_THROWS_WITH_AS(validate_scenario(scenario_from_text(base("[weather]\nmode='hrrr'"), "x.toml"), w),
                         doctest::Contains("mode"), std::runtime_error);
    std::string res = "[[resources]]\nid='hc'\ntype='hand_t2'\n[[resources]]\nid='dz'\ntype='dozer_t2'\n[[resources]]\nid='at'\ntype='airtanker_seat'\n";
    CHECK_THROWS_WITH_AS(
        validate_scenario(scenario_from_text(base(res + "[[commands]]\nkind='cut_line'\nresource_id='nope'\npath=[[1,1],[2,2]]\nmethod='hand'"), "x.toml"), w),
        doctest::Contains("unknown resource_id"), std::runtime_error);
    CHECK_THROWS_WITH_AS(
        validate_scenario(scenario_from_text(base(res + "[[commands]]\nkind='cut_line'\nresource_id='hc'\npath=[[1,1],[2,2]]\nmethod='dozer'"), "x.toml"), w),
        doctest::Contains("does not match"), std::runtime_error);
    CHECK_THROWS_WITH_AS(
        validate_scenario(scenario_from_text(base(res + "[[commands]]\nkind='cut_line'\nresource_id='dz'\npath=[[1,1]]\nmethod='dozer'"), "x.toml"), w),
        doctest::Contains(">= 2 points"), std::runtime_error);
    CHECK_THROWS_WITH_AS(
        validate_scenario(scenario_from_text(base(res + "[[commands]]\nkind='air_drop'\nresource_id='hc'\ntarget=[[1,1]]\nagent='water'"), "x.toml"), w),
        doctest::Contains("air resource"), std::runtime_error);
    CHECK_THROWS_WITH_AS(
        validate_scenario(scenario_from_text(base(res + "[[commands]]\nkind='mop_up'\nresource_id='hc'\nregion=[[1,1],[2,2]]"), "x.toml"), w),
        doctest::Contains(">= 3 points"), std::runtime_error);
    CHECK_THROWS_WITH_AS(
        validate_scenario(scenario_from_text(base(res + "[[commands]]\nkind='burnout'\nresource_id='hc'\nanchor_path=[[1,1],[40,2]]"), "x.toml"), w),
        doctest::Contains("off the grid"), std::runtime_error);
    CHECK_NOTHROW(validate_scenario(
        scenario_from_text(base(res + "[[commands]]\nkind='air_drop'\nresource_id='at'\ntarget=[[1,1],[3,3]]\nagent='retardant'\n"
                                      "[[commands]]\nkind='cut_line'\nresource_id='dz'\npath=[[1,1],[2,2]]\nmethod='dozer'\n"
                                      "[[commands]]\nkind='mop_up'\nresource_id='hc'\nregion=[[1,1],[5,1],[5,5]]"),
                           "x.toml"),
        w));
    CHECK_THROWS_WITH_AS(validate_scenario(scenario_from_text(base("[[resources]]\nid='a'\ntype='hand_t1'\n[[resources]]\nid='a'\ntype='hand_t1'"), "x.toml"), w),
                         doctest::Contains("duplicate"), std::runtime_error);
}

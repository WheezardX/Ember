#pragma once
// Scenario file (formats.md §3) + command schema (docs/sim/suppression.md §1–2).
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../third_party/toml.hpp"
#include "interface.h"
#include "weather.h"

namespace embersim {

struct World;

struct Ignition {
    int32_t t_s = 0;
    std::vector<std::pair<int32_t, int32_t>> cells;  // (x, y)
    IgnitionCause cause = IgnitionCause::Scenario;
};

struct ResourceSpec {
    std::string id;
    std::string type;  // hand_t1 | hand_t2 | dozer_t1 | dozer_t2 | dozer_t3 | airtanker_large | airtanker_seat | helicopter_bucket
};

struct Command {
    int32_t t_s = 0;
    std::string kind;         // cut_line | air_drop | burnout | mop_up | hold
    std::string resource_id;
    std::vector<std::pair<int32_t, int32_t>> points;  // path / target / anchor_path / region (cells)
    std::string method;       // cut_line: hand | dozer
    std::string agent;        // air_drop: retardant | water
    int volume_class = 1;     // air_drop
    std::string firing_pattern = "strip";  // burnout
    int32_t depth_m = 30;     // mop_up
    uint32_t order = 0;       // position in the file (tie-break for sorting)
};

struct Scenario {
    std::filesystem::path file;  // absolute path of the scenario file (relative paths resolve against its dir)
    std::string name;
    std::filesystem::path world_path;
    int32_t t_start_s = 0;
    bool ignite_from_arrival = false;  // ignite every cell with 0 <= arrival_s <= t_start_s at t_start_s (shadow runs)
    int32_t duration_s = 0;
    int32_t dt_s = 60;
    uint64_t seed = 0;

    std::string model_id = "ember-ca";
    std::filesystem::path params_path;  // empty = the model's default pack
    std::map<std::string, std::string> overrides;  // dotted key -> TOML value text

    std::string weather_mode = "pack";  // pack | constant
    WeatherSample weather_constant;

    std::vector<Ignition> ignitions;
    std::vector<ResourceSpec> resources;
    std::vector<Command> commands;  // sorted by (t_s, order) after load
    std::filesystem::path production_pack_path;  // empty = default

    std::filesystem::path output_dir;
    uint32_t keyframe_every = 60;
    uint32_t metrics_every = 1;  // observer cadence in ticks (HUD values carry between)
    bool stream = true;

    std::string raw_toml;  // verbatim file text (recorded in the replay)
};

// Parses and structurally validates (types, required keys, path resolution, command sort).
// Grid-dependent validation (on-grid cells, resource/method match) is validate_scenario().
Scenario load_scenario(const std::filesystem::path& file);
Scenario scenario_from_text(const std::string& toml_text, const std::filesystem::path& as_if_file);
void validate_scenario(const Scenario& s, const World& w);  // throws std::runtime_error

// Expand an Ignition into an IgnitionForced delta on a grid.
Delta ignition_delta(const Ignition& ig, uint32_t nx);

}  // namespace embersim

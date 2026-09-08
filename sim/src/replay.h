#pragma once
// Replay file v1 (formats.md §5): commands + seeds + versions + world/params pins + result hashes.
#include <filesystem>
#include <string>

#include "../third_party/json.hpp"
#include "../third_party/toml.hpp"
#include "interface.h"
#include "params.h"
#include "runner.h"
#include "scenario.h"
#include "worldpack.h"

namespace embersim {

nlohmann::json toml_to_json(const toml::node& n);
nlohmann::json scenario_to_json(const Scenario& s);
nlohmann::json command_to_json(const Command& c);

// `stream_name` is the stream file name relative to the replay's directory, or empty.
void write_replay(const std::filesystem::path& path, const Scenario& s, const World& w, const ParamsPack& p,
                  const Caps& caps, const RunResult& r, const std::string& stream_name);

// Parses and checks format/version; throws std::runtime_error on structural problems.
nlohmann::json load_replay(const std::filesystem::path& path);

}  // namespace embersim

#pragma once
// Headless runner (story 4.4): scenario -> run -> state stream + replay; and replay verification.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../third_party/json.hpp"
#include "scenario.h"

namespace embersim {

struct RunOptions {
    std::optional<std::filesystem::path> output_dir;  // overrides scenario [output].dir
    std::optional<bool> write_stream;                 // overrides [output].stream
    bool quiet = false;
    uint32_t checkpoint_every = 0;  // ticks between recorded hashes in the replay (0 = keyframe_every)
    // Called after every tick (progress / tests). Return false to abort.
    std::function<bool(uint32_t tick, int32_t t_s, uint64_t hash)> on_tick;
};

struct RunResult {
    uint32_t ticks = 0;
    uint64_t final_hash = 0;
    std::vector<std::pair<uint32_t, uint64_t>> checkpoints;  // (tick, hash)
    std::filesystem::path replay_path, stream_path;
    double load_ms = 0, run_ms = 0;
    nlohmann::json diagnostics;  // weather_held_steps, rejected_deltas, ...
};

// Loads the world/params/packs named by the scenario, runs it, writes outputs. Throws
// std::runtime_error with actionable messages (which file, which field).
RunResult run_scenario(const Scenario& s, const RunOptions& opt);

// Loads a replay, re-loads the pinned world + params (refusing on pin/version mismatch with a
// message naming the field), re-simulates, and compares every recorded checkpoint hash.
// Returns 0 on match, 1 on mismatch, 2 on refusal; `report` receives a human-readable summary.
int replay_verify(const std::filesystem::path& replay_json, std::string& report, bool quiet);

// Pack lookup: explicit path, else EMBERSIM_PACKS env var, else the compiled-in packs dir.
std::filesystem::path resolve_pack(const std::filesystem::path& explicit_path, const std::string& default_name);

}  // namespace embersim

#include "replay.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "version.h"

namespace embersim {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
std::string hex64(uint64_t v) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::setw(16) << std::setfill('0') << v;
    return ss.str();
}
std::string generic(const fs::path& p) { return p.generic_string(); }
std::string relative_or_abs(const fs::path& p, const fs::path& base) {
    std::error_code ec;
    fs::path rel = fs::relative(p, base, ec);
    if (ec || rel.empty()) return generic(p);
    return generic(rel);
}
}  // namespace

json toml_to_json(const toml::node& n) {
    if (const toml::table* t = n.as_table()) {
        json o = json::object();
        for (const auto& [k, v] : *t) o[std::string(k.str())] = toml_to_json(v);
        return o;
    }
    if (const toml::array* a = n.as_array()) {
        json arr = json::array();
        for (const auto& v : *a) arr.push_back(toml_to_json(v));
        return arr;
    }
    if (auto v = n.value<std::string>()) return *v;
    if (auto v = n.value<int64_t>()) return *v;
    if (auto v = n.value<double>()) return *v;
    if (auto v = n.value<bool>()) return *v;
    std::ostringstream ss;
    n.visit([&](auto&& v) { ss << v; });
    return ss.str();
}

json command_to_json(const Command& c) {
    json j;
    j["t_s"] = c.t_s;
    j["kind"] = c.kind;
    j["resource_id"] = c.resource_id;
    json pts = json::array();
    for (const auto& [x, y] : c.points) pts.push_back({x, y});
    j["points"] = pts;
    if (c.kind == "cut_line") j["method"] = c.method;
    if (c.kind == "air_drop") {
        j["agent"] = c.agent;
        j["volume_class"] = c.volume_class;
    }
    if (c.kind == "burnout") j["firing_pattern"] = c.firing_pattern;
    if (c.kind == "mop_up") j["depth_m"] = c.depth_m;
    j["order"] = c.order;
    return j;
}

json scenario_to_json(const Scenario& s) {
    json j;
    j["file"] = generic(s.file);
    j["name"] = s.name;
    j["world"] = generic(s.world_path);
    j["t_start_s"] = s.t_start_s;
    j["duration_s"] = s.duration_s;
    j["dt_s"] = s.dt_s;
    j["seed"] = s.seed;
    j["model_id"] = s.model_id;
    j["params"] = s.params_path.empty() ? json(nullptr) : json(generic(s.params_path));
    j["overrides"] = s.overrides;
    j["weather"] = {{"mode", s.weather_mode},
                    {"constant", {{"wind10_u_cms", s.weather_constant.u_cms},
                                  {"wind10_v_cms", s.weather_constant.v_cms},
                                  {"t2_dk", s.weather_constant.t2_dk},
                                  {"rh2_dpct", s.weather_constant.rh2_dpct},
                                  {"precip_cmm", s.weather_constant.precip_cmm}}}};
    json igs = json::array();
    for (const Ignition& ig : s.ignitions) {
        json cells = json::array();
        for (const auto& [x, y] : ig.cells) cells.push_back({x, y});
        igs.push_back({{"t_s", ig.t_s}, {"cells", cells}, {"cause", static_cast<int>(ig.cause)}});
    }
    j["ignitions"] = igs;
    json rs = json::array();
    for (const ResourceSpec& r : s.resources) rs.push_back({{"id", r.id}, {"type", r.type}});
    j["resources"] = rs;
    j["production_pack"] = s.production_pack_path.empty() ? json(nullptr) : json(generic(s.production_pack_path));
    j["output"] = {{"dir", generic(s.output_dir)}, {"keyframe_every", s.keyframe_every}, {"stream", s.stream}};
    j["raw_toml"] = s.raw_toml;
    return j;
}

void write_replay(const fs::path& path, const Scenario& s, const World& w, const ParamsPack& p, const Caps& caps,
                  const RunResult& r, const std::string& stream_name) {
    fs::path dir = path.parent_path();
    json j;
    j["format"] = "ember-replay";
    j["version"] = REPLAY_VERSION;
    j["interface_version"] = INTERFACE_VERSION;
    j["embersim_version"] = EMBERSIM_VERSION;
    j["commands_version"] = COMMANDS_VERSION;
    j["stream_version"] = STREAM_VERSION;
    j["model"] = {{"id", caps.model_id},
                  {"version", caps.model_version},
                  {"params_file", p.path.empty() ? json(nullptr) : json(generic(p.path))},
                  {"params_file_relative", p.path.empty() ? json(nullptr) : json(relative_or_abs(p.path, dir))},
                  {"params_sha256", p.sha256},
                  {"overrides", p.overrides}};
    j["world"] = {{"pack", generic(w.pack_dir)},
                  {"pack_relative", relative_or_abs(w.pack_dir, dir)},
                  {"pack_sha256", w.pack_sha256},
                  {"world_manifest_hash", w.world_manifest_hash},
                  {"name", w.name},
                  {"grid", {{"nx", w.grid.nx}, {"ny", w.grid.ny}, {"cell_size_m", w.grid.cell_size_m},
                            {"crs", w.grid.crs}, {"origin_x", w.grid.origin_x}, {"origin_y", w.grid.origin_y}}},
                  {"t0_unix", w.t0_unix}};
    j["scenario"] = scenario_to_json(s);
    j["seeds"] = {{"run_seed", s.seed}};
    json cmds = json::array();
    for (const Command& c : s.commands) cmds.push_back(command_to_json(c));
    j["commands"] = cmds;
    json cps = json::array();
    for (const auto& [tick, hash] : r.checkpoints) cps.push_back({{"tick", tick}, {"hash", hex64(hash)}});
    j["result"] = {{"ticks", r.ticks},
                   {"final_state_hash", hex64(r.final_hash)},
                   {"checkpoints", cps},
                   {"diagnostics", r.diagnostics},
                   {"timing", {{"load_ms", r.load_ms}, {"run_ms", r.run_ms},
                               {"ticks_per_s", r.run_ms > 0 ? r.ticks * 1000.0 / r.run_ms : 0.0}}}};
    j["stream"] = stream_name.empty() ? json(nullptr) : json(stream_name);
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("replay: cannot create " + path.string());
    f << j.dump(2) << "\n";
}

json load_replay(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("replay: cannot open " + path.string());
    json j;
    try {
        j = json::parse(f);
    } catch (const std::exception& e) {
        throw std::runtime_error("replay: " + path.string() + " is not valid JSON: " + e.what());
    }
    if (j.value("format", "") != "ember-replay")
        throw std::runtime_error("replay: " + path.string() + ": format is not ember-replay");
    for (const char* k : {"version", "interface_version", "model", "world", "scenario", "result"})
        if (!j.contains(k)) throw std::runtime_error("replay: " + path.string() + ": missing field '" + k + "'");
    return j;
}

}  // namespace embersim

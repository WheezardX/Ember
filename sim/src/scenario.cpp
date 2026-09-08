#include "scenario.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#include "version.h"
#include "worldpack.h"

namespace embersim {

namespace fs = std::filesystem;

namespace {

[[noreturn]] void fail(const Scenario& s, const std::string& msg) {
    throw std::runtime_error("scenario " + s.file.string() + ": " + msg);
}

fs::path resolve_rel(const Scenario& s, const std::string& p) {
    fs::path pp(p);
    if (pp.is_absolute()) return pp.lexically_normal();
    return (s.file.parent_path() / pp).lexically_normal();
}

std::string node_to_text(const toml::node& n) {
    std::ostringstream ss;
    n.visit([&](auto&& v) { ss << v; });
    return ss.str();
}

std::vector<std::pair<int32_t, int32_t>> parse_points(const Scenario& s, const toml::node* n, const std::string& where) {
    std::vector<std::pair<int32_t, int32_t>> pts;
    const toml::array* arr = n ? n->as_array() : nullptr;
    if (!arr) fail(s, where + " must be an array of [x, y] cell pairs");
    for (size_t i = 0; i < arr->size(); ++i) {
        const toml::array* pair = arr->get(i)->as_array();
        if (!pair || pair->size() != 2 || !pair->get(0)->is_integer() || !pair->get(1)->is_integer())
            fail(s, where + "[" + std::to_string(i) + "] must be [x, y] integers");
        pts.emplace_back(static_cast<int32_t>(pair->get(0)->value_or<int64_t>(0)),
                         static_cast<int32_t>(pair->get(1)->value_or<int64_t>(0)));
    }
    return pts;
}

IgnitionCause parse_cause(const Scenario& s, const std::string& c, const std::string& where) {
    if (c == "scenario") return IgnitionCause::Scenario;
    if (c == "burnout") return IgnitionCause::Burnout;
    if (c == "spot") return IgnitionCause::Spot;
    if (c == "playback") return IgnitionCause::Playback;
    if (c == "other") return IgnitionCause::Other;
    fail(s, where + ": unknown cause '" + c + "' (scenario|burnout|spot|playback|other)");
}

void parse_commands(Scenario& s, const toml::table& tbl, const std::string& src, uint32_t& order) {
    const toml::array* cmds = tbl["commands"].as_array();
    if (!cmds) return;
    for (size_t i = 0; i < cmds->size(); ++i) {
        std::string where = src + " [[commands]][" + std::to_string(i) + "]";
        const toml::table* c = cmds->get(i)->as_table();
        if (!c) fail(s, where + " must be a table");
        Command cmd;
        cmd.order = order++;
        cmd.t_s = static_cast<int32_t>((*c)["t_s"].value_or<int64_t>(0));
        cmd.kind = (*c)["kind"].value_or<std::string>("");
        cmd.resource_id = (*c)["resource_id"].value_or<std::string>("");
        if (cmd.kind.empty()) fail(s, where + ": 'kind' is required");
        if (cmd.kind == "hold") fail(s, where + ": kind 'hold' is reserved and refused in v1");
        const char* pkey = nullptr;
        if (cmd.kind == "cut_line") pkey = "path";
        else if (cmd.kind == "air_drop") pkey = "target";
        else if (cmd.kind == "burnout") pkey = "anchor_path";
        else if (cmd.kind == "mop_up") pkey = "region";
        else fail(s, where + ": unknown kind '" + cmd.kind + "' (cut_line|air_drop|burnout|mop_up)");
        cmd.points = parse_points(s, c->get(pkey), where + "." + pkey);
        cmd.method = (*c)["method"].value_or<std::string>("");
        cmd.agent = (*c)["agent"].value_or<std::string>("");
        cmd.volume_class = static_cast<int>((*c)["volume_class"].value_or<int64_t>(1));
        cmd.firing_pattern = (*c)["firing_pattern"].value_or<std::string>("strip");
        cmd.depth_m = static_cast<int32_t>((*c)["depth_m"].value_or<int64_t>(30));
        if (cmd.resource_id.empty()) fail(s, where + ": 'resource_id' is required");
        if (cmd.kind == "cut_line" && cmd.method != "hand" && cmd.method != "dozer")
            fail(s, where + ": cut_line.method must be hand|dozer");
        if (cmd.kind == "air_drop" && cmd.agent != "retardant" && cmd.agent != "water")
            fail(s, where + ": air_drop.agent must be retardant|water");
        if (cmd.kind == "air_drop" && (cmd.volume_class < 1 || cmd.volume_class > 3))
            fail(s, where + ": air_drop.volume_class must be 1..3");
        if (cmd.kind == "burnout" && cmd.firing_pattern != "strip")
            fail(s, where + ": burnout.firing_pattern must be 'strip' in v1");
        s.commands.push_back(std::move(cmd));
    }
}

}  // namespace

Scenario scenario_from_text(const std::string& text, const fs::path& as_if_file) {
    Scenario s;
    s.file = fs::absolute(as_if_file).lexically_normal();
    s.raw_toml = text;
    toml::table tbl;
    try {
        tbl = toml::parse(text, s.file.string());
    } catch (const toml::parse_error& e) {
        std::ostringstream m;
        m << e.description() << " (line " << e.source().begin.line << ")";
        fail(s, m.str());
    }
    int64_t sv = tbl["scenario_version"].value_or<int64_t>(0);
    if (sv != SCENARIO_VERSION)
        fail(s, "scenario_version " + std::to_string(sv) + " unsupported (expected " + std::to_string(SCENARIO_VERSION) + ")");

    const toml::table* sc = tbl["scenario"].as_table();
    if (!sc) fail(s, "[scenario] table is required");
    s.name = (*sc)["name"].value_or<std::string>("");
    if (s.name.empty()) fail(s, "[scenario].name is required");
    std::string world = (*sc)["world"].value_or<std::string>("");
    if (world.empty()) fail(s, "[scenario].world is required");
    s.world_path = resolve_rel(s, world);
    s.t_start_s = static_cast<int32_t>((*sc)["t_start_s"].value_or<int64_t>(0));
    s.ignite_from_arrival = (*sc)["ignite_from_arrival"].value_or<bool>(false);
    if (!(*sc)["duration_s"].is_integer()) fail(s, "[scenario].duration_s (integer seconds) is required");
    s.duration_s = static_cast<int32_t>((*sc)["duration_s"].value_or<int64_t>(0));
    s.dt_s = static_cast<int32_t>((*sc)["dt_s"].value_or<int64_t>(60));
    s.seed = static_cast<uint64_t>((*sc)["seed"].value_or<int64_t>(0));

    if (const toml::table* md = tbl["model"].as_table()) {
        s.model_id = (*md)["id"].value_or<std::string>("ember-ca");
        std::string params = (*md)["params"].value_or<std::string>("");
        if (!params.empty()) s.params_path = resolve_rel(s, params);
        if (const toml::table* ov = (*md)["overrides"].as_table()) {
            for (const auto& [k, v] : *ov) s.overrides[std::string(k.str())] = node_to_text(v);
        }
    }

    if (const toml::table* wx = tbl["weather"].as_table()) {
        s.weather_mode = (*wx)["mode"].value_or<std::string>("pack");
        if (const toml::table* c = (*wx)["constant"].as_table()) {
            s.weather_constant.u_cms = static_cast<int32_t>((*c)["wind10_u_cms"].value_or<int64_t>(0));
            s.weather_constant.v_cms = static_cast<int32_t>((*c)["wind10_v_cms"].value_or<int64_t>(0));
            s.weather_constant.t2_dk = static_cast<int32_t>((*c)["t2_dk"].value_or<int64_t>(2981));
            s.weather_constant.rh2_dpct = static_cast<int32_t>((*c)["rh2_dpct"].value_or<int64_t>(250));
            s.weather_constant.precip_cmm = static_cast<int32_t>((*c)["precip_cmm"].value_or<int64_t>(0));
        }
    }

    if (const toml::array* igs = tbl["ignitions"].as_array()) {
        for (size_t i = 0; i < igs->size(); ++i) {
            std::string where = "[[ignitions]][" + std::to_string(i) + "]";
            const toml::table* t = igs->get(i)->as_table();
            if (!t) fail(s, where + " must be a table");
            Ignition ig;
            ig.t_s = static_cast<int32_t>((*t)["t_s"].value_or<int64_t>(0));
            ig.cells = parse_points(s, t->get("cells"), where + ".cells");
            if (ig.cells.empty()) fail(s, where + ".cells must list at least one [x, y]");
            ig.cause = parse_cause(s, (*t)["cause"].value_or<std::string>("scenario"), where);
            s.ignitions.push_back(std::move(ig));
        }
    }

    if (const toml::array* rs = tbl["resources"].as_array()) {
        for (size_t i = 0; i < rs->size(); ++i) {
            std::string where = "[[resources]][" + std::to_string(i) + "]";
            const toml::table* t = rs->get(i)->as_table();
            if (!t) fail(s, where + " must be a table");
            ResourceSpec r;
            r.id = (*t)["id"].value_or<std::string>("");
            r.type = (*t)["type"].value_or<std::string>("");
            if (r.id.empty() || r.type.empty()) fail(s, where + ": id and type are required");
            s.resources.push_back(std::move(r));
        }
    }

    uint32_t order = 0;
    parse_commands(s, tbl, "", order);
    std::string cf = tbl["commands_file"].value_or<std::string>("");
    if (!cf.empty()) {
        fs::path cpath = resolve_rel(s, cf);
        std::ifstream f(cpath);
        if (!f) fail(s, "commands_file not found: " + cpath.string());
        std::stringstream ss;
        ss << f.rdbuf();
        toml::table ct;
        try {
            ct = toml::parse(ss.str(), cpath.string());
        } catch (const toml::parse_error& e) {
            fail(s, "commands_file " + cpath.string() + ": " + std::string(e.description()));
        }
        parse_commands(s, ct, cpath.filename().string(), order);
    }
    std::stable_sort(s.commands.begin(), s.commands.end(), [](const Command& a, const Command& b) {
        return a.t_s != b.t_s ? a.t_s < b.t_s : a.order < b.order;
    });

    if (const toml::table* sp = tbl["suppression"].as_table()) {
        std::string pp = (*sp)["production_pack"].value_or<std::string>("");
        if (!pp.empty()) s.production_pack_path = resolve_rel(s, pp);
    }

    std::string outdir = "runs/" + s.name;
    if (const toml::table* out = tbl["output"].as_table()) {
        if (auto d = (*out)["dir"].value<std::string>()) outdir = *d;
        s.keyframe_every = static_cast<uint32_t>((*out)["keyframe_every"].value_or<int64_t>(60));
        s.metrics_every = static_cast<uint32_t>((*out)["metrics_every"].value_or<int64_t>(1));
        s.stream = (*out)["stream"].value_or<bool>(true);
    }
    s.output_dir = resolve_rel(s, outdir);
    return s;
}

Scenario load_scenario(const fs::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) throw std::runtime_error("scenario: cannot open " + file.string());
    std::stringstream ss;
    ss << f.rdbuf();
    return scenario_from_text(ss.str(), file);
}

void validate_scenario(const Scenario& s, const World& w) {
    if (s.dt_s <= 0) fail(s, "[scenario].dt_s must be > 0");
    if (s.duration_s <= 0) fail(s, "[scenario].duration_s must be > 0");
    if (s.keyframe_every == 0) fail(s, "[output].keyframe_every must be > 0");
    if (s.weather_mode != "pack" && s.weather_mode != "constant")
        fail(s, "[weather].mode must be pack|constant (got '" + s.weather_mode + "')");
    auto on_grid = [&](const std::pair<int32_t, int32_t>& p) { return w.grid.in_bounds(p.first, p.second); };
    for (size_t i = 0; i < s.ignitions.size(); ++i)
        for (size_t j = 0; j < s.ignitions[i].cells.size(); ++j)
            if (!on_grid(s.ignitions[i].cells[j]))
                fail(s, "[[ignitions]][" + std::to_string(i) + "].cells[" + std::to_string(j) + "] = [" +
                            std::to_string(s.ignitions[i].cells[j].first) + ", " +
                            std::to_string(s.ignitions[i].cells[j].second) + "] is off the " +
                            std::to_string(w.grid.nx) + "x" + std::to_string(w.grid.ny) + " grid");
    std::set<std::string> ids;
    for (size_t i = 0; i < s.resources.size(); ++i)
        if (!ids.insert(s.resources[i].id).second)
            fail(s, "[[resources]][" + std::to_string(i) + "]: duplicate id '" + s.resources[i].id + "'");
    auto type_of = [&](const std::string& id) -> const std::string& {
        for (const auto& r : s.resources)
            if (r.id == id) return r.type;
        static const std::string none;
        return none;
    };
    for (size_t i = 0; i < s.commands.size(); ++i) {
        const Command& c = s.commands[i];
        std::string where = "[[commands]][" + std::to_string(i) + "] (" + c.kind + " @ t=" + std::to_string(c.t_s) + ")";
        const std::string& type = type_of(c.resource_id);
        if (type.empty()) fail(s, where + ": unknown resource_id '" + c.resource_id + "'");
        size_t n = c.points.size();
        if (c.kind == "cut_line") {
            if (n < 2) fail(s, where + ": path needs >= 2 points");
            bool hand = type.rfind("hand_", 0) == 0, dozer = type.rfind("dozer_", 0) == 0;
            if ((c.method == "hand" && !hand) || (c.method == "dozer" && !dozer))
                fail(s, where + ": method '" + c.method + "' does not match resource type '" + type + "'");
        } else if (c.kind == "air_drop") {
            if (n < 1 || n > 2) fail(s, where + ": target needs 1 (point) or 2 (segment) points");
            bool air = type.rfind("airtanker_", 0) == 0 || type.rfind("helicopter_", 0) == 0;
            if (!air) fail(s, where + ": air_drop needs an air resource, got type '" + type + "'");
        } else if (c.kind == "burnout") {
            if (n < 2) fail(s, where + ": anchor_path needs >= 2 points");
        } else if (c.kind == "mop_up") {
            if (n < 3) fail(s, where + ": region needs >= 3 points");
        }
        for (size_t j = 0; j < n; ++j)
            if (!on_grid(c.points[j]))
                fail(s, where + ": point " + std::to_string(j) + " = [" + std::to_string(c.points[j].first) + ", " +
                            std::to_string(c.points[j].second) + "] is off the grid");
    }
}

Delta ignition_delta(const Ignition& ig, uint32_t nx) {
    Delta d;
    d.kind = DeltaKind::IgnitionForced;
    d.cause = ig.cause;
    d.cells.reserve(ig.cells.size());
    for (const auto& [x, y] : ig.cells) d.cells.push_back(static_cast<uint32_t>(y) * nx + static_cast<uint32_t>(x));
    return d;
}

}  // namespace embersim

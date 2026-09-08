#include "runner.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "replay.h"
#include "session.h"
#include "stream.h"
#include "version.h"

namespace embersim {

namespace fs = std::filesystem;
using json = nlohmann::json;

fs::path resolve_pack(const fs::path& explicit_path, const std::string& default_name) {
    if (!explicit_path.empty()) {
        if (!fs::exists(explicit_path)) throw std::runtime_error("pack not found: " + explicit_path.string());
        return explicit_path;
    }
    if (const char* env = std::getenv("EMBERSIM_PACKS")) {
        fs::path p = fs::path(env) / default_name;
        if (fs::exists(p)) return p;
    }
    fs::path p = fs::path(EMBERSIM_PACKS_DIR) / default_name;
    if (!fs::exists(p))
        throw std::runtime_error("pack '" + default_name + "' not found (looked in $EMBERSIM_PACKS and " + std::string(EMBERSIM_PACKS_DIR) + ")");
    return p;
}

namespace {
std::string hex64(uint64_t v) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::setw(16) << std::setfill('0') << v;
    return ss.str();
}
uint64_t parse_hex64(const std::string& s) {
    std::string t = s;
    if (t.rfind("0x", 0) == 0) t = t.substr(2);
    return std::stoull(t, nullptr, 16);
}
}  // namespace

RunResult run_scenario(const Scenario& s, const RunOptions& opt) {
    Session sess(s);
    RunResult r;
    r.load_ms = sess.load_ms();
    bool write_stream = opt.write_stream.value_or(s.stream);
    fs::path outdir = opt.output_dir.value_or(s.output_dir);
    fs::create_directories(outdir);
    uint32_t cp_every = opt.checkpoint_every ? opt.checkpoint_every : s.keyframe_every;

    std::unique_ptr<StreamWriter> sw;
    std::string stream_name;
    if (write_stream) {
        stream_name = s.name + ".ess";
        r.stream_path = outdir / stream_name;
        sw = std::make_unique<StreamWriter>(r.stream_path, sess.stream_header());
        sw->keyframe(0, sess.t_s(), sess.state());
    }
    r.checkpoints.emplace_back(0u, sess.state_hash());

    const uint32_t total = sess.total_ticks();
    uint32_t next_progress = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (!sess.done()) {
        const TickResult& tr = sess.step();
        if (sw) {
            sw->tick(tr.tick, tr.t_s, tr.hash, sess.state(), tr.out, tr.overlay, tr.metrics);
            if (tr.tick % s.keyframe_every == 0 && !sess.done()) sw->keyframe(tr.tick, tr.t_s, sess.state());
        }
        if (tr.tick % cp_every == 0 || sess.done()) r.checkpoints.emplace_back(tr.tick, tr.hash);
        if (!opt.quiet && total > 0 && tr.tick * 20 / total >= next_progress) {
            std::fprintf(stderr, "  tick %u/%u  t=%ds  burning=%u burned=%u  hash=%s\n", tr.tick, total, tr.t_s,
                         tr.metrics.burning, tr.metrics.burned, hex64(tr.hash).c_str());
            next_progress = tr.tick * 20 / total + 1;
        }
        if (opt.on_tick && !opt.on_tick(tr.tick, tr.t_s, tr.hash)) break;
    }
    r.run_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    r.ticks = sess.tick();
    r.final_hash = sess.state_hash();
    if (sw) sw->end(r.ticks, r.final_hash);
    r.diagnostics = sess.diagnostics();
    r.diagnostics["timing"] = {{"load_ms", r.load_ms}, {"run_ms", r.run_ms},
                               {"ticks_per_s", r.run_ms > 0 ? r.ticks * 1000.0 / r.run_ms : 0.0}};
    r.replay_path = outdir / (s.name + ".replay.json");
    write_replay(r.replay_path, s, sess.world(), sess.params(), sess.caps(), r, stream_name);
    return r;
}

int replay_verify(const fs::path& replay_json, std::string& report, bool quiet) {
    std::ostringstream rep;
    json j = load_replay(replay_json);
    fs::path dir = fs::absolute(replay_json).parent_path();
    auto refuse = [&](const std::string& field, const std::string& expected, const std::string& got) {
        rep << "REFUSED: " << field << " mismatch\n  replay: " << expected << "\n  found : " << got << "\n";
        report = rep.str();
        return 2;
    };
    if (j.value("version", 0) != REPLAY_VERSION)
        return refuse("version", std::to_string(REPLAY_VERSION), std::to_string(j.value("version", 0)));
    std::string iv = j.value("interface_version", "");
    if (iv.substr(0, iv.find('.')) != std::string(INTERFACE_VERSION).substr(0, std::string(INTERFACE_VERSION).find('.')))
        return refuse("interface_version (major)", INTERFACE_VERSION, iv);
    if (j.value("commands_version", 0) != COMMANDS_VERSION)
        return refuse("commands_version", std::to_string(COMMANDS_VERSION), std::to_string(j.value("commands_version", 0)));
    if (j.value("stream_version", 0) != STREAM_VERSION)
        return refuse("stream_version", std::to_string(STREAM_VERSION), std::to_string(j.value("stream_version", 0)));

    const json& jw = j["world"];
    fs::path pack;
    if (jw.contains("pack_relative") && jw["pack_relative"].is_string() && fs::exists(dir / jw["pack_relative"].get<std::string>()))
        pack = dir / jw["pack_relative"].get<std::string>();
    else
        pack = jw.value("pack", "");
    if (!fs::exists(pack / "world.json")) {
        rep << "REFUSED: world pack not found: " << pack.string() << "\n";
        report = rep.str();
        return 2;
    }
    World probe = load_world(pack);
    if (probe.pack_sha256 != jw.value("pack_sha256", "")) return refuse("world.pack_sha256", jw.value("pack_sha256", ""), probe.pack_sha256);
    if (probe.world_manifest_hash != jw.value("world_manifest_hash", ""))
        return refuse("world.world_manifest_hash", jw.value("world_manifest_hash", ""), probe.world_manifest_hash);

    const json& js = j["scenario"];
    Scenario s = scenario_from_text(js.value("raw_toml", ""), fs::path(js.value("file", "replay.scenario.toml")));
    s.world_path = pack;
    const json& jm = j["model"];
    if (jm.contains("params_file") && jm["params_file"].is_string()) {
        fs::path pf;
        if (jm.contains("params_file_relative") && jm["params_file_relative"].is_string() &&
            fs::exists(dir / jm["params_file_relative"].get<std::string>()))
            pf = dir / jm["params_file_relative"].get<std::string>();
        else
            pf = jm["params_file"].get<std::string>();
        s.params_path = pf;
    }
    s.overrides.clear();
    if (jm.contains("overrides") && jm["overrides"].is_object())
        for (const auto& [k, v] : jm["overrides"].items()) s.overrides[k] = v.get<std::string>();
    s.model_id = jm.value("id", s.model_id);

    Session sess(s);
    if (sess.params().sha256 != jm.value("params_sha256", "")) return refuse("model.params_sha256", jm.value("params_sha256", ""), sess.params().sha256);
    if (sess.caps().model_version != jm.value("version", ""))
        return refuse("model.version", jm.value("version", ""), sess.caps().model_version);

    std::vector<std::pair<uint32_t, uint64_t>> cps;
    for (const auto& c : j["result"]["checkpoints"]) cps.emplace_back(c.value("tick", 0u), parse_hex64(c.value("hash", "0x0")));
    size_t ci = 0;
    int rc = 0;
    auto check = [&](uint32_t tick, uint64_t hash) {
        while (ci < cps.size() && cps[ci].first < tick) ++ci;
        if (ci < cps.size() && cps[ci].first == tick) {
            if (cps[ci].second != hash) {
                rep << "MISMATCH at tick " << tick << ": replay " << hex64(cps[ci].second) << " vs re-sim " << hex64(hash) << "\n";
                rc = 1;
                return false;
            }
            ++ci;
        }
        return true;
    };
    if (!check(0, sess.state_hash())) {
        report = rep.str();
        return rc;
    }
    while (!sess.done()) {
        const TickResult& tr = sess.step();
        if (!check(tr.tick, tr.hash)) break;
    }
    if (rc == 0) {
        uint32_t want = j["result"].value("ticks", 0u);
        if (sess.tick() != want) {
            rep << "MISMATCH: replay ran " << want << " ticks, re-sim ran " << sess.tick() << "\n";
            rc = 1;
        } else {
            rep << "OK: " << cps.size() << " checkpoints match over " << sess.tick() << " ticks; final "
                << hex64(sess.state_hash()) << "\n";
        }
    }
    if (!quiet) std::fputs(rep.str().c_str(), stderr);
    report = rep.str();
    return rc;
}

}  // namespace embersim

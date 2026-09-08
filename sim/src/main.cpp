// embersim CLI: run / replay / info / models / version. Exit codes: 0 ok, 1 replay mismatch,
// 2 replay refused, 3 usage or error (message on stderr).
#include <cstdio>
#include <cstring>
#include <exception>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "interface.h"
#include "runner.h"
#include "scenario.h"
#include "version.h"
#include "worldpack.h"

using namespace embersim;

namespace {

int usage() {
    std::fputs(
        "embersim — Ember sim core (engine-free, deterministic)\n"
        "  embersim run <scenario.toml> [--out DIR] [--no-stream] [--quiet]\n"
        "  embersim replay <file.replay.json> [--quiet]      exit 0 match / 1 mismatch / 2 refused\n"
        "  embersim info <pack.ewp>\n"
        "  embersim models\n"
        "  embersim version\n",
        stderr);
    return 3;
}

std::string hex64(uint64_t v) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::setw(16) << std::setfill('0') << v;
    return ss.str();
}

int cmd_run(int argc, char** argv) {
    if (argc < 3) return usage();
    RunOptions o;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) o.output_dir = std::filesystem::path(argv[++i]);
        else if (a == "--no-stream") o.write_stream = false;
        else if (a == "--quiet") o.quiet = true;
        else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return usage();
        }
    }
    Scenario s = load_scenario(argv[2]);
    if (!o.quiet) std::fprintf(stderr, "run %s: model %s, world %s\n", s.name.c_str(), s.model_id.c_str(), s.world_path.string().c_str());
    RunResult r = run_scenario(s, o);
    std::printf("scenario   : %s\n", s.name.c_str());
    std::printf("ticks      : %u (dt %d s, %d s total)\n", r.ticks, s.dt_s, s.duration_s);
    std::printf("final hash : %s\n", hex64(r.final_hash).c_str());
    std::printf("load / run : %.1f ms / %.1f ms (%.0f ticks/s)\n", r.load_ms, r.run_ms, r.run_ms > 0 ? r.ticks * 1000.0 / r.run_ms : 0.0);
    std::printf("replay     : %s\n", r.replay_path.string().c_str());
    if (!r.stream_path.empty()) std::printf("stream     : %s\n", r.stream_path.string().c_str());
    if (r.diagnostics.contains("rejected_deltas") && !r.diagnostics["rejected_deltas"].empty())
        std::printf("rejected   : %s\n", r.diagnostics["rejected_deltas"].dump().c_str());
    return 0;
}

int cmd_replay(int argc, char** argv) {
    if (argc < 3) return usage();
    bool quiet = false;
    for (int i = 3; i < argc; ++i)
        if (std::strcmp(argv[i], "--quiet") == 0) quiet = true;
    std::string report;
    int rc = replay_verify(argv[2], report, true);
    if (!quiet || rc != 0) std::fputs(report.c_str(), rc == 0 ? stdout : stderr);
    return rc;
}

int cmd_info(int argc, char** argv) {
    if (argc < 3) return usage();
    World w = load_world(argv[2]);
    std::printf("pack       : %s (%s)\n", w.name.c_str(), w.pack_dir.string().c_str());
    std::printf("grid       : %u x %u @ %.1f m  crs %s  origin (%.1f, %.1f)\n", w.grid.nx, w.grid.ny, w.grid.cell_size_m,
                w.grid.crs.c_str(), w.grid.origin_x, w.grid.origin_y);
    std::printf("t0         : %s (unix %lld)\n", w.t0_utc.c_str(), static_cast<long long>(w.t0_unix));
    std::printf("sha256     : %s\n", w.pack_sha256.c_str());
    std::printf("world pin  : %s\n", w.world_manifest_hash.empty() ? "(none)" : w.world_manifest_hash.c_str());
    std::printf("layers     : elevation fbfm40 cc ch cbh cbd evt%s%s%s%s%s\n", w.has_greenness() ? " greenness" : "",
                w.has_structures() ? " structures" : "", w.has_arrival() ? " arrival_s" : "",
                w.has_confidence() ? " confidence" : "", w.has_hillshade() ? " hillshade" : "");
    if (w.weather)
        std::printf("weather    : %u steps x %d s, grid %u x %u @ %.0f m%s\n", w.weather->num_steps, w.weather->step_s,
                    w.weather->nx, w.weather->ny, w.weather->dx_m, w.weather->precip_present ? "" : " (no precip)");
    else
        std::printf("weather    : none\n");
    size_t nodata = 0;
    std::map<FuelClass, size_t> hist;
    for (size_t i = 0; i < w.ncells(); ++i) {
        if (w.elevation_cm[i] == ELEV_NODATA) ++nodata;
        ++hist[fuel_class_of(w.fbfm40[i])];
    }
    std::printf("elev nodata: %zu cells\n", nodata);
    std::printf("fuel class : ");
    for (const auto& [c, n] : hist) std::printf("%s=%zu (%.1f%%) ", fuel_class_name(c), n, 100.0 * n / w.ncells());
    std::printf("\n");
    if (w.has_arrival()) {
        size_t burned = 0;
        int32_t amax = -1;
        for (int32_t a : w.arrival_s)
            if (a >= 0) { ++burned; if (a > amax) amax = a; }
        std::printf("arrival    : %zu burned cells, max %d s (%.1f h)\n", burned, amax, amax / 3600.0);
    }
    return 0;
}

int cmd_models() {
    std::printf("%-18s %-8s %-9s %-7s %-8s %-6s %-7s %s\n", "id", "version", "accepts", "intens", "spotting", "rewind", "max_dt", "streams");
    for (const auto& id : model_ids()) {
        auto m = make_model(id);
        if (!m) {
            std::printf("%-18s (unavailable in this build)\n", id.c_str());
            continue;
        }
        Caps c = m->caps();
        std::string acc;
        for (uint8_t k = 1; k <= 5; ++k) acc += c.accepts_kind(static_cast<DeltaKind>(k)) ? "1" : "0";
        std::string streams;
        for (const auto& s : c.rng_streams) streams += (streams.empty() ? "" : ",") + s;
        std::printf("%-18s %-8s %-9s %-7s %-8s %-6s %-7d %s\n", id.c_str(), c.model_version.c_str(), acc.c_str(),
                    c.provides_intensity ? "yes" : "no", c.provides_spotting ? "yes" : "no", c.supports_rewind ? "yes" : "no",
                    c.max_dt_s, streams.empty() ? "-" : streams.c_str());
    }
    std::printf("accepts bits: FuelRemoved RetardantApplied MoistureBumped IgnitionForced ExtinguishForced\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    try {
        if (cmd == "run") return cmd_run(argc, argv);
        if (cmd == "replay") return cmd_replay(argc, argv);
        if (cmd == "info") return cmd_info(argc, argv);
        if (cmd == "models") return cmd_models();
        if (cmd == "version") {
            std::printf("embersim %s (interface %s, scenario v%d, stream v%d, replay v%d)\n", EMBERSIM_VERSION, INTERFACE_VERSION,
                        SCENARIO_VERSION, STREAM_VERSION, REPLAY_VERSION);
            return 0;
        }
        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 3;
    }
}

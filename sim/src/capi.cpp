// C API (embersim.h) over the runner and Session.
#include "embersim/embersim.h"

#include <cstring>
#include <exception>
#include <memory>
#include <string>

#include "runner.h"
#include "scenario.h"
#include "session.h"
#include "version.h"

using namespace embersim;

struct es_sim {
    std::unique_ptr<Session> session;
};

namespace {
void set_err(char* err, size_t err_len, const std::string& msg) {
    if (!err || err_len == 0) return;
    size_t n = msg.size() < err_len - 1 ? msg.size() : err_len - 1;
    std::memcpy(err, msg.data(), n);
    err[n] = '\0';
}
}  // namespace

extern "C" {

const char* es_interface_version(void) { return INTERFACE_VERSION; }
const char* es_embersim_version(void) { return EMBERSIM_VERSION; }

int es_run_scenario(const char* scenario_path, const char* out_dir, char* err, size_t err_len) {
    try {
        Scenario s = load_scenario(scenario_path);
        RunOptions o;
        o.quiet = true;
        if (out_dir && *out_dir) o.output_dir = std::filesystem::path(out_dir);
        run_scenario(s, o);
        return 0;
    } catch (const std::exception& e) {
        set_err(err, err_len, e.what());
        return -1;
    }
}

int es_replay_verify(const char* replay_path, char* err, size_t err_len) {
    try {
        std::string report;
        int rc = replay_verify(replay_path, report, true);
        if (rc != 0) set_err(err, err_len, report);
        return rc;
    } catch (const std::exception& e) {
        set_err(err, err_len, e.what());
        return -1;
    }
}

es_sim* es_sim_create(const char* scenario_path, char* err, size_t err_len) {
    try {
        auto* sim = new es_sim;
        sim->session = std::make_unique<Session>(load_scenario(scenario_path));
        return sim;
    } catch (const std::exception& e) {
        set_err(err, err_len, e.what());
        return nullptr;
    }
}

void es_sim_destroy(es_sim* sim) { delete sim; }

int es_sim_step(es_sim* sim, uint32_t ticks, char* err, size_t err_len) {
    if (!sim) return -1;
    int n = 0;
    try {
        for (uint32_t i = 0; i < ticks && !sim->session->done(); ++i) {
            sim->session->step();
            ++n;
        }
        return n;
    } catch (const std::exception& e) {
        set_err(err, err_len, e.what());
        return -1;
    }
}

int32_t es_sim_time_s(const es_sim* sim) { return sim->session->t_s(); }
uint32_t es_sim_tick(const es_sim* sim) { return sim->session->tick(); }
uint64_t es_sim_state_hash(const es_sim* sim) { return sim->session->state_hash(); }
void es_sim_grid(const es_sim* sim, uint32_t* nx, uint32_t* ny, uint32_t* cell_mm) {
    const GridInfo& g = sim->session->world().grid;
    if (nx) *nx = g.nx;
    if (ny) *ny = g.ny;
    if (cell_mm) *cell_mm = g.cell_mm;
}
const uint8_t* es_sim_phase(const es_sim* sim) { return sim->session->state().phase; }
const uint8_t* es_sim_intensity(const es_sim* sim) { return sim->session->state().intensity; }
const int32_t* es_sim_arrival_s(const es_sim* sim) { return sim->session->state().arrival_s; }
int32_t es_sim_containment_permyriad(const es_sim* sim) { return sim->session->last().metrics.containment_permyriad; }
uint32_t es_sim_burned_cells(const es_sim* sim) { return sim->session->last().metrics.burned; }
uint32_t es_sim_burning_cells(const es_sim* sim) { return sim->session->last().metrics.burning; }
int64_t es_sim_cost_cents(const es_sim* sim) { return sim->session->last().metrics.cost_cents; }

int es_sim_issue_command(es_sim* sim, const char* command_toml, char* err, size_t err_len) {
    (void)sim;
    (void)command_toml;
    // v1: SuppressionSim takes its command list at init (suppression.h has no late-issue entry
    // point). Gameplay-time issuing lands with the Epic 6 interface revision.
    set_err(err, err_len, "es_sim_issue_command: runtime command issue is not supported in the v1 build");
    return -2;
}

}  // extern "C"

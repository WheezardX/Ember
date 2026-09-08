/* embersim C API (ADR 0008 §1) — the boundary UE, Python (4.8), and other frontends use.
 * Everything is plain C; errors are returned as codes with a message buffer. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMBERSIM_API_VERSION 1

typedef struct es_sim es_sim; /* opaque running simulation */

/* Versions */
const char* es_interface_version(void);
const char* es_embersim_version(void);

/* One-shot: run a scenario file to completion (writes stream/replay per its [output]).
 * out_dir may be NULL. Returns 0 on success; on error returns nonzero and fills err. */
int es_run_scenario(const char* scenario_path, const char* out_dir, char* err, size_t err_len);

/* Verify a replay: 0 match, 1 mismatch, 2 refused (pin/version), <0 error. */
int es_replay_verify(const char* replay_path, char* err, size_t err_len);

/* Stepping API: create from a scenario, step ticks, inspect state, destroy. */
es_sim* es_sim_create(const char* scenario_path, char* err, size_t err_len);
void es_sim_destroy(es_sim* sim);
int es_sim_step(es_sim* sim, uint32_t ticks, char* err, size_t err_len); /* returns ticks actually stepped */
int32_t es_sim_time_s(const es_sim* sim);
uint32_t es_sim_tick(const es_sim* sim);
uint64_t es_sim_state_hash(const es_sim* sim);
void es_sim_grid(const es_sim* sim, uint32_t* nx, uint32_t* ny, uint32_t* cell_mm);
/* Borrowed pointers, valid until the next es_sim_step. */
const uint8_t* es_sim_phase(const es_sim* sim);
const uint8_t* es_sim_intensity(const es_sim* sim);
const int32_t* es_sim_arrival_s(const es_sim* sim);
/* Metrics of the last completed tick. */
int32_t es_sim_containment_permyriad(const es_sim* sim);
uint32_t es_sim_burned_cells(const es_sim* sim);
uint32_t es_sim_burning_cells(const es_sim* sim);
int64_t es_sim_cost_cents(const es_sim* sim);
/* Inject a command at runtime (TOML fragment of one [[commands]] table, without the header).
 * Gameplay (Epic 6) issues commands through this. Returns 0 or error. */
int es_sim_issue_command(es_sim* sim, const char* command_toml, char* err, size_t err_len);

#ifdef __cplusplus
}
#endif

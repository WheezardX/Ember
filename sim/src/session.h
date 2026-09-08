#pragma once
// A loaded, steppable simulation: world + weather + params + model + suppression + observers,
// ticked exactly the way the runner and the C API both need (one tick loop, two callers).
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../third_party/json.hpp"
#include "interface.h"
#include "observers.h"
#include "params.h"
#include "scenario.h"
#include "stream.h"
#include "suppression.h"
#include "weather.h"
#include "worldpack.h"

namespace embersim {

// Dead fuel moisture cartoon (spec §4.3) — used for the HUD's grid-mean m10.
int32_t dead_fuel_m10(int32_t rh2_dpct, int32_t t2_dk);

// Throws when dt_s exceeds the model's declared max_dt_s (ADR 0008 §1).
void check_dt(const Caps& caps, int32_t dt_s);

struct TickResult {
    uint32_t tick = 0;    // 1-based index of the completed tick
    int32_t t_s = 0;      // time at the END of the tick
    uint64_t hash = 0;
    TickOutput out;
    std::vector<OverlayItem> overlay;
    Metrics metrics;
};

class Session {
public:
    explicit Session(const Scenario& scenario);  // loads everything; throws with actionable messages

    bool done() const { return tick_ >= total_ticks_; }
    uint32_t tick() const { return tick_; }
    uint32_t total_ticks() const { return total_ticks_; }
    int32_t t_s() const { return t_; }  // time at the start of the next tick
    const TickResult& step();
    const TickResult& last() const { return last_; }
    uint64_t state_hash() const { return model_->state_hash(); }
    FireStateView state() const { return model_->state(); }

    const Scenario& scenario() const { return scen_; }
    const World& world() const { return world_; }
    const ParamsPack& params() const { return params_; }
    const Caps& caps() const { return caps_; }
    const WeatherSampler& weather() const { return weather_; }
    double load_ms() const { return load_ms_; }
    StreamHeader stream_header() const;
    nlohmann::json diagnostics() const;  // rejected deltas, weather holds, weather mode, last diag

private:
    Scenario scen_;
    World world_;
    WeatherSampler weather_;
    std::string weather_mode_effective_;
    ParamsPack params_;
    std::unique_ptr<IFireModel> model_;
    Caps caps_;
    SuppressionSim supp_;
    bool has_supp_ = false;
    uint16_t secure_permille_ = 500;
    std::vector<Ignition> ignitions_;  // sorted by t_s
    size_t ign_cursor_ = 0;
    uint32_t tick_ = 0, total_ticks_ = 0;
    int32_t t_ = 0, t_end_ = 0;
    TickResult last_;
    std::map<std::string, uint64_t> rejected_;
    double load_ms_ = 0;
};

}  // namespace embersim

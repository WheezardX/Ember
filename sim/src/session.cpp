#include "session.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <stdexcept>

#include "runner.h"
#include "version.h"

namespace embersim {

int32_t dead_fuel_m10(int32_t rh2_dpct, int32_t t2_dk) {
    int32_t rh = rh2_dpct / 10;
    if (rh < 0) rh = 0;
    if (rh > 100) rh = 100;
    int32_t m10 = rh <= 60 ? 10 + 2 * rh : 130 + (rh - 60) * 10 / 3;
    if (t2_dk < 2831) m10 += 20;
    else if (t2_dk > 3031) m10 -= 10;
    return m10 < 0 ? 0 : m10;
}

void check_dt(const Caps& caps, int32_t dt_s) {
    if (caps.max_dt_s > 0 && dt_s > caps.max_dt_s)
        throw std::runtime_error("model '" + caps.model_id + "' declares max_dt_s = " + std::to_string(caps.max_dt_s) +
                                 " but the scenario asks for dt_s = " + std::to_string(dt_s));
}

namespace {
int major_of(const std::string& semver) {
    try {
        return std::stoi(semver.substr(0, semver.find('.')));
    } catch (...) {
        return -1;
    }
}
}  // namespace

Session::Session(const Scenario& scenario) : scen_(scenario) {
    auto t0 = std::chrono::steady_clock::now();
    world_ = load_world(scen_.world_path);
    validate_scenario(scen_, world_);

    model_ = make_model(scen_.model_id);
    if (!model_) {
        const std::vector<std::string> known_ids = model_ids();
        std::string ids;
        for (const auto& id : known_ids) ids += (ids.empty() ? "" : ", ") + id;
        // A registered id whose factory returns null is a build without that model.
        bool known = std::find(known_ids.begin(), known_ids.end(), scen_.model_id) != known_ids.end();
        if (known) throw std::runtime_error("model '" + scen_.model_id + "' is not available in this build");
        throw std::runtime_error("scenario " + scen_.file.string() + ": unknown model id '" + scen_.model_id +
                                 "' (known: " + ids + ")");
    }
    caps_ = model_->caps();
    if (major_of(caps_.interface_version) != major_of(INTERFACE_VERSION))
        throw std::runtime_error("model '" + caps_.model_id + "' implements interface " + caps_.interface_version +
                                 ", runner is " + INTERFACE_VERSION + " (major mismatch)");
    check_dt(caps_, scen_.dt_s);

    // Weather (ADR 0009). "pack" needs a weather pack unless the model does not use weather.
    if (scen_.weather_mode == "pack") {
        if (world_.weather) {
            weather_ = WeatherSampler::from_pack(*world_.weather, world_.grid, world_.t0_unix);
            weather_mode_effective_ = "pack";
        } else if (caps_.uses_weather) {
            throw std::runtime_error("scenario " + scen_.file.string() + ": [weather] mode = \"pack\" but world pack '" +
                                     world_.name + "' carries no weather (use mode = \"constant\")");
        } else {
            weather_ = WeatherSampler::constant(WeatherSample{});
            weather_mode_effective_ = "none";
        }
    } else {
        weather_ = WeatherSampler::constant(scen_.weather_constant);
        weather_mode_effective_ = "constant";
    }

    // Params: explicit, else the model's default pack, else empty.
    if (!scen_.params_path.empty()) params_ = load_params(scen_.params_path);
    else if (scen_.model_id == "ember-ca") params_ = load_params(resolve_pack({}, "ca_params.v1.toml"));
    else params_ = params_from_text("", "<none>");
    for (const auto& [k, v] : scen_.overrides) params_.apply_override(k, v);
    secure_permille_ = static_cast<uint16_t>(params_.get_int("global.retardant_secure_permille", 500));

    Seeds seeds;
    seeds.run_seed = scen_.seed;
    model_->init(world_, &weather_, params_, seeds, scen_.t_start_s);

    if (!scen_.resources.empty() || !scen_.commands.empty()) {
        ParamsPack prod = load_params(resolve_pack(scen_.production_pack_path, "production_rates.nwcg.toml"));
        supp_.init(world_, prod, scen_.resources, scen_.commands, scen_.seed);
        has_supp_ = true;
    }

    reset_shadow();
    ignitions_ = scen_.ignitions;
    if (scen_.ignite_from_arrival) {
        // Shadow-run start: the observed fire state at t_start becomes a forced ignition set.
        if (!world_.has_arrival())
            throw std::runtime_error("scenario " + scen_.file.string() + ": ignite_from_arrival needs a world pack with an arrival_s layer");
        Ignition ig;
        ig.t_s = scen_.t_start_s;
        ig.cause = IgnitionCause::Playback;
        for (uint32_t y = 0; y < world_.grid.ny; ++y)
            for (uint32_t x = 0; x < world_.grid.nx; ++x) {
                int32_t a = world_.arrival_s[static_cast<size_t>(y) * world_.grid.nx + x];
                if (a >= 0 && a <= scen_.t_start_s) ig.cells.emplace_back(static_cast<int32_t>(x), static_cast<int32_t>(y));
            }
        ignitions_.push_back(std::move(ig));
    }
    std::stable_sort(ignitions_.begin(), ignitions_.end(), [](const Ignition& a, const Ignition& b) { return a.t_s < b.t_s; });
    t_ = scen_.t_start_s;
    t_end_ = scen_.t_start_s + scen_.duration_s;
    total_ticks_ = static_cast<uint32_t>((static_cast<int64_t>(scen_.duration_s) + scen_.dt_s - 1) / scen_.dt_s);
    load_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

const TickResult& Session::step() {
    if (done()) return last_;
    const int32_t dt = scen_.dt_s;
    std::vector<Delta> deltas;
    while (ign_cursor_ < ignitions_.size() && ignitions_[ign_cursor_].t_s < t_ + dt) {
        if (ignitions_[ign_cursor_].t_s >= t_) deltas.push_back(ignition_delta(ignitions_[ign_cursor_], world_.grid.nx));
        ++ign_cursor_;
    }
    std::vector<OverlayItem> overlay;
    if (has_supp_) {
        std::vector<Delta> sd = supp_.tick(t_, dt, model_->state());
        for (const Delta& d : sd)
            for (uint32_t idx : d.cells) overlay.push_back({static_cast<uint8_t>(d.kind), idx, d.magnitude, d.resource_idx});
        for (Delta& d : sd) deltas.push_back(std::move(d));
    }
    weather_.set_time(t_);
    last_.out = model_->advance(dt, deltas);
    for (const RejectedDelta& r : last_.out.rejected) rejected_[delta_kind_name(r.kind)] += r.count;
    last_.overlay = std::move(overlay);
    FireStateView view = model_->state();
    apply_dirty(last_.out.dirty);
    if (scen_.metrics_every <= 1 || (tick_ + 1) % scen_.metrics_every == 0 || tick_ + 1 == total_ticks_)
        last_.metrics = compute_metrics(view, world_, secure_permille_);
    if (has_supp_) {
        last_.metrics.cost_cents = supp_.cost_cents();
        last_.metrics.busy_resources = supp_.busy_count();
    }
    WeatherSample m = weather_.mean();
    last_.metrics.wind_u_cms = m.u_cms;
    last_.metrics.wind_v_cms = m.v_cms;
    last_.metrics.m10 = dead_fuel_m10(m.rh2_dpct, m.t2_dk);
    t_ += dt;
    ++tick_;
    last_.tick = tick_;
    last_.t_s = t_;
    last_.hash = run_hash_;
    if (tick_ % scen_.keyframe_every == 0 || done()) {
        uint64_t full = model_->state_hash();
        if (full != run_hash_)
            throw std::runtime_error("model " + caps_.model_id + " violated the dirty-region contract at tick " +
                                     std::to_string(tick_) + ": incremental hash != full hash");
    }
    return last_;
}

void Session::reset_shadow() {
    FireStateView v = model_->state();
    sh_phase_.assign(v.phase, v.phase + v.ncells());
    sh_intensity_.assign(v.intensity, v.intensity + v.ncells());
    sh_arrival_.assign(v.arrival_s, v.arrival_s + v.ncells());
    run_hash_ = hash_state(v);
}

void Session::apply_dirty(const std::vector<uint32_t>& dirty) {
    FireStateView v = model_->state();
    for (uint32_t i : dirty) {
        if (i >= sh_phase_.size()) continue;
        run_hash_ ^= cell_hash(i, sh_phase_[i], sh_intensity_[i], sh_arrival_[i]);
        sh_phase_[i] = v.phase[i];
        sh_intensity_[i] = v.intensity[i];
        sh_arrival_[i] = v.arrival_s[i];
        run_hash_ ^= cell_hash(i, sh_phase_[i], sh_intensity_[i], sh_arrival_[i]);
    }
}

StreamHeader Session::stream_header() const {
    StreamHeader h;
    h.nx = world_.grid.nx;
    h.ny = world_.grid.ny;
    h.cell_mm = world_.grid.cell_mm;
    h.t0_unix = world_.t0_unix;
    h.dt_s = static_cast<uint32_t>(scen_.dt_s);
    h.keyframe_every = scen_.keyframe_every;
    h.flags = 0;
    h.world_pack_sha256_hex = world_.pack_sha256;
    h.model_id = caps_.model_id;
    h.model_version = caps_.model_version;
    h.interface_version = caps_.interface_version;
    for (const ResourceSpec& r : scen_.resources) h.resources.emplace_back(r.id, r.type);
    return h;
}

nlohmann::json Session::diagnostics() const {
    nlohmann::json d;
    d["weather_mode_effective"] = weather_mode_effective_;
    d["weather_held_steps"] = weather_.held_steps();
    d["weather_held_tail_s"] = weather_.held_tail_s();
    d["rejected_deltas"] = rejected_;
    nlohmann::json last = nlohmann::json::object();
    for (const Diag& x : last_.out.diag) last[x.key] = x.value;
    d["last_tick_diag"] = last;
    d["ticks"] = tick_;
    return d;
}

}  // namespace embersim

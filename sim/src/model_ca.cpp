// "ember-ca" v1 — the default game fire model. Reference implementation of
// docs/sim/default-model-spec.md; section numbers in comments refer to that document. The
// tick path is integer-only (spec §0); every random draw is hash64(run_seed, SYS_*, cell,
// tick, k) (spec §6). Behaviour this file has that the spec does not describe is a bug.
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "fixed.h"
#include "hash.h"
#include "interface.h"
#include "models.h"
#include "params.h"
#include "weather.h"
#include "worldpack.h"

namespace embersim {

namespace {

// ---- §1 neighbourhood --------------------------------------------------------------------
// Sixteen directions in the normative order. dy positive = south (raster rows).
constexpr int32_t DX[16] = {1, 1, 0, -1, -1, -1, 0, 1, 2, 1, -1, -2, -2, -1, 1, 2};
constexpr int32_t DY[16] = {0, -1, -1, -1, 0, 1, 1, 1, -1, -2, -2, -1, 1, 2, 2, 1};
// Ring of each direction: 0 = axis (d = 1 cell), 1 = diagonal (√2), 2 = knight (√5).
constexpr int32_t RING[16] = {0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2};
// Unit vectors in Q16: round(65536 * d / |d|). 65536/√2 = 46340.95 -> 46341;
// 65536/√5 = 29308.9 -> 29309; 2*65536/√5 = 58617.8 -> 58617 (Python round(), see hash_test).
constexpr int32_t Q16_1 = 65536, Q16_S2 = 46341, Q16_S5_1 = 29309, Q16_S5_2 = 58617;
constexpr int32_t UX[16] = {Q16_1, Q16_S2, 0, -Q16_S2, -Q16_1, -Q16_S2, 0, Q16_S2,
                            Q16_S5_2, Q16_S5_1, -Q16_S5_1, -Q16_S5_2, -Q16_S5_2, -Q16_S5_1, Q16_S5_1, Q16_S5_2};
constexpr int32_t UY[16] = {0, -Q16_S2, -Q16_1, -Q16_S2, 0, Q16_S2, Q16_1, Q16_S2,
                            -Q16_S5_1, -Q16_S5_2, -Q16_S5_2, -Q16_S5_1, Q16_S5_1, Q16_S5_2, Q16_S5_2, Q16_S5_1};

constexpr int32_t NO_SLOT = -1;
constexpr int64_t MAX_BURNING_S = 24 * 3600;  // hard cap on a cell's burning phase
constexpr uint8_t PH_UNBURNABLE = 0, PH_UNBURNED = 1, PH_BURNING = 2, PH_BURNED = 3;

struct ClassParams {
    int32_t base_rate_mms = 0, mx10 = 0, residence_s = 0, wind_reduction_q8 = 0, k_wind_q8 = 0;
    int32_t intensity_t1_mms = 0, intensity_t2_mms = 0, green_min_q8 = 256, spot_ignite_permille = 0;
    bool herbaceous = false;
};

struct GlobalParams {
    int32_t default_greenness, lb_per_ms_q8, lb_max_q8, head_cap_q8, slope_equiv_cms_per_q8unit, slope_equiv_cap_cms;
    int32_t rain_m10_per_mm, dry_m10_per_hour, f_moist_floor_q8, retardant_eff_q8, retardant_wash_permille_per_mm;
    std::vector<int32_t> retardant_decay_permille_per_hour;
    bool crown_enabled;
    int32_t crown_cbh_max_dm, crown_cbd_min_gm3, crown_cc_min_pct, crown_wind_min_cms, crown_mult_q8;
    bool spot_enabled, spot_test_mode;
    int32_t spot_rate_ppm_per_s, spot_wind_min_cms, spot_wind_ref_cms, spot_dist_min_m, spot_dist_max_m;
    int32_t spot_dist_wind_cap, spot_jitter_deg, spot_flight_mms;
};

struct PendingSpot {
    int32_t land_s;
    uint32_t src;
    uint32_t launch_tick;
    uint32_t dst;
    int32_t launch_s;
    uint64_t key;
    bool operator<(const PendingSpot& o) const {
        if (land_s != o.land_s) return land_s < o.land_s;
        if (src != o.src) return src < o.src;
        return key < o.key;
    }
};

class CaModel final : public IFireModel {
public:
    Caps caps() const override {
        Caps c;
        c.model_id = "ember-ca";
        c.model_version = "1.0.0";
        c.accepts = ACCEPTS_ALL;
        c.provides_intensity = true;
        c.provides_spotting = true;
        c.deterministic = true;
        c.supports_rewind = false;
        c.uses_weather = true;
        c.max_dt_s = 600;
        c.rng_streams = {"spot_launch", "spot_transport", "spot_ignite"};
        return c;
    }

    void init(const World& world, const WeatherSampler* weather, const ParamsPack& params, const Seeds& seeds,
              int32_t t_start_s) override;
    TickOutput advance(int32_t dt_s, std::span<const Delta> deltas) override;

    FireStateView state() const override {
        FireStateView v;
        v.nx = nx_;
        v.ny = ny_;
        v.phase = phase_.data();
        v.intensity = intensity_.data();
        v.arrival_s = arrival_.data();
        v.retardant = retardant_.data();
        return v;
    }
    int32_t now_s() const override { return t_; }

private:
    // ---- params ----
    void load_params(const ParamsPack& p);
    static ClassParams read_class(const ParamsPack& p, const std::string& prefix, const ClassParams* base);

    // ---- helpers ----
    struct CellCtx {
        int32_t vx = 0, vy = 0, vmag = 0;  // effective wind-slope vector (cm/s) and magnitude
        int32_t wx = 0, wy = 0, wmag = 0;  // mid-flame wind only (spotting uses |W|)
        int32_t r_base = 0, r_head = 0;
        int32_t lb_q8 = 256, e_q16 = 0;
    };
    void compute_ctx(uint32_t i, CellCtx& c) const;
    int32_t f_moist_q8(uint32_t i) const;
    int32_t m10_eff(uint32_t i) const;
    void ignite(uint32_t j, int32_t a, int32_t r_head_for_intensity);
    void release_slot(uint32_t i);
    void mark_dirty(uint32_t i) { dirty_.push_back(i); }
    void apply_deltas(std::span<const Delta> deltas, TickOutput& out);
    void sample_weather();
    void land_spots();
    void decay();
    void spread(int32_t dt_s);
    void launch_spots(int32_t dt_s);
    void burnout(int32_t dt_s);

    // ---- static world ----
    uint32_t nx_ = 0, ny_ = 0;
    uint32_t cell_mm_ = 30000;
    uint32_t d_mm_[3] = {30000, 42426, 67082};
    const World* world_ = nullptr;
    const WeatherSampler* wx_ = nullptr;
    WeatherSampler own_wx_;
    bool owns_wx_ = false;
    uint64_t seed_ = 0;
    GlobalParams g_{};
    ClassParams fp_[256];
    FuelClass cls_[256];
    std::vector<int32_t> gx_, gy_;  // Q8 rise/run

    // ---- dynamic state (§2) ----
    int32_t t_ = 0;
    uint32_t tick_ = 0;
    int32_t dt_cur_ = 60;
    std::vector<uint8_t> phase_, intensity_, fuel_, fuel_orig_, crown_, ret_class_, greenness_;
    std::vector<int32_t> arrival_;
    std::vector<int32_t> burnout_at_;   // arrival + residence_s (minimum burning time, see burnout())
    std::vector<uint8_t> waiting_;      // per slot: this tick, some unburned neighbour still waits on us
    std::vector<uint16_t> retardant_;   // permille (view)
    std::vector<uint32_t> ret_acc_;     // permille * 3600 (sub-tick decay resolution)
    std::vector<uint16_t> bump_m10_;
    std::vector<int32_t> bump_until_;
    std::vector<uint32_t> ret_cells_, bump_cells_;  // cells with nonzero retardant / bump

    // active table
    std::vector<int32_t> slot_of_;
    std::vector<std::array<uint32_t, 16>> progress_;
    std::vector<uint32_t> free_slots_;
    std::vector<uint32_t> active_;      // burning cells (sorted at tick start)
    std::vector<uint32_t> snapshot_;    // burning cells at tick start (spread / spot / burnout set)
    std::vector<uint32_t> orphans_;     // burning cells whose fuel was removed (burn out, never spread)

    // per weather cell
    std::vector<int32_t> m10_base_, wet_acc_;  // wet_acc in m10 * 3600

    // per tick scratch
    std::vector<uint32_t> dirty_, proposed_;
    std::vector<int32_t> prop_arrival_, prop_rhead_;
    std::vector<PendingSpot> pending_;
    std::vector<SpotEvent> spot_events_;

    // diag counters
    int64_t ignitions_ = 0, spot_launches_ = 0, spot_ignitions_ = 0, fuel_removed_on_black_ = 0,
            ignition_refused_ = 0, unknown_fuel_cells_ = 0;
};

// ---- params -------------------------------------------------------------------------------

ClassParams CaModel::read_class(const ParamsPack& p, const std::string& prefix, const ClassParams* base) {
    ClassParams c = base ? *base : ClassParams{};
    auto gi = [&](const char* key, int32_t& dst) {
        std::string k = prefix + "." + key;
        if (base) {
            if (p.has(k)) dst = static_cast<int32_t>(p.get_int(k));
        } else {
            dst = static_cast<int32_t>(p.get_int(k));
        }
    };
    gi("base_rate_mms", c.base_rate_mms);
    gi("mx10", c.mx10);
    gi("residence_s", c.residence_s);
    gi("wind_reduction_q8", c.wind_reduction_q8);
    gi("k_wind_q8", c.k_wind_q8);
    gi("intensity_t1_mms", c.intensity_t1_mms);
    gi("intensity_t2_mms", c.intensity_t2_mms);
    gi("green_min_q8", c.green_min_q8);
    gi("spot_ignite_permille", c.spot_ignite_permille);
    std::string hk = prefix + ".herbaceous";
    if (base) {
        if (p.has(hk)) c.herbaceous = p.get_bool(hk);
    } else {
        c.herbaceous = p.get_bool(hk);
    }
    return c;
}

void CaModel::load_params(const ParamsPack& p) {
    auto I = [&](const char* k) { return static_cast<int32_t>(p.get_int(k)); };
    g_.default_greenness = I("global.default_greenness");
    g_.lb_per_ms_q8 = I("global.lb_per_ms_q8");
    g_.lb_max_q8 = I("global.lb_max_q8");
    g_.head_cap_q8 = I("global.head_cap_q8");
    g_.slope_equiv_cms_per_q8unit = I("global.slope_equiv_cms_per_q8unit");
    g_.slope_equiv_cap_cms = I("global.slope_equiv_cap_cms");
    g_.rain_m10_per_mm = I("global.rain_m10_per_mm");
    g_.dry_m10_per_hour = I("global.dry_m10_per_hour");
    g_.f_moist_floor_q8 = I("global.f_moist_floor_q8");
    g_.retardant_eff_q8 = I("global.retardant_eff_q8");
    g_.retardant_wash_permille_per_mm = I("global.retardant_wash_permille_per_mm");
    {
        const toml::node* n = p.find("global.retardant_decay_permille_per_hour");
        const toml::array* arr = n ? n->as_array() : nullptr;
        if (!arr) throw std::runtime_error("params: missing array 'global.retardant_decay_permille_per_hour'");
        g_.retardant_decay_permille_per_hour.clear();
        for (const auto& e : *arr) {
            auto v = e.value<int64_t>();
            if (!v) throw std::runtime_error("params: 'global.retardant_decay_permille_per_hour' must be integers");
            g_.retardant_decay_permille_per_hour.push_back(static_cast<int32_t>(*v));
        }
        if (g_.retardant_decay_permille_per_hour.empty())
            throw std::runtime_error("params: 'global.retardant_decay_permille_per_hour' is empty");
    }
    g_.crown_enabled = p.get_bool("crowning.enabled");
    g_.crown_cbh_max_dm = I("crowning.crown_cbh_max_dm");
    g_.crown_cbd_min_gm3 = I("crowning.crown_cbd_min_gm3");
    g_.crown_cc_min_pct = I("crowning.crown_cc_min_pct");
    g_.crown_wind_min_cms = I("crowning.crown_wind_min_cms");
    g_.crown_mult_q8 = I("crowning.crown_mult_q8");
    g_.spot_enabled = p.get_bool("spotting.enabled");
    g_.spot_test_mode = p.get_bool("spotting.deterministic_test_mode");
    g_.spot_rate_ppm_per_s = I("spotting.spot_rate_ppm_per_s");
    g_.spot_wind_min_cms = I("spotting.spot_wind_min_cms");
    g_.spot_wind_ref_cms = I("spotting.spot_wind_ref_cms");
    g_.spot_dist_min_m = I("spotting.spot_dist_min_m");
    g_.spot_dist_max_m = I("spotting.spot_dist_max_m");
    g_.spot_dist_wind_cap = I("spotting.spot_dist_wind_cap");
    g_.spot_jitter_deg = I("spotting.spot_jitter_deg");
    g_.spot_flight_mms = I("spotting.spot_flight_mms");
    if (g_.spot_wind_ref_cms <= 0 || g_.spot_flight_mms <= 0 || g_.spot_dist_max_m < g_.spot_dist_min_m)
        throw std::runtime_error("params: spotting.* must have spot_wind_ref_cms > 0, spot_flight_mms > 0, dist_max >= dist_min");

    ClassParams by_class[FUEL_CLASS_COUNT];
    for (int c = 0; c < FUEL_CLASS_COUNT; ++c)
        by_class[c] = read_class(p, std::string("class.") + fuel_class_name(static_cast<FuelClass>(c)), nullptr);
    for (int code = 0; code < 256; ++code) {
        FuelClass c = fuel_class_of(static_cast<uint8_t>(code));
        cls_[code] = c;
        fp_[code] = by_class[static_cast<int>(c)];
        std::string key = "fuel." + std::to_string(code);
        if (p.has(key)) fp_[code] = read_class(p, key, &by_class[static_cast<int>(c)]);
        if (fp_[code].mx10 < 0 || fp_[code].base_rate_mms < 0)
            throw std::runtime_error("params: negative rate/mx10 for " + key);
    }
}

// ---- init ---------------------------------------------------------------------------------

void CaModel::init(const World& world, const WeatherSampler* weather, const ParamsPack& params, const Seeds& seeds,
                   int32_t t_start_s) {
    load_params(params);
    world_ = &world;
    nx_ = world.grid.nx;
    ny_ = world.grid.ny;
    cell_mm_ = world.grid.cell_mm;
    // d_k = round(cell_mm * {1, sqrt2, sqrt5}) via integer sqrt of scaled squares (spec §1).
    d_mm_[0] = cell_mm_;
    d_mm_[1] = static_cast<uint32_t>((isqrt64(static_cast<uint64_t>(cell_mm_) * cell_mm_ * 2 * 4) + 1) / 2);
    d_mm_[2] = static_cast<uint32_t>((isqrt64(static_cast<uint64_t>(cell_mm_) * cell_mm_ * 5 * 4) + 1) / 2);
    seed_ = seeds.run_seed;
    if (weather) {
        wx_ = weather;
        owns_wx_ = false;
    } else {
        own_wx_ = WeatherSampler::constant(WeatherSample{0, 0, 2981, 250, 0});
        wx_ = &own_wx_;
        owns_wx_ = true;
    }
    const size_t n = world.ncells();
    phase_.assign(n, PH_UNBURNABLE);
    intensity_.assign(n, 0);
    arrival_.assign(n, -1);
    burnout_at_.assign(n, 0);
    fuel_.assign(n, 0);
    fuel_orig_.assign(n, 0);
    crown_.assign(n, 0);
    ret_class_.assign(n, 0);
    retardant_.assign(n, 0);
    ret_acc_.assign(n, 0);
    bump_m10_.assign(n, 0);
    bump_until_.assign(n, 0);
    slot_of_.assign(n, NO_SLOT);
    prop_arrival_.assign(n, INT32_MAX);
    prop_rhead_.assign(n, 0);
    gx_.assign(n, 0);
    gy_.assign(n, 0);
    greenness_.assign(n, static_cast<uint8_t>(clampi(g_.default_greenness, 0, 255)));
    if (world.has_greenness()) greenness_ = world.greenness;
    progress_.clear();
    waiting_.clear();
    free_slots_.clear();
    active_.clear();
    pending_.clear();
    ret_cells_.clear();
    bump_cells_.clear();
    orphans_.clear();
    ignitions_ = spot_launches_ = spot_ignitions_ = fuel_removed_on_black_ = ignition_refused_ = 0;
    unknown_fuel_cells_ = 0;

    for (size_t i = 0; i < n; ++i) {
        uint8_t code = world.fbfm40[i];
        fuel_[i] = code;
        fuel_orig_[i] = code;
        bool nodata = world.elevation_cm[i] == ELEV_NODATA;
        bool burn = (cls_[code] != FuelClass::NB);
        if (code != 0 && !(code >= 91 && code <= 99) && !burn) ++unknown_fuel_cells_;
        phase_[i] = (!nodata && burn) ? PH_UNBURNED : PH_UNBURNABLE;
    }
    // Gradient in Q8 rise/run (central differences; edges one-sided; nodata neighbours = own).
    const int64_t cell_cm = cell_mm_ / 10;
    for (uint32_t y = 0; y < ny_; ++y) {
        for (uint32_t x = 0; x < nx_; ++x) {
            size_t i = static_cast<size_t>(y) * nx_ + x;
            int64_t e0 = world.elevation_cm[i];
            if (e0 == ELEV_NODATA) continue;
            auto elev = [&](int64_t xx, int64_t yy) -> int64_t {
                if (xx < 0 || yy < 0 || xx >= nx_ || yy >= ny_) return e0;
                int64_t e = world.elevation_cm[static_cast<size_t>(yy) * nx_ + xx];
                return e == ELEV_NODATA ? e0 : e;
            };
            int64_t xm = (x > 0) ? x - 1 : x, xp = (x + 1 < nx_) ? x + 1 : x;
            int64_t ym = (y > 0) ? y - 1 : y, yp = (y + 1 < ny_) ? y + 1 : y;
            int64_t dxc = (xp - xm) * cell_cm, dyc = (yp - ym) * cell_cm;
            gx_[i] = dxc > 0 ? static_cast<int32_t>(((elev(xp, y) - elev(xm, y)) * 256) / dxc) : 0;
            gy_[i] = dyc > 0 ? static_cast<int32_t>(((elev(x, yp) - elev(x, ym)) * 256) / dyc) : 0;
        }
    }
    const uint32_t wc = wx_->wcells();
    m10_base_.assign(wc, 0);
    wet_acc_.assign(wc, 0);
    t_ = t_start_s;
    tick_ = 0;
}

// ---- rate pipeline (§4) -------------------------------------------------------------------

int32_t CaModel::m10_eff(uint32_t i) const {
    uint32_t w = wx_->wcell_of(i % nx_, i / nx_);
    int64_t m = static_cast<int64_t>(m10_base_[w]) + bump_m10_[i] + wet_acc_[w] / 3600;
    return static_cast<int32_t>(clampi<int64_t>(m, 0, 400));
}

int32_t CaModel::f_moist_q8(uint32_t i) const {
    const ClassParams& fp = fp_[fuel_[i]];
    int32_t m = m10_eff(i);
    if (fp.mx10 <= 0 || m >= fp.mx10) return 0;
    int64_t q = (static_cast<int64_t>(fp.mx10 - m) * 256) / fp.mx10;
    int32_t f = static_cast<int32_t>((q * q) >> 8);
    return f < g_.f_moist_floor_q8 ? g_.f_moist_floor_q8 : f;
}

void CaModel::compute_ctx(uint32_t i, CellCtx& c) const {
    const ClassParams& fp = fp_[fuel_[i]];
    const WeatherSample& s = wx_->at_cell(i % nx_, i / nx_);
    // §4.1 mid-flame wind in grid axes: v is northward, grid y is southward -> (u, -v).
    c.wx = static_cast<int32_t>((static_cast<int64_t>(s.u_cms) * fp.wind_reduction_q8) >> 8);
    c.wy = static_cast<int32_t>((static_cast<int64_t>(-s.v_cms) * fp.wind_reduction_q8) >> 8);
    // Note: >> on negative values is an arithmetic shift (C++20 guarantees it): floor semantics.
    // Uphill is the +gradient direction (gx > 0 means elevation rises to the east). The spec
    // (§4.1) writes (-gx, -gy); that is a sign slip in the document, corrected here.
    int64_t sx = (static_cast<int64_t>(gx_[i]) * g_.slope_equiv_cms_per_q8unit) >> 8;
    int64_t sy = (static_cast<int64_t>(gy_[i]) * g_.slope_equiv_cms_per_q8unit) >> 8;
    int64_t smag = static_cast<int64_t>(isqrt64(static_cast<uint64_t>(sx * sx + sy * sy)));
    if (smag > g_.slope_equiv_cap_cms && smag > 0) {
        sx = (sx * g_.slope_equiv_cap_cms) / smag;
        sy = (sy * g_.slope_equiv_cap_cms) / smag;
    }
    c.vx = static_cast<int32_t>(c.wx + sx);
    c.vy = static_cast<int32_t>(c.wy + sy);
    c.wmag = static_cast<int32_t>(isqrt64(static_cast<uint64_t>(static_cast<int64_t>(c.wx) * c.wx + static_cast<int64_t>(c.wy) * c.wy)));
    c.vmag = static_cast<int32_t>(isqrt64(static_cast<uint64_t>(static_cast<int64_t>(c.vx) * c.vx + static_cast<int64_t>(c.vy) * c.vy)));

    // §4 R_base: R0 ×Q8 f_moist ×Q8 f_green ×Q8 f_ret ×Q8 f_crown, in that order.
    int64_t r = fp.base_rate_mms;
    r = (r * f_moist_q8(i)) >> 8;
    int32_t f_green = 256;
    if (fp.herbaceous) f_green = 256 - ((256 - fp.green_min_q8) * greenness_[i]) / 255;
    r = (r * f_green) >> 8;
    int32_t f_ret = 256 - (static_cast<int32_t>(retardant_[i]) * g_.retardant_eff_q8) / 1000;
    r = (r * f_ret) >> 8;
    int32_t f_crown = crown_[i] ? g_.crown_mult_q8 : 256;
    r = (r * f_crown) >> 8;
    c.r_base = static_cast<int32_t>(r);
    int64_t head = (r * (25600 + static_cast<int64_t>(fp.k_wind_q8) * c.vmag)) / 25600;
    int64_t cap = (r * g_.head_cap_q8) >> 8;
    if (head > cap) head = cap;
    c.r_head = static_cast<int32_t>(head);
    // §4.2 ellipse
    int64_t lb = 256 + (static_cast<int64_t>(g_.lb_per_ms_q8) * c.vmag) / 100;
    if (lb > g_.lb_max_q8) lb = g_.lb_max_q8;
    c.lb_q8 = static_cast<int32_t>(lb);
    if (lb <= 256) {
        c.e_q16 = 0;
    } else {
        int64_t inv_lb2_q16 = (static_cast<int64_t>(1) << 32) / (lb * lb);  // Q16 of 1/LB^2
        int64_t e2_q16 = 65536 - inv_lb2_q16;
        c.e_q16 = static_cast<int32_t>(isqrt64(static_cast<uint64_t>(e2_q16 * 65536)));
    }
}

// ---- ignition / slots ---------------------------------------------------------------------

void CaModel::ignite(uint32_t j, int32_t a, int32_t r_head_for_intensity) {
    phase_[j] = PH_BURNING;
    arrival_[j] = a;
    uint32_t slot;
    if (!free_slots_.empty()) {
        slot = free_slots_.back();
        free_slots_.pop_back();
    } else {
        slot = static_cast<uint32_t>(progress_.size());
        progress_.emplace_back();
        waiting_.push_back(1);
    }
    progress_[slot].fill(0);
    waiting_[slot] = 1;  // assume neighbours are waiting until the first spread pass says otherwise
    slot_of_[j] = static_cast<int32_t>(slot);
    active_.push_back(j);
    // §5.3 intensity from the igniting cell's R_head against this cell's thresholds.
    const ClassParams& fp = fp_[fuel_[j]];
    uint8_t cls = 1;
    if (r_head_for_intensity >= fp.intensity_t2_mms) cls = 3;
    else if (r_head_for_intensity >= fp.intensity_t1_mms) cls = 2;
    // §5.4 crowning, decided at ignition from this cell's canopy and its own effective wind.
    crown_[j] = 0;
    CellCtx own;
    compute_ctx(j, own);
    if (g_.crown_enabled && cls >= 2) {
        FuelClass fc = cls_[fuel_[j]];
        if ((fc == FuelClass::TU || fc == FuelClass::TL) && world_->cbh_dm[j] <= g_.crown_cbh_max_dm &&
            world_->cbd_gm3[j] >= g_.crown_cbd_min_gm3 && world_->cc_pct[j] >= g_.crown_cc_min_pct &&
            own.vmag >= g_.crown_wind_min_cms) {
            crown_[j] = 1;
            cls = 3;
        }
    }
    intensity_[j] = cls;
    burnout_at_[j] = static_cast<int32_t>(static_cast<int64_t>(a) + fp.residence_s);
    ++ignitions_;
    mark_dirty(j);
}

void CaModel::release_slot(uint32_t i) {
    int32_t s = slot_of_[i];
    if (s == NO_SLOT) return;
    free_slots_.push_back(static_cast<uint32_t>(s));
    slot_of_[i] = NO_SLOT;
    // Removal from active_ is lazy: compacted at the next tick start (cells with NO_SLOT drop).
}

// ---- §7 deltas ----------------------------------------------------------------------------

void CaModel::apply_deltas(std::span<const Delta> deltas, TickOutput& out) {
    const size_t n = phase_.size();
    for (const Delta& d : deltas) {
        switch (d.kind) {
            case DeltaKind::FuelRemoved:
                for (uint32_t c : d.cells) {
                    if (c >= n) continue;
                    fuel_[c] = 0;
                    if (phase_[c] == PH_BURNING || phase_[c] == PH_BURNED) {
                        ++fuel_removed_on_black_;
                        if (phase_[c] == PH_BURNING && slot_of_[c] != NO_SLOT) {
                            release_slot(c);  // stops spreading; stays black, burns out on schedule
                            orphans_.push_back(c);
                        }
                    } else if (phase_[c] != PH_UNBURNABLE) {
                        phase_[c] = PH_UNBURNABLE;
                        mark_dirty(c);
                    }
                }
                break;
            case DeltaKind::RetardantApplied: {
                uint8_t dc = d.decay_class;
                if (dc >= g_.retardant_decay_permille_per_hour.size())
                    dc = static_cast<uint8_t>(g_.retardant_decay_permille_per_hour.size() - 1);
                for (uint32_t c : d.cells) {
                    if (c >= n) continue;
                    uint32_t v = retardant_[c] + d.magnitude;
                    if (v > 1000) v = 1000;
                    if (retardant_[c] == 0 && v > 0) ret_cells_.push_back(c);
                    if (retardant_[c] != v) mark_dirty(c);
                    retardant_[c] = static_cast<uint16_t>(v);
                    ret_acc_[c] = v * 3600u;
                    ret_class_[c] = dc;
                }
                break;
            }
            case DeltaKind::MoistureBumped:
                for (uint32_t c : d.cells) {
                    if (c >= n) continue;
                    if (bump_m10_[c] == 0 && d.magnitude > 0) bump_cells_.push_back(c);
                    if (d.magnitude > bump_m10_[c]) bump_m10_[c] = d.magnitude;
                    int32_t until = t_ + d.ttl_s;
                    if (until > bump_until_[c]) bump_until_[c] = until;
                }
                break;
            case DeltaKind::IgnitionForced:
                for (uint32_t c : d.cells) {
                    if (c >= n) continue;
                    if (phase_[c] == PH_UNBURNED) {
                        CellCtx ctx;
                        compute_ctx(c, ctx);
                        ignite(c, t_, ctx.r_base);
                    } else {
                        ++ignition_refused_;
                    }
                }
                break;
            case DeltaKind::ExtinguishForced:
                for (uint32_t c : d.cells) {
                    if (c >= n) continue;
                    if (phase_[c] == PH_BURNING) {
                        phase_[c] = PH_BURNED;
                        release_slot(c);
                        mark_dirty(c);
                    }
                }
                break;
        }
    }
    (void)out;
}

// ---- §3 step 2: weather -------------------------------------------------------------------

void CaModel::sample_weather() {
    if (owns_wx_) own_wx_.set_time(t_);
    const uint32_t wc = wx_->wcells();
    const bool step_start = wx_->step_started();
    for (uint32_t w = 0; w < wc; ++w) {
        const WeatherSample& s = wx_->at_wcell(w);
        int32_t rh = clampi(s.rh2_dpct / 10, 0, 100);
        int32_t m10 = rh <= 60 ? 10 + 2 * rh : 130 + ((rh - 60) * 10) / 3;
        if (s.t2_dk < 2831) m10 += 20;
        else if (s.t2_dk > 3031) m10 -= 10;
        m10_base_[w] = m10;
        if (step_start && s.precip_cmm > 0)
            wet_acc_[w] += static_cast<int32_t>((static_cast<int64_t>(s.precip_cmm) * g_.rain_m10_per_mm * 3600) / 100);
    }
}

// ---- §3 step 3: land pending spots ---------------------------------------------------------

void CaModel::land_spots() {
    if (pending_.empty()) return;
    std::sort(pending_.begin(), pending_.end());
    size_t k = 0;
    for (; k < pending_.size() && pending_[k].land_s <= t_; ++k) {
        const PendingSpot& ps = pending_[k];
        SpotEvent ev;
        ev.src = ps.src;
        ev.dst = ps.dst;
        ev.launch_s = ps.launch_s;
        ev.land_s = ps.land_s;
        ev.landed = true;
        ev.ignited = false;
        ev.stream_key = ps.key;
        if (phase_[ps.dst] == PH_UNBURNED) {
            bool ok;
            if (g_.spot_test_mode) {
                ok = true;
            } else {
                uint64_t h3 = hash64(seed_, static_cast<uint64_t>(SYS_SPOT_IGNITE), ps.src, ps.launch_tick, 3);
                int64_t thr = (static_cast<int64_t>(fp_[fuel_[ps.dst]].spot_ignite_permille) * f_moist_q8(ps.dst)) / 256;
                ok = static_cast<int64_t>(h3 % 1000) < thr;
            }
            if (ok) {
                CellCtx c;
                compute_ctx(ps.dst, c);
                ignite(ps.dst, ps.land_s, c.r_base);
                ev.ignited = true;
                ++spot_ignitions_;
            }
        }
        spot_events_.push_back(ev);
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(k));
}

// ---- §3 step 4: decay ---------------------------------------------------------------------

void CaModel::decay() {
    // Retardant: per-tick decay in permille*3600 units, plus rain wash at step start.
    const bool step_start = wx_->step_started();
    size_t keep = 0;
    for (size_t k = 0; k < ret_cells_.size(); ++k) {
        uint32_t c = ret_cells_[k];
        uint32_t acc = ret_acc_[c];
        int32_t rate = g_.retardant_decay_permille_per_hour[ret_class_[c]];
        uint32_t dec = static_cast<uint32_t>(rate) * static_cast<uint32_t>(dt_cur_);
        acc = acc > dec ? acc - dec : 0;
        if (step_start) {
            const WeatherSample& s = wx_->at_cell(c % nx_, c / nx_);
            if (s.precip_cmm > 0) {
                uint64_t wash = (static_cast<uint64_t>(s.precip_cmm) * g_.retardant_wash_permille_per_mm * 3600) / 100;
                acc = acc > wash ? static_cast<uint32_t>(acc - wash) : 0;
            }
        }
        uint16_t perm = static_cast<uint16_t>(acc / 3600);
        if (perm != retardant_[c]) mark_dirty(c);
        ret_acc_[c] = acc;
        retardant_[c] = perm;
        if (acc > 0) ret_cells_[keep++] = c;
    }
    ret_cells_.resize(keep);
    // Moisture bumps expire at bump_until.
    keep = 0;
    for (size_t k = 0; k < bump_cells_.size(); ++k) {
        uint32_t c = bump_cells_[k];
        if (t_ >= bump_until_[c]) {
            bump_m10_[c] = 0;
        } else {
            bump_cells_[keep++] = c;
        }
    }
    bump_cells_.resize(keep);
    // Weather-cell wetness dries.
    const int32_t dry = g_.dry_m10_per_hour * dt_cur_;
    for (auto& w : wet_acc_) w = w > dry ? w - dry : 0;
}

// ---- §3 step 5: spread --------------------------------------------------------------------

void CaModel::spread(int32_t dt_s) {
    proposed_.clear();
    const int64_t NX = nx_, NY = ny_;
    for (uint32_t i : snapshot_) {
        int32_t slot = slot_of_[i];
        if (slot == NO_SLOT || phase_[i] != PH_BURNING) continue;  // released by a delta this tick
        CellCtx c;
        compute_ctx(i, c);
        if (c.r_head <= 0) {
            waiting_[static_cast<size_t>(slot)] = 0;  // extinction: cannot push anyone; burn out on schedule
            continue;
        }
        std::array<uint32_t, 16>& prog = progress_[static_cast<size_t>(slot)];
        const int64_t x = i % nx_, y = i / nx_;
        uint8_t waiting = 0;
        for (int k = 0; k < 16; ++k) {
            int64_t xx = x + DX[k], yy = y + DY[k];
            if (xx < 0 || yy < 0 || xx >= NX || yy >= NY) continue;
            uint32_t j = static_cast<uint32_t>(yy * NX + xx);
            if (phase_[j] != PH_UNBURNED) continue;
            if (RING[k] == 2) {
                // §1 knight-move rule: a (2,1) step may not hop a one-cell fuel break. The two
                // cells the ray passes through must not both be unburnable.
                int64_t ax = DX[k] / 2, ay = DY[k] / 2;  // (±1,0) or (0,±1): the long-axis half step
                int64_t bx = ax + (ax == 0 ? DX[k] : 0), by = ay + (ay == 0 ? DY[k] : 0);
                uint32_t ia = static_cast<uint32_t>((y + ay) * NX + (x + ax));
                uint32_t ib = static_cast<uint32_t>((y + by) * NX + (x + bx));
                if (phase_[ia] == PH_UNBURNABLE && phase_[ib] == PH_UNBURNABLE) continue;
            }
            int64_t r_k;
            if (c.vmag == 0 || c.e_q16 == 0) {
                r_k = c.r_head;
            } else {
                int64_t dot = static_cast<int64_t>(UX[k]) * c.vx + static_cast<int64_t>(UY[k]) * c.vy;
                int64_t cos_q16 = dot / c.vmag;  // signed, truncates toward zero
                int64_t denom = 65536 - ((static_cast<int64_t>(c.e_q16) * cos_q16) >> 16);
                if (denom < 1) denom = 1;
                int64_t ratio_q16 = ((65536 - c.e_q16) * 65536) / denom;
                r_k = (static_cast<int64_t>(c.r_head) * ratio_q16) >> 16;
            }
            if (r_k <= 0) continue;
            const uint32_t d = d_mm_[RING[k]];
            uint32_t p = prog[static_cast<size_t>(k)];
            uint64_t np = static_cast<uint64_t>(p) + static_cast<uint64_t>(r_k) * dt_s;
            if (np > 0xFFFFFFFFull) np = 0xFFFFFFFFull;
            prog[static_cast<size_t>(k)] = static_cast<uint32_t>(np);
            if (np < d) waiting = 1;
            if (np >= d) {
                int32_t a = t_ + static_cast<int32_t>((static_cast<int64_t>(d) - p) / r_k);
                if (a < prop_arrival_[j]) {
                    if (prop_arrival_[j] == INT32_MAX) proposed_.push_back(j);
                    prop_arrival_[j] = a;
                    prop_rhead_[j] = c.r_head;
                }
            }
        }
        waiting_[static_cast<size_t>(slot)] = waiting;
    }
    std::sort(proposed_.begin(), proposed_.end());
    for (uint32_t j : proposed_) {
        if (phase_[j] == PH_UNBURNED) ignite(j, prop_arrival_[j], prop_rhead_[j]);
        prop_arrival_[j] = INT32_MAX;
        prop_rhead_[j] = 0;
    }
}

// ---- §3 step 6: spot launches --------------------------------------------------------------

void CaModel::launch_spots(int32_t dt_s) {
    if (!g_.spot_enabled) return;
    const int64_t NX = nx_, NY = ny_;
    for (uint32_t i : snapshot_) {
        if (phase_[i] != PH_BURNING || slot_of_[i] == NO_SLOT) continue;
        FuelClass fc = cls_[fuel_[i]];
        bool source = crown_[i] != 0 ||
                      (intensity_[i] == 3 && (fc == FuelClass::SH || fc == FuelClass::SB || fc == FuelClass::TU));
        if (!source) continue;
        CellCtx c;
        compute_ctx(i, c);
        if (c.wmag < g_.spot_wind_min_cms) continue;
        // 1. launch draw
        uint64_t h = hash64(seed_, static_cast<uint64_t>(SYS_SPOT_LAUNCH), i, tick_, 0);
        int64_t thr = static_cast<int64_t>(g_.spot_rate_ppm_per_s) * dt_s;
        if (thr > 999999) thr = 999999;
        if (!g_.spot_test_mode && static_cast<int64_t>(h % 1000000) >= thr) continue;
        ++spot_launches_;
        // 2. transport
        uint64_t h1 = hash64(seed_, static_cast<uint64_t>(SYS_SPOT_TRANSPORT), i, tick_, 1);
        uint64_t h2 = hash64(seed_, static_cast<uint64_t>(SYS_SPOT_TRANSPORT), i, tick_, 2);
        int64_t span = static_cast<int64_t>(g_.spot_dist_max_m - g_.spot_dist_min_m + 1);
        int64_t d_m = g_.spot_dist_min_m + static_cast<int64_t>(h1 % static_cast<uint64_t>(span));
        d_m = (d_m * c.wmag) / g_.spot_wind_ref_cms;
        int64_t cell_m = cell_mm_ / 1000;
        int64_t dmax = static_cast<int64_t>(g_.spot_dist_max_m) * g_.spot_dist_wind_cap;
        d_m = clampi<int64_t>(d_m, cell_m, dmax);
        // Bearing frame: grid axes, 0° = +x (east), 90° = +y (SOUTH). Downwind of W = (wx, wy)
        // in the same frame, so iatan2(wy, wx) is the downwind bearing directly.
        int32_t bearing = iatan2_deg(c.wy, c.wx);
        int32_t jit = static_cast<int32_t>(h2 % static_cast<uint64_t>(2 * g_.spot_jitter_deg + 1)) - g_.spot_jitter_deg;
        bearing = (((bearing + jit) % 360) + 360) % 360;
        int64_t d_mm = d_m * 1000;
        int64_t dx = div_round(d_mm * cos_q14(bearing), static_cast<int64_t>(cell_mm_) * 16384);
        int64_t dy = div_round(d_mm * sin_q14(bearing), static_cast<int64_t>(cell_mm_) * 16384);
        int64_t tx = static_cast<int64_t>(i % nx_) + dx, ty = static_cast<int64_t>(i / nx_) + dy;
        // 3. landing time
        int32_t land = t_ + static_cast<int32_t>(d_mm / g_.spot_flight_mms);
        if (land < t_ + dt_s) land = t_ + dt_s;
        if (tx < 0 || ty < 0 || tx >= NX || ty >= NY) {
            SpotEvent ev;
            ev.src = i;
            ev.dst = i;
            ev.launch_s = t_;
            ev.land_s = land;
            ev.landed = false;
            ev.ignited = false;
            ev.stream_key = h;
            spot_events_.push_back(ev);
            continue;
        }
        PendingSpot ps;
        ps.land_s = land;
        ps.src = i;
        ps.launch_tick = tick_;
        ps.dst = static_cast<uint32_t>(ty * NX + tx);
        ps.launch_s = t_;
        ps.key = h;
        pending_.push_back(ps);
    }
}

// ---- §3 step 7: burnout -------------------------------------------------------------------

void CaModel::burnout(int32_t dt_s) {
    for (uint32_t i : snapshot_) {
        if (phase_[i] != PH_BURNING || slot_of_[i] == NO_SLOT) continue;
        // A cell goes cold once its residence has elapsed AND no unburned neighbour is still
        // accumulating progress from it (or after MAX_BURNING_S regardless). A pure residence
        // clock would let a 180 s grass cell die ~1000 s before it can push a neighbour 30 m.
        const int64_t elapsed_end = static_cast<int64_t>(t_) + dt_s;
        bool due = elapsed_end >= burnout_at_[i];
        bool waiting = waiting_[static_cast<size_t>(slot_of_[i])] != 0;
        if ((due && !waiting) || elapsed_end - arrival_[i] >= MAX_BURNING_S) {
            phase_[i] = PH_BURNED;
            release_slot(i);
            mark_dirty(i);
        }
    }
    // Cells whose fuel was removed while burning: no row, no spread, but they still go cold on
    // the schedule fixed at their ignition.
    size_t keep = 0;
    for (size_t k = 0; k < orphans_.size(); ++k) {
        uint32_t i = orphans_[k];
        if (phase_[i] != PH_BURNING) continue;  // extinguished by a delta
        if (static_cast<int64_t>(t_) + dt_s >= burnout_at_[i]) {
            phase_[i] = PH_BURNED;
            mark_dirty(i);
        } else {
            orphans_[keep++] = i;
        }
    }
    orphans_.resize(keep);
}

// ---- §3 advance ---------------------------------------------------------------------------

TickOutput CaModel::advance(int32_t dt_s, std::span<const Delta> deltas) {
    TickOutput out;
    if (dt_s <= 0) throw std::runtime_error("ember-ca: dt_s must be positive");
    if (dt_s > 600) throw std::runtime_error("ember-ca: dt_s exceeds max_dt_s (600)");
    dt_cur_ = dt_s;
    dirty_.clear();
    spot_events_.clear();

    // Active list: drop released cells, sort ascending (canonical iteration order, ADR 0008 §5).
    {
        size_t keep = 0;
        for (size_t k = 0; k < active_.size(); ++k)
            if (slot_of_[active_[k]] != NO_SLOT) active_[keep++] = active_[k];
        active_.resize(keep);
        std::sort(active_.begin(), active_.end());
        active_.erase(std::unique(active_.begin(), active_.end()), active_.end());
    }

    apply_deltas(deltas, out);      // 1
    sample_weather();               // 2
    land_spots();                   // 3
    decay();                        // 4
    // Snapshot of cells burning at the start of spread: newly ignited cells (by deltas or spot
    // landings this tick) are included per §3 ("Ignition ... allocate row"); cells ignited BY
    // spread this tick are not (they are appended to active_ after the loop).
    // Cells ignited in steps 1/3 were appended to active_ unsorted; sort so iteration is canonical.
    snapshot_.clear();
    for (uint32_t i : active_)
        if (slot_of_[i] != NO_SLOT && phase_[i] == PH_BURNING) snapshot_.push_back(i);
    std::sort(snapshot_.begin(), snapshot_.end());
    snapshot_.erase(std::unique(snapshot_.begin(), snapshot_.end()), snapshot_.end());
    spread(dt_s);                   // 5
    launch_spots(dt_s);             // 6
    burnout(dt_s);                  // 7

    std::sort(dirty_.begin(), dirty_.end());
    dirty_.erase(std::unique(dirty_.begin(), dirty_.end()), dirty_.end());
    out.dirty = dirty_;
    out.spots = spot_events_;
    int64_t active_count = 0;
    for (uint32_t i : active_) active_count += (slot_of_[i] != NO_SLOT);
    out.diag.push_back({"active_cells", active_count});
    out.diag.push_back({"ignitions", ignitions_});
    out.diag.push_back({"spot_launches", spot_launches_});
    out.diag.push_back({"spot_ignitions", spot_ignitions_});
    out.diag.push_back({"pending_spots", static_cast<int64_t>(pending_.size())});
    out.diag.push_back({"weather_step", wx_->current_step()});
    out.diag.push_back({"held_steps", wx_->held_steps()});
    out.diag.push_back({"fuel_removed_on_black", fuel_removed_on_black_});
    out.diag.push_back({"ignition_refused", ignition_refused_});
    out.diag.push_back({"unknown_fuel_cells", unknown_fuel_cells_});
    out.diag.push_back({"m10_mean", m10_base_.empty() ? 0 : m10_base_[0] + wet_acc_[0] / 3600});
    t_ += dt_s;
    ++tick_;
    return out;
}

}  // namespace

std::unique_ptr<IFireModel> make_ca_model() { return std::make_unique<CaModel>(); }

}  // namespace embersim

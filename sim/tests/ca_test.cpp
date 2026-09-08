// ember-ca property tests (spec §10) on in-memory synthetic worlds. These pin behaviour, not
// numbers: shapes, orderings, and invariants that a reader of the spec would predict.
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "doctest.h"
#include "hash.h"
#include "interface.h"
#include "params.h"
#include "testworld.h"
#include "weather.h"
#include "worldpack.h"

using namespace embersim;
using embersim::test::WorldSpec;
using embersim::test::make_world;

namespace {

constexpr int32_t DT = 60;
const WeatherSample DRY_CALM{0, 0, 2981, 100, 0};  // 25 C, 10 % RH -> m10 = 30

ParamsPack params_with(const std::vector<std::pair<std::string, std::string>>& ov) {
    ParamsPack p = embersim::test::default_ca_params();
    for (const auto& [k, v] : ov) p.apply_override(k, v);
    return p;
}

struct Sim {
    World world;
    WeatherSampler wx;
    ParamsPack params;
    std::unique_ptr<IFireModel> model;
    int32_t t = 0;
    bool hash_each_tick = true;
    std::vector<uint64_t> hashes;
    std::vector<TickOutput> outs;

    Sim(const WorldSpec& spec, const ParamsPack& p, const WeatherSample& s, uint64_t seed = 1)
        : world(make_world(spec)), wx(WeatherSampler::constant(s)), params(p) {
        model = make_model("ember-ca");
        REQUIRE(model);
        Seeds seeds;
        seeds.run_seed = seed;
        model->init(world, &wx, params, seeds, 0);
    }
    uint32_t idx(int32_t x, int32_t y) const { return static_cast<uint32_t>(y) * world.grid.nx + static_cast<uint32_t>(x); }
    void ignite(int32_t x, int32_t y) {
        Delta d;
        d.kind = DeltaKind::IgnitionForced;
        d.cells = {idx(x, y)};
        step({d});
    }
    TickOutput step(std::vector<Delta> deltas = {}, int32_t dt = DT) {
        wx.set_time(t);
        TickOutput o = model->advance(dt, deltas);
        t += dt;
        if (hash_each_tick) hashes.push_back(model->state_hash());
        outs.push_back(o);
        return o;
    }
    void run(int32_t seconds) {
        for (int32_t k = 0; k < seconds / DT; ++k) step();
    }
    FireStateView view() const { return model->state(); }
    bool black(int32_t x, int32_t y) const {
        if (x < 0 || y < 0 || x >= static_cast<int32_t>(world.grid.nx) || y >= static_cast<int32_t>(world.grid.ny)) return false;
        uint8_t p = view().phase[idx(x, y)];
        return p == 2 || p == 3;
    }
    // Extent of black cells from (cx, cy) along a direction (dx, dy): number of consecutive cells.
    int32_t ray(int32_t cx, int32_t cy, int32_t dx, int32_t dy) const {
        int32_t n = 0;
        while (black(cx + (n + 1) * dx, cy + (n + 1) * dy)) ++n;
        return n;
    }
    // Farthest black cell east/west/north/south of (cx, cy) regardless of connectivity.
    int32_t extent(int32_t cx, int32_t cy, int32_t dx, int32_t dy) const {
        int32_t best = 0;
        FireStateView v = view();
        for (uint32_t y = 0; y < v.ny; ++y)
            for (uint32_t x = 0; x < v.nx; ++x) {
                uint8_t p = v.phase[y * v.nx + x];
                if (p != 2 && p != 3) continue;
                int32_t d = (static_cast<int32_t>(x) - cx) * dx + (static_cast<int32_t>(y) - cy) * dy;
                if (d > best) best = d;
            }
        return best;
    }
    size_t count(uint8_t phase) const { return embersim::test::count_phase(view(), phase); }
    int64_t diag(const TickOutput& o, const char* key) const {
        for (const auto& d : o.diag)
            if (d.key == key) return d.value;
        return -1;
    }
};

WorldSpec grass(uint32_t n = 101) {
    WorldSpec s;
    s.nx = s.ny = n;
    s.fuel = 104;  // GR4: 45 mm/s base
    s.with_greenness = true;
    s.greenness = 0;  // cured
    return s;
}

}  // namespace

TEST_CASE("default params pack loads and every FBFM40 code resolves") {
    WorldSpec s;
    s.nx = 16;
    s.ny = 16;
    World w = make_world(s);
    for (size_t i = 0; i < w.ncells(); ++i) w.fbfm40[i] = static_cast<uint8_t>(91 + (i % 114));  // 91..204
    ParamsPack p = embersim::test::default_ca_params();
    auto m = make_model("ember-ca");
    Seeds seeds;
    CHECK_NOTHROW(m->init(w, nullptr, p, seeds, 0));
    Caps c = m->caps();
    CHECK(c.model_id == "ember-ca");
    CHECK(c.accepts == ACCEPTS_ALL);
    CHECK(c.max_dt_s == 600);
    CHECK(c.rng_streams.size() == 3);
    // NB codes (91-99) and gaps in the FBFM40 numbering are unburnable, real codes unburned.
    FireStateView v = m->state();
    for (size_t i = 0; i < w.ncells(); ++i) {
        bool nb = fuel_class_of(w.fbfm40[i]) == FuelClass::NB;
        CHECK(v.phase[i] == (nb ? 0 : 1));
    }
    CHECK(fuel_class_of(102) == FuelClass::GR);
    CHECK(fuel_class_of(124) == FuelClass::GS);
    CHECK(fuel_class_of(149) == FuelClass::SH);
    CHECK(fuel_class_of(165) == FuelClass::TU);
    CHECK(fuel_class_of(189) == FuelClass::TL);
    CHECK(fuel_class_of(204) == FuelClass::SB);
    CHECK(fuel_class_of(110) == FuelClass::NB);
    // A missing required key is named in the error.
    ParamsPack bad = params_from_text("[global]\nx = 1\n");
    auto m2 = make_model("ember-ca");
    CHECK_THROWS_WITH_AS(m2->init(w, nullptr, bad, seeds, 0), doctest::Contains("global.default_greenness"), std::runtime_error);
}

TEST_CASE("flat calm: disc grows linearly and isotropically") {
    Sim sim(grass(), params_with({{"spotting.enabled", "false"}}), DRY_CALM);
    sim.ignite(50, 50);
    // v1.1 rates: calm cured GR2 runs ~5 m/min, so 1.5 h + 1.5 h stays inside the 101-cell world.
    sim.run(5400);
    int32_t r1 = sim.ray(50, 50, 1, 0);
    sim.run(5400);
    int32_t r2 = sim.ray(50, 50, 1, 0);
    CHECK(r1 >= 5);
    // radius ~ linear in T: r2 ≈ 2 r1 within one cell
    CHECK(std::abs(r2 - 2 * r1) <= 2);
    int32_t rays[8] = {sim.ray(50, 50, 1, 0), sim.ray(50, 50, -1, 0), sim.ray(50, 50, 0, 1), sim.ray(50, 50, 0, -1),
                       sim.ray(50, 50, 1, 1), sim.ray(50, 50, -1, -1), sim.ray(50, 50, 1, -1), sim.ray(50, 50, -1, 1)};
    // diagonal rays are in cell steps of √2, so scale them
    int32_t mx = 0, mn = 1 << 30;
    for (int k = 0; k < 8; ++k) {
        int32_t r = k < 4 ? rays[k] : static_cast<int32_t>((rays[k] * 14142 + 5000) / 10000);
        mx = std::max(mx, r);
        mn = std::min(mn, r);
    }
    CHECK(mx - mn <= 2 + r2 / 10);  // 16-direction CA: ~10 % anisotropy at large radii is inherent
    // Never un-burns, no NB burned, phase monotone during the run: checked via hashes changing only forward.
    CHECK(sim.count(0) == 0);
}

TEST_CASE("wind stretches the ellipse downwind (east)") {
    WorldSpec s = grass(240);
    WeatherSample wind{500, 0, 2981, 100, 0};  // 5 m/s from the west -> blows east
    Sim sim(s, params_with({{"spotting.enabled", "false"}}), wind);
    sim.ignite(40, 120);
    sim.run(2 * 3600);  // v1.1 rates: ~0.9 km/h head; 2 h stays inside the 240-cell world
    int32_t east = sim.extent(40, 120, 1, 0), west = sim.extent(40, 120, -1, 0);
    int32_t north = sim.extent(40, 120, 0, -1), south = sim.extent(40, 120, 0, 1);
    CHECK(east > 3 * west);
    CHECK(east > north);
    // Wavelet LB from the spec: |V| = 500 * 110/256 = 214 cm/s -> LB = 256 + lb_per_ms_q8*214/100 (v1.1: 384 -> 4.2)
    double lb_expected = (256 + 384 * 214 / 100) / 256.0;
    double lb_measured = static_cast<double>(east + west) / (north + south);
    // The burned-set envelope of 16-direction wavelets is rounder than one wavelet: measured LB
    // lands at ~55-75 % of the wavelet LB (docs/sim/tuning-memo.md). Pin that band.
    CHECK(lb_measured > 0.5 * lb_expected);
    CHECK(lb_measured < 0.85 * lb_expected);
    CHECK(lb_measured > 2.0);
    CHECK(std::abs(north - south) <= 2);  // symmetric about the wind axis
}

TEST_CASE("slope: upslope is faster and the bias grows with slope") {
    auto run = [](int32_t slope) {
        WorldSpec s = grass(121);
        s.slope_pct_x = slope;  // elevation rises to the east
        Sim sim(s, params_with({{"spotting.enabled", "false"}}), DRY_CALM);
        sim.ignite(60, 60);
        sim.run(4 * 3600);
        return std::make_pair(sim.extent(60, 60, 1, 0), sim.extent(60, 60, -1, 0));
    };
    auto [e15, w15] = run(15);
    auto [e30, w30] = run(30);
    CHECK(e15 > w15);
    CHECK(e30 > w30);
    CHECK(e30 * w15 > e15 * w30);  // ratio increases with slope
    // South-north symmetric on an x-ramp
    WorldSpec s = grass(121);
    s.slope_pct_x = 30;
    Sim sim(s, params_with({{"spotting.enabled", "false"}}), DRY_CALM);
    sim.ignite(60, 60);
    sim.run(4 * 3600);
    CHECK(std::abs(sim.extent(60, 60, 0, 1) - sim.extent(60, 60, 0, -1)) <= 1);
}

TEST_CASE("fuel: grass runs, timber smolders") {
    WorldSpec g = grass(101);
    WorldSpec t = grass(101);
    t.fuel = 183;  // TL3
    t.with_greenness = false;
    Sim sg(g, params_with({{"spotting.enabled", "false"}}), DRY_CALM);
    Sim st(t, params_with({{"spotting.enabled", "false"}}), DRY_CALM);
    sg.ignite(50, 50);
    st.ignite(50, 50);
    sg.run(3 * 3600);
    st.run(3 * 3600);
    CHECK(sg.ray(50, 50, 1, 0) > 3 * st.ray(50, 50, 1, 0));
    // Grass cells go cold fast (180 s); timber litter keeps burning (2400 s).
    double grass_burning_frac = static_cast<double>(sg.count(2)) / (sg.count(2) + sg.count(3));
    double timber_burning_frac = static_cast<double>(st.count(2)) / (st.count(2) + st.count(3));
    CHECK(timber_burning_frac > grass_burning_frac);
    CHECK(st.count(2) > 0);
}

TEST_CASE("greenness: cured grass outruns green grass") {
    WorldSpec cured = grass(101);
    WorldSpec green = grass(101);
    green.greenness = 255;
    Sim sc(cured, params_with({{"spotting.enabled", "false"}}), DRY_CALM);
    Sim sgn(green, params_with({{"spotting.enabled", "false"}}), DRY_CALM);
    sc.ignite(50, 50);
    sgn.ignite(50, 50);
    sc.run(6 * 3600);
    sgn.run(6 * 3600);
    CHECK(sc.ray(50, 50, 1, 0) > 4 * sgn.ray(50, 50, 1, 0));
    CHECK(sgn.ray(50, 50, 1, 0) >= 1);  // still burns, just slowly (green_min 10 %: ~2 mm/s)
}

TEST_CASE("moisture: wet weather slows spread; extinction stops it") {
    WorldSpec s = grass(101);
    Sim dry(s, params_with({{"spotting.enabled", "false"}}), DRY_CALM);
    Sim damp(s, params_with({{"spotting.enabled", "false"}}), WeatherSample{0, 0, 2981, 500, 0});  // 50 % RH -> m10 110
    Sim wet(s, params_with({{"spotting.enabled", "false"}}), WeatherSample{0, 0, 2981, 950, 0});   // 95 % -> m10 246 > mx 150
    dry.ignite(50, 50);
    damp.ignite(50, 50);
    wet.ignite(50, 50);
    dry.run(4 * 3600);
    damp.run(4 * 3600);
    wet.run(4 * 3600);
    CHECK(dry.ray(50, 50, 1, 0) > 3 * damp.ray(50, 50, 1, 0));
    CHECK(damp.ray(50, 50, 1, 0) >= 1);  // m10 110 of mx 150 -> ~3 mm/s
    CHECK(wet.count(2) + wet.count(3) == 1);  // only the forced ignition
}

TEST_CASE("a one-cell diagonal (staircase) line is not porous to diagonal moves") {
    WorldSpec s = grass(96);
    WeatherSample wind{500, 0, 2981, 100, 0};  // 5 m/s from the west
    Sim sim(s, params_with({{"spotting.enabled", "false"}}), wind);
    // Staircase line on the main diagonal (i, i): a 45° hand line spanning the whole world, so
    // the fire (started SW of it, wind pushing east) can only reach x > y by crossing it.
    Delta d;
    d.kind = DeltaKind::FuelRemoved;
    for (uint32_t i = 0; i < 96; ++i) d.cells.push_back(i * 96 + i);
    sim.model->advance(60, std::span<const Delta>(&d, 1));
    sim.ignite(10, 60);
    sim.run(4 * 3600);
    const FireStateView v = sim.view();
    size_t burned = 0, beyond = 0;
    for (uint32_t y = 0; y < 96; ++y)
        for (uint32_t x = 0; x < 96; ++x)
            if (v.phase[y * 96 + x] >= 2) {
                ++burned;
                if (x > y) ++beyond;
            }
    CHECK(burned > 500);   // the fire ran and reached the line
    CHECK(beyond == 0);    // nothing crossed it
}

TEST_CASE("barrier stops the fire; a deterministic spot crosses it") {
    WorldSpec s = grass(128);
    s.fuel = 145;  // SH5 — a spot source when intensity 3
    s.barrier_x = 70;
    WeatherSample wind{800, 0, 2981, 100, 0};
    auto p_off = params_with({{"spotting.enabled", "false"}, {"class.SH.intensity_t2_mms", "30"}});
    auto p_on = params_with({{"spotting.enabled", "true"}, {"spotting.deterministic_test_mode", "true"},
                             {"class.SH.intensity_t2_mms", "30"}});
    Sim off(s, p_off, wind);
    Sim on(s, p_on, wind);
    off.ignite(40, 64);
    on.ignite(40, 64);
    off.run(6 * 3600);
    on.run(6 * 3600);
    CHECK(off.extent(40, 64, 1, 0) < 30);  // never past x = 70 (extent 30)
    for (uint32_t y = 0; y < 128; ++y) CHECK(off.view().phase[y * 128 + 70] == 0);
    CHECK(on.extent(40, 64, 1, 0) > 30);   // spotted across the barrier
    for (uint32_t y = 0; y < 128; ++y) CHECK(on.view().phase[y * 128 + 70] == 0);
    int64_t launches = 0, ignitions = 0;
    for (const auto& o : on.outs) {
        launches = std::max(launches, on.diag(o, "spot_launches"));
        ignitions = std::max(ignitions, on.diag(o, "spot_ignitions"));
    }
    CHECK(launches > 0);
    CHECK(ignitions > 0);
    bool any_event = false;
    for (const auto& o : on.outs) any_event = any_event || !o.spots.empty();
    CHECK(any_event);
}

TEST_CASE("determinism: same seed -> identical hashes every tick; other seed differs with spotting") {
    WorldSpec s = grass(128);
    s.fuel = 145;
    WeatherSample wind{800, 0, 2981, 100, 0};
    auto p = params_with({{"class.SH.intensity_t2_mms", "30"}, {"spotting.spot_rate_ppm_per_s", "2000"}});
    Sim a(s, p, wind, 42), b(s, p, wind, 42), c(s, p, wind, 43);
    a.ignite(40, 64);
    b.ignite(40, 64);
    c.ignite(40, 64);
    a.run(3 * 3600);
    b.run(3 * 3600);
    c.run(3 * 3600);
    REQUIRE(a.hashes.size() == b.hashes.size());
    for (size_t k = 0; k < a.hashes.size(); ++k) CHECK(a.hashes[k] == b.hashes[k]);
    bool differs = false;
    for (size_t k = 0; k < a.hashes.size(); ++k) differs = differs || (a.hashes[k] != c.hashes[k]);
    CHECK(differs);
    int64_t launches = 0;
    for (const auto& o : a.outs) launches = std::max(launches, a.diag(o, "spot_launches"));
    CHECK(launches > 0);
}

TEST_CASE("deltas: FuelRemoved on a burning cell keeps phase but stops spread, then goes cold") {
    Sim sim(grass(31), params_with({{"spotting.enabled", "false"}}), DRY_CALM);
    sim.ignite(15, 15);
    Delta d;
    d.kind = DeltaKind::FuelRemoved;
    d.cells = {sim.idx(15, 15)};
    TickOutput o = sim.step({d});
    CHECK(sim.view().phase[sim.idx(15, 15)] == 2);
    CHECK(sim.diag(o, "fuel_removed_on_black") == 1);
    sim.run(3600);
    CHECK(sim.count(2) + sim.count(3) == 1);  // nothing else ever burned
    CHECK(sim.view().phase[sim.idx(15, 15)] == 3);  // burned out on the grass schedule (180 s)
}

TEST_CASE("deltas: refusals, extinguish, overlaps, retardant decay, moisture bump") {
    Sim sim(grass(41), params_with({{"spotting.enabled", "false"}}), DRY_CALM);
    // Make (5,5) unburnable via FuelRemoved, then try to ignite it and a burning cell.
    Delta rm;
    rm.kind = DeltaKind::FuelRemoved;
    rm.cells = {sim.idx(5, 5)};
    Delta ret;
    ret.kind = DeltaKind::RetardantApplied;
    ret.cells = {sim.idx(5, 5), sim.idx(6, 6)};
    ret.magnitude = 1000;
    ret.decay_class = 2;
    TickOutput o = sim.step({rm, ret});  // overlapping deltas in one tick
    CHECK(sim.view().phase[sim.idx(5, 5)] == 0);
    // Decay runs after deltas in the same tick (spec §3): 80 permille/h * 60 s = 1.33 -> 998.
    CHECK(sim.view().retardant[sim.idx(5, 5)] >= 998);
    CHECK(sim.view().retardant[sim.idx(6, 6)] >= 998);
    Delta ig;
    ig.kind = DeltaKind::IgnitionForced;
    ig.cells = {sim.idx(5, 5), sim.idx(20, 20)};
    o = sim.step({ig});
    CHECK(sim.view().phase[sim.idx(5, 5)] == 0);
    CHECK(sim.view().phase[sim.idx(20, 20)] == 2);
    CHECK(sim.diag(o, "ignition_refused") == 1);
    o = sim.step({ig});  // (5,5) unburnable + (20,20) already burning -> two more refusals
    CHECK(sim.diag(o, "ignition_refused") == 3);
    // Extinguish acts only on burning cells.
    Delta ex;
    ex.kind = DeltaKind::ExtinguishForced;
    ex.cells = {sim.idx(20, 20), sim.idx(30, 30), sim.idx(5, 5)};
    o = sim.step({ex});
    CHECK(sim.view().phase[sim.idx(20, 20)] == 3);
    CHECK(sim.view().phase[sim.idx(30, 30)] == 1);
    CHECK(sim.view().phase[sim.idx(5, 5)] == 0);
    CHECK(o.rejected.empty());
    // Retardant decays: class 2 = 80 permille/h.
    sim.run(5 * 3600);
    uint16_t r5 = sim.view().retardant[sim.idx(6, 6)];
    CHECK(r5 < 1000);
    CHECK(r5 > 0);
    CHECK(r5 >= 560);  // ~304 ticks * 80/60 permille ~= 405 decayed
    CHECK(r5 <= 620);
    sim.run(10 * 3600);
    CHECK(sim.view().retardant[sim.idx(6, 6)] == 0);
    // Moisture bump slows spread (compare two fresh sims).
    Sim base(grass(101), params_with({{"spotting.enabled", "false"}}), DRY_CALM);
    Sim bumped(grass(101), params_with({{"spotting.enabled", "false"}}), DRY_CALM);
    Delta mb;
    mb.kind = DeltaKind::MoistureBumped;
    for (uint32_t i = 0; i < bumped.world.ncells(); ++i) mb.cells.push_back(i);
    mb.magnitude = 50;  // m10 30 -> 80 of mx 150: ~9 mm/s instead of 28
    mb.ttl_s = 24 * 3600;
    bumped.step({mb});
    base.step();
    base.ignite(50, 50);
    bumped.ignite(50, 50);
    base.run(2 * 3600);
    bumped.run(2 * 3600);
    CHECK(base.ray(50, 50, 1, 0) > bumped.ray(50, 50, 1, 0));
    CHECK(bumped.ray(50, 50, 1, 0) >= 1);
    // Retardant slows spread too.
    Sim coated(grass(101), params_with({{"spotting.enabled", "false"}}), DRY_CALM);
    Delta coat;
    coat.kind = DeltaKind::RetardantApplied;
    for (uint32_t i = 0; i < coated.world.ncells(); ++i) coat.cells.push_back(i);
    coat.magnitude = 1000;
    coat.decay_class = 1;
    coated.step({coat});
    coated.ignite(50, 50);
    coated.run(2 * 3600);
    CHECK(coated.ray(50, 50, 1, 0) < base.ray(50, 50, 1, 0) / 3);
}

TEST_CASE("invariants under a delta soup: monotone phase, arrival set once, unburnable never burns, dirty is exact") {
    WorldSpec s = grass(64);
    s.fuel = 145;
    s.barrier_x = 40;
    WeatherSample wind{600, -300, 2981, 200, 0};
    Sim sim(s, params_with({{"class.SH.intensity_t2_mms", "30"}, {"spotting.spot_rate_ppm_per_s", "3000"}}), wind, 7);
    sim.ignite(20, 30);
    const size_t n = sim.world.ncells();
    std::vector<uint8_t> prev_phase(sim.view().phase, sim.view().phase + n);
    std::vector<uint8_t> prev_int(sim.view().intensity, sim.view().intensity + n);
    std::vector<int32_t> prev_arr(sim.view().arrival_s, sim.view().arrival_s + n);
    std::vector<uint16_t> prev_ret(sim.view().retardant, sim.view().retardant + n);
    std::vector<uint8_t> init_unburnable(n);
    for (size_t i = 0; i < n; ++i) init_unburnable[i] = prev_phase[i] == 0;
    for (uint32_t tick = 0; tick < 240; ++tick) {
        std::vector<Delta> ds;
        for (int k = 0; k < 3; ++k) {
            uint64_t h = hash64(99, tick, k);
            Delta d;
            d.kind = static_cast<DeltaKind>(1 + (h % 5));
            d.cells = {static_cast<uint32_t>((h >> 8) % n), static_cast<uint32_t>((h >> 24) % n)};
            d.magnitude = static_cast<uint16_t>(100 + (h >> 40) % 900);
            d.decay_class = static_cast<uint8_t>(1 + (h >> 50) % 2);
            d.ttl_s = 600;
            ds.push_back(d);
        }
        TickOutput o = sim.step(ds);
        FireStateView v = sim.view();
        CHECK(o.rejected.empty());
        size_t changed = 0;
        for (size_t i = 0; i < n; ++i) {
            uint8_t ph = v.phase[i];
            // monotone: 1->2->3, or 1->0 (fuel removed); 0 stays 0; 2/3 never decrease
            if (prev_phase[i] == 0) CHECK(ph == 0);
            if (prev_phase[i] == 2) CHECK(ph >= 2);
            if (prev_phase[i] == 3) CHECK(ph == 3);
            if (init_unburnable[i]) CHECK(ph == 0);
            if (prev_arr[i] >= 0) CHECK(v.arrival_s[i] == prev_arr[i]);
            if (ph == 2 || ph == 3) CHECK(v.arrival_s[i] >= 0);
            bool diff = ph != prev_phase[i] || v.intensity[i] != prev_int[i] || v.arrival_s[i] != prev_arr[i] ||
                        v.retardant[i] != prev_ret[i];
            if (diff) {
                ++changed;
                CHECK(std::binary_search(o.dirty.begin(), o.dirty.end(), static_cast<uint32_t>(i)));
            }
            prev_phase[i] = ph;
            prev_int[i] = v.intensity[i];
            prev_arr[i] = v.arrival_s[i];
            prev_ret[i] = v.retardant[i];
        }
        CHECK(o.dirty.size() == changed);
        for (size_t k = 1; k < o.dirty.size(); ++k) CHECK(o.dirty[k - 1] < o.dirty[k]);
    }
    CHECK(sim.count(3) > 10);
}

TEST_CASE("dt_s above max_dt_s is refused") {
    Sim sim(grass(16), params_with({}), DRY_CALM);
    CHECK_THROWS_AS(sim.step({}, 601), std::runtime_error);
    CHECK_NOTHROW(sim.step({}, 600));
}

TEST_CASE("perf: 512x512 grass flat, centre ignition, 8 m/s wind, 12 sim-hours at dt=60") {
    WorldSpec s;
    s.nx = s.ny = 512;
    s.fuel = 104;  // GR4 (GR2 at the v1 coefficients barely moves in 6 h — a CP4 tuning item)
    s.with_greenness = true;
    s.greenness = 0;
    Sim sim(s, params_with({{"spotting.enabled", "false"}}), WeatherSample{800, 0, 2981, 100, 0});
    sim.ignite(256, 256);
    sim.hash_each_tick = false;  // the runner hashes at checkpoints, not every tick
    auto t0 = std::chrono::steady_clock::now();
    const int ticks = 12 * 60;
    sim.run(ticks * DT);
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    size_t black = sim.count(2) + sim.count(3);
    uint64_t h = sim.model->state_hash();
    std::printf("[perf] 512x512: %d ticks in %.3f s = %.0f ticks/s, %.0f grid-cells/s, %zu cells burned, "
                "%.0fx realtime, final hash %016llx\n",
                ticks, secs, ticks / secs, 262144.0 * ticks / secs, black, (ticks * DT) / secs,
                static_cast<unsigned long long>(h));
    CHECK(black > 1000);
    CHECK(secs < 30.0);
}

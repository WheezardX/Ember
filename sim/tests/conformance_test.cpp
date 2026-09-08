// Interface conformance suite (ADR 0008 §6): runs against EVERY registered model whose
// factory is available in this build. Passing with both the playback driver and the CA is the
// proof the interface is not secretly shaped around either.
#include <cstdlib>
#include <string>
#include <vector>

#include "doctest.h"
#include "interface.h"
#include "params.h"
#include "session.h"
#include "testworld.h"
#include "weather.h"
#include "worldpack.h"

using namespace embersim;
using namespace embersim::test;

namespace {

// 32x32 flat GR2 world with a synthetic radial arrival raster (so playback has something to
// replay) and no structures.
World conformance_world() {
    WorldSpec spec;
    spec.nx = 32;
    spec.ny = 32;
    spec.fuel = 102;
    World w = make_world(spec);
    w.arrival_s.assign(w.ncells(), -1);
    w.confidence.assign(w.ncells(), 0);
    for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 32; ++x) {
            int64_t dx = static_cast<int64_t>(x) - 16, dy = static_cast<int64_t>(y) - 16;
            int64_t d2 = dx * dx + dy * dy;
            if (d2 <= 100) {
                w.arrival_s[y * 32 + x] = static_cast<int32_t>(d2 * 60);
                w.confidence[y * 32 + x] = d2 == 0 ? 1 : 2;
            }
        }
    return w;
}

struct Rig {
    World world = conformance_world();
    ParamsPack params = default_ca_params();
    WeatherSampler weather = WeatherSampler::constant(WeatherSample{200, 0, 2981, 250, 0});
    std::unique_ptr<IFireModel> model;
    Caps caps;
    explicit Rig(const std::string& id, uint64_t seed = 7) {
        model = make_model(id);
        REQUIRE(model);
        caps = model->caps();
        Seeds s;
        s.run_seed = seed;
        model->init(world, &weather, params, s, 0);
        weather.set_time(0);
    }
    Delta delta(DeltaKind k, std::vector<uint32_t> cells) {
        Delta d;
        d.kind = k;
        d.cells = std::move(cells);
        d.magnitude = 500;
        d.decay_class = 1;
        d.ttl_s = 600;
        d.cause = IgnitionCause::Scenario;
        return d;
    }
    TickOutput tick(int32_t dt, std::vector<Delta> deltas = {}) {
        weather.set_time(model->now_s());
        return model->advance(dt, deltas);
    }
};

std::vector<std::string> available_models() {
    std::vector<std::string> out;
    for (const auto& id : model_ids())
        if (make_model(id)) out.push_back(id);
    return out;
}

struct Snapshot {
    std::vector<uint8_t> phase, intensity;
    std::vector<int32_t> arrival;
    explicit Snapshot(const FireStateView& v)
        : phase(v.phase, v.phase + v.ncells()), intensity(v.intensity, v.intensity + v.ncells()),
          arrival(v.arrival_s, v.arrival_s + v.ncells()) {}
};

}  // namespace

TEST_CASE("conformance: at least the null and playback models are registered") {
    auto ids = available_models();
    CHECK(std::find(ids.begin(), ids.end(), "null") != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), "arrival-playback") != ids.end());
}

TEST_CASE("conformance: capability honesty — rejected deltas are exactly the undeclared kinds") {
    for (const auto& id : available_models()) {
        CAPTURE(id);
        Rig r(id);
        std::vector<Delta> deltas;
        for (uint8_t k = 1; k <= 5; ++k) deltas.push_back(r.delta(static_cast<DeltaKind>(k), {5 * 32 + 5, 5 * 32 + 6, 5 * 32 + 7}));
        TickOutput out = r.tick(60, deltas);
        for (uint8_t k = 1; k <= 5; ++k) {
            DeltaKind kind = static_cast<DeltaKind>(k);
            bool declared = r.caps.accepts_kind(kind);
            bool reported = false;
            for (const RejectedDelta& rj : out.rejected)
                if (rj.kind == kind) {
                    reported = true;
                    CHECK(rj.count == 3);
                }
            CHECK(reported == !declared);
        }
        CHECK(r.caps.interface_version == std::string(INTERFACE_VERSION));
        CHECK_FALSE(r.caps.model_id.empty());
        CHECK_FALSE(r.caps.model_version.empty());
    }
}

TEST_CASE("conformance: phases are monotone, arrival is set exactly once, unburnable stays put") {
    for (const auto& id : available_models()) {
        CAPTURE(id);
        Rig r(id);
        r.world.fbfm40[0] = 99;  // does not matter after init; the model already read fuel
        Snapshot prev(r.model->state());
        for (int step = 0; step < 40; ++step) {
            std::vector<Delta> deltas;
            if (step == 0) deltas.push_back(r.delta(DeltaKind::IgnitionForced, {16 * 32 + 16}));
            if (step == 10) deltas.push_back(r.delta(DeltaKind::ExtinguishForced, {16 * 32 + 16}));
            r.tick(60, deltas);
            Snapshot cur(r.model->state());
            for (size_t i = 0; i < cur.phase.size(); ++i) {
                CHECK(cur.phase[i] >= prev.phase[i]);
                if (prev.phase[i] == 0) CHECK(cur.phase[i] == 0);
                if (prev.arrival[i] >= 0) CHECK(cur.arrival[i] == prev.arrival[i]);
                if (cur.phase[i] >= 2) CHECK(cur.arrival[i] >= 0);
                if (cur.phase[i] <= 1) CHECK(cur.arrival[i] == -1);
                if (prev.phase[i] <= 1 && cur.phase[i] >= 2) CHECK(cur.arrival[i] <= r.model->now_s());
            }
            prev = cur;
        }
    }
}

TEST_CASE("conformance: dirty region is exactly the set of changed cells") {
    for (const auto& id : available_models()) {
        CAPTURE(id);
        Rig r(id);
        Snapshot prev(r.model->state());
        for (int step = 0; step < 30; ++step) {
            std::vector<Delta> deltas;
            if (step == 0) deltas.push_back(r.delta(DeltaKind::IgnitionForced, {16 * 32 + 16, 3 * 32 + 3}));
            if (step == 5) deltas.push_back(r.delta(DeltaKind::FuelRemoved, {10 * 32 + 10, 10 * 32 + 11}));
            TickOutput out = r.tick(60, deltas);
            Snapshot cur(r.model->state());
            std::vector<uint32_t> changed;
            for (size_t i = 0; i < cur.phase.size(); ++i)
                if (cur.phase[i] != prev.phase[i] || cur.intensity[i] != prev.intensity[i] || cur.arrival[i] != prev.arrival[i])
                    changed.push_back(static_cast<uint32_t>(i));
            // dirty must be sorted, unique, and a superset of changed; every dirty cell that did
            // not change state must at least be an accepted-delta target (models may report
            // touched cells). We require exact equality on state-changing cells.
            std::vector<uint32_t> dirty = out.dirty;
            CHECK(std::is_sorted(dirty.begin(), dirty.end()));
            CHECK(std::adjacent_find(dirty.begin(), dirty.end()) == dirty.end());
            for (uint32_t c : changed) CHECK(std::binary_search(dirty.begin(), dirty.end(), c));
            for (uint32_t c : dirty) {
                bool in_changed = std::binary_search(changed.begin(), changed.end(), c);
                bool delta_target = false;
                for (const Delta& d : deltas)
                    for (uint32_t dc : d.cells)
                        if (dc == c) delta_target = true;
                CHECK((in_changed || delta_target));
            }
            prev = cur;
        }
    }
}

TEST_CASE("conformance: determinism under fixed seeds") {
    for (const auto& id : available_models()) {
        CAPTURE(id);
        Rig a(id, 42), b(id, 42);
        CHECK(a.model->state_hash() == b.model->state_hash());
        for (int step = 0; step < 30; ++step) {
            std::vector<Delta> deltas;
            if (step == 0) deltas.push_back(a.delta(DeltaKind::IgnitionForced, {16 * 32 + 16}));
            a.tick(60, deltas);
            b.tick(60, deltas);
            CHECK(a.model->state_hash() == b.model->state_hash());
            CHECK(a.model->now_s() == b.model->now_s());
        }
        CHECK(a.caps.deterministic);
    }
}

TEST_CASE("conformance: rewind round-trip when declared") {
    for (const auto& id : available_models()) {
        CAPTURE(id);
        Rig r(id);
        if (!r.caps.supports_rewind) {
            CHECK_FALSE(r.model->rewind(0));
            continue;
        }
        for (int i = 0; i < 10; ++i) r.tick(60);
        uint64_t h10 = r.model->state_hash();
        int32_t t10 = r.model->now_s();
        for (int i = 0; i < 5; ++i) r.tick(60);
        CHECK(r.model->state_hash() != h10);
        REQUIRE(r.model->rewind(t10));
        CHECK(r.model->now_s() == t10);
        CHECK(r.model->state_hash() == h10);
        REQUIRE(r.model->rewind(t10 / 2));
        while (r.model->now_s() < t10) r.tick(60);
        CHECK(r.model->state_hash() == h10);
    }
}

TEST_CASE("conformance: state_hash is the canonical FNV over phase/intensity/arrival") {
    for (const auto& id : available_models()) {
        Rig r(id);
        CHECK(r.model->state_hash() == hash_state(r.model->state()));
    }
}

TEST_CASE("conformance: max_dt_s is enforced by the runner") {
    Caps c;
    c.model_id = "fake";
    c.max_dt_s = 60;
    CHECK_NOTHROW(check_dt(c, 60));
    CHECK_THROWS_WITH_AS(check_dt(c, 61), doctest::Contains("max_dt_s"), std::runtime_error);
    c.max_dt_s = 0;
    CHECK_NOTHROW(check_dt(c, 1 << 20));
    for (const auto& id : available_models()) {
        Rig r(id);
        if (r.caps.max_dt_s > 0) CHECK_THROWS(check_dt(r.caps, r.caps.max_dt_s + 1));
    }
}

TEST_CASE("playback: replays the arrival raster and exposes confidence as diagnostics") {
    Rig r("arrival-playback");
    FireStateView v = r.model->state();
    CHECK(v.phase[16 * 32 + 16] == 2);  // arrival 0 burns at t=0
    CHECK(v.arrival_s[16 * 32 + 16] == 0);
    CHECK(v.phase[16 * 32 + 17] == 1);  // arrival 60 not yet
    TickOutput out = r.tick(60);
    v = r.model->state();
    CHECK(v.phase[16 * 32 + 17] == 2);
    CHECK(v.phase[17 * 32 + 16] == 2);
    CHECK(v.intensity[17 * 32 + 16] == 1);
    int64_t c1 = -1, c2 = -1;
    for (const Diag& d : out.diag) {
        if (d.key == "confidence_class_1") c1 = d.value;
        if (d.key == "confidence_class_2") c2 = d.value;
    }
    CHECK(c1 == 1);
    CHECK(c2 == 4);
    // residence: with dt 3600 the origin cell should be burned (arrival 0 + 3600 <= 3660)
    r.tick(3600);
    v = r.model->state();
    CHECK(v.phase[16 * 32 + 16] == 3);
}

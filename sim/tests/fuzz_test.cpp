// H1 — property / fuzz suite. Random worlds, random delta soups, random command scripts,
// checked against the invariants ADR 0008 promises: unburnable stays unburnable (except via
// FuelRemoved, which only makes cells unburnable), phase never goes backwards, arrival is set
// exactly once, dirty lists are exact, observers are read-only, and everything is bit-repeatable.
// Every "random" number comes from hash64 so a failure is reproducible from its seed.
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "hash.h"
#include "interface.h"
#include "observers.h"
#include "params.h"
#include "suppression.h"
#include "testworld.h"
#include "weather.h"
#include "worldpack.h"

using namespace embersim;
using embersim::test::WorldSpec;
using embersim::test::make_world;

namespace {

struct Rng {
    uint64_t seed;
    uint64_t n = 0;
    uint64_t next() { return hash64(seed, SYS_RUNNER, 0xF0ull, n++); }
    uint32_t below(uint32_t m) { return static_cast<uint32_t>(next() % m); }
};

World random_world(uint64_t seed, uint32_t n) {
    WorldSpec s;
    s.nx = s.ny = n;
    World w = make_world(s);
    Rng r{seed ^ 0xABCDull};
    static const uint8_t fuels[] = {99, 91, 101, 102, 104, 122, 145, 161, 165, 183, 189, 204, 0};
    for (size_t i = 0; i < w.ncells(); ++i) {
        w.fbfm40[i] = fuels[r.below(13)];
        w.elevation_cm[i] = 100000 + static_cast<int32_t>(r.below(20000));
        if (r.below(50) == 0) w.elevation_cm[i] = ELEV_NODATA;
        w.cc_pct[i] = static_cast<uint8_t>(r.below(90));
        w.cbh_dm[i] = static_cast<uint16_t>(r.below(60));
        w.cbd_gm3[i] = static_cast<uint16_t>(r.below(300));
    }
    return w;
}

Delta random_delta(Rng& r, uint32_t ncells) {
    Delta d;
    d.kind = static_cast<DeltaKind>(1 + r.below(5));
    uint32_t k = 1 + r.below(6);
    for (uint32_t i = 0; i < k; ++i) d.cells.push_back(r.below(ncells));
    d.magnitude = static_cast<uint16_t>(r.below(1001));
    d.decay_class = static_cast<uint8_t>(1 + r.below(2));
    d.ttl_s = static_cast<int32_t>(60 + r.below(7200));
    d.cause = static_cast<IgnitionCause>(r.below(5));
    return d;
}

struct Snapshot {
    std::vector<uint8_t> phase, intensity;
    std::vector<int32_t> arrival;
    explicit Snapshot(const FireStateView& v)
        : phase(v.phase, v.phase + v.ncells()), intensity(v.intensity, v.intensity + v.ncells()),
          arrival(v.arrival_s, v.arrival_s + v.ncells()) {}
};

// Runs a delta soup against a model; returns the per-tick hashes. Checks invariants on the way.
std::vector<uint64_t> soup(const std::string& model_id, const World& w, uint64_t seed, int ticks, bool check) {
    auto m = make_model(model_id);
    REQUIRE(m);
    ParamsPack p = embersim::test::default_ca_params();
    p.apply_override("spotting.enabled", "true");
    WeatherSampler wx = WeatherSampler::constant({300, -200, 3011, 150, 0});
    Seeds seeds{seed};
    m->init(w, &wx, p, seeds, 0);
    Rng r{seed};
    std::vector<uint64_t> hashes;
    Snapshot before(m->state());
    const Caps caps = m->caps();
    for (int t = 0; t < ticks; ++t) {
        std::vector<Delta> deltas;
        uint32_t nd = r.below(4);
        for (uint32_t i = 0; i < nd; ++i) deltas.push_back(random_delta(r, static_cast<uint32_t>(w.ncells())));
        wx.set_time(t * 60);
        TickOutput out = m->advance(60, deltas);
        FireStateView v = m->state();
        Snapshot after(v);
        if (check) {
            // dirty is sorted, unique, and covers every visible change
            CHECK(std::is_sorted(out.dirty.begin(), out.dirty.end()));
            CHECK(std::adjacent_find(out.dirty.begin(), out.dirty.end()) == out.dirty.end());
            for (size_t i = 0; i < w.ncells(); ++i) {
                uint8_t pb = before.phase[i], pa = after.phase[i];
                // monotone: 1 -> 2 -> 3; 0 stays 0; 1 -> 0 only via FuelRemoved
                if (pb == 0) CHECK(pa == 0);
                if (pb == 2) CHECK((pa == 2 || pa == 3));
                if (pb == 3) CHECK(pa == 3);
                if (pb == 1 && pa == 0) {
                    bool removed = false;
                    for (const Delta& d : deltas)
                        if (d.kind == DeltaKind::FuelRemoved)
                            for (uint32_t c : d.cells) removed |= (c == i);
                    CHECK(removed);
                }
                if (before.arrival[i] >= 0) CHECK(after.arrival[i] == before.arrival[i]);
                bool changed = pb != pa || before.intensity[i] != after.intensity[i] || before.arrival[i] != after.arrival[i];
                if (changed) CHECK(std::binary_search(out.dirty.begin(), out.dirty.end(), static_cast<uint32_t>(i)));
            }
            // rejected kinds are exactly the undeclared ones
            for (const auto& rj : out.rejected) CHECK(!caps.accepts_kind(rj.kind));
        }
        hashes.push_back(m->state_hash());
        before = std::move(after);
    }
    return hashes;
}

}  // namespace

TEST_CASE("fuzz: ember-ca delta soup holds the invariants and repeats bit-exactly") {
    for (uint64_t seed = 1; seed <= 6; ++seed) {
        World w = random_world(seed, 40);
        auto h1 = soup("ember-ca", w, seed, 90, true);
        auto h2 = soup("ember-ca", w, seed, 90, false);
        CHECK(h1 == h2);
    }
}

TEST_CASE("fuzz: null model delta soup (the reference implementor) holds the invariants") {
    for (uint64_t seed = 11; seed <= 13; ++seed) {
        World w = random_world(seed, 24);
        auto h = soup("null", w, seed, 40, true);
        CHECK(!h.empty());
    }
}

TEST_CASE("fuzz: observers are read-only and repeatable") {
    World w = random_world(77, 40);
    w.structures.assign(w.ncells(), 0);
    for (size_t i = 0; i < w.ncells(); i += 7) w.structures[i] = 1;
    auto m = make_model("ember-ca");
    ParamsPack p = embersim::test::default_ca_params();
    WeatherSampler wx = WeatherSampler::constant({300, 0, 3011, 150, 0});
    m->init(w, &wx, p, Seeds{5}, 0);
    Delta ig;
    ig.kind = DeltaKind::IgnitionForced;
    for (uint32_t i = 0; i < w.ncells(); i += 97) ig.cells.push_back(i);
    for (int t = 0; t < 60; ++t) {
        wx.set_time(t * 60);
        std::vector<Delta> ds;
        if (t == 0) ds.push_back(ig);
        m->advance(60, ds);
        uint64_t before = m->state_hash();
        Metrics a = compute_metrics(m->state(), w, 500);
        Metrics b = compute_metrics(m->state(), w, 500);
        CHECK(m->state_hash() == before);
        CHECK(a.containment_permyriad == b.containment_permyriad);
        CHECK(a.burned == b.burned);
        CHECK(a.structures_lost == b.structures_lost);
        CHECK(a.containment_permyriad >= 0);
        CHECK(a.containment_permyriad <= 10000);
        CHECK(a.structures_lost >= 0);  // layer present
    }
}

TEST_CASE("fuzz: random command scripts validate, emit on-grid canonical deltas, and repeat") {
    ParamsPack prod = embersim::test::default_production_pack();
    static const char* types[] = {"hand_t1", "hand_t2", "dozer_t1", "dozer_t2", "dozer_t3",
                                  "airtanker_large", "airtanker_seat", "helicopter_bucket"};
    for (uint64_t seed = 100; seed < 106; ++seed) {
        World w = random_world(seed, 48);
        Rng r{seed};
        std::vector<ResourceSpec> res;
        for (int i = 0; i < 5; ++i) res.push_back({"r" + std::to_string(i), types[r.below(8)]});
        std::vector<Command> cmds;
        for (int i = 0; i < 8; ++i) {
            const ResourceSpec& rs = res[r.below(5)];
            Command c;
            c.t_s = static_cast<int32_t>(r.below(3600));
            c.resource_id = rs.id;
            c.order = static_cast<uint32_t>(i);
            bool air = rs.type.rfind("airtanker", 0) == 0 || rs.type.rfind("helicopter", 0) == 0;
            bool dozer = rs.type.rfind("dozer", 0) == 0;
            auto pt = [&] { return std::make_pair(static_cast<int32_t>(r.below(48)), static_cast<int32_t>(r.below(48))); };
            if (air) {
                c.kind = "air_drop";
                c.agent = rs.type.rfind("helicopter", 0) == 0 ? "water" : "retardant";
                c.volume_class = 1 + static_cast<int>(r.below(3));
                c.points = {pt(), pt()};
            } else if (r.below(3) == 0 && !dozer) {
                c.kind = "mop_up";
                c.points = {pt(), pt(), pt(), pt()};
                c.depth_m = 30 + static_cast<int32_t>(r.below(60));
            } else {
                c.kind = "cut_line";
                c.method = dozer ? "dozer" : "hand";
                c.points = {pt(), pt(), pt()};
            }
            cmds.push_back(c);
        }
        std::stable_sort(cmds.begin(), cmds.end(), [](const Command& a, const Command& b) { return a.t_s < b.t_s; });
        auto run_once = [&] {
            SuppressionSim s;
            s.init(w, prod, res, cmds, seed);
            auto m = make_model("null");
            ParamsPack none;
            m->init(w, nullptr, none, Seeds{seed}, 0);
            std::vector<uint8_t> kinds;
            int64_t cost = 0;
            for (int t = 0; t < 240; ++t) {
                std::vector<Delta> ds = s.tick(t * 60, 60, m->state());
                for (const Delta& d : ds) {
                    for (uint32_t c : d.cells) CHECK(c < w.ncells());
                    kinds.push_back(static_cast<uint8_t>(d.kind));
                }
                m->advance(60, ds);
                CHECK(s.cost_cents() >= cost);
                cost = s.cost_cents();
            }
            return std::make_pair(kinds, cost);
        };
        auto a = run_once();
        auto b = run_once();
        CHECK(a.first == b.first);
        CHECK(a.second == b.second);
    }
}

TEST_CASE("soak: 200x200 mixed world, 4 h with spotting, deterministic and bounded") {
    World w = random_world(4242, 200);
    auto m = make_model("ember-ca");
    ParamsPack p = embersim::test::default_ca_params();
    WeatherSampler wx = WeatherSampler::constant({600, 200, 3031, 120, 0});
    m->init(w, &wx, p, Seeds{9}, 0);
    Delta ig;
    ig.kind = DeltaKind::IgnitionForced;
    ig.cells = {100 * 200 + 100, 50 * 200 + 50};
    uint64_t last = 0;
    for (int t = 0; t < 240; ++t) {
        wx.set_time(t * 60);
        std::vector<Delta> ds;
        if (t == 0) ds.push_back(ig);
        TickOutput out = m->advance(60, ds);
        last = m->state_hash();
        CHECK(out.dirty.size() <= w.ncells());
    }
    CHECK(last != 0);
    FireStateView v = m->state();
    size_t burned = 0;
    for (size_t i = 0; i < v.ncells(); ++i) burned += (v.phase[i] >= 2);
    CHECK(burned > 100);
}

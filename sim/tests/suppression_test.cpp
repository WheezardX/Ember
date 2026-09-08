// Suppression sim v1 tests (docs/sim/suppression.md §3–§4) on in-memory synthetic worlds.
#include <algorithm>
#include <stdexcept>

#include "doctest.h"
#include "suppression.h"
#include "testworld.h"

using namespace embersim;
using namespace embersim::test;

namespace {

struct Fixture {
    World world;
    ParamsPack pack = default_production_pack();
    std::vector<uint8_t> phase, intensity;
    std::vector<int32_t> arrival;
    std::vector<uint16_t> retardant;

    explicit Fixture(const WorldSpec& spec) : world(make_world(spec)) {
        size_t n = world.ncells();
        phase.assign(n, 1);
        intensity.assign(n, 0);
        arrival.assign(n, -1);
        retardant.assign(n, 0);
    }
    FireStateView view() const {
        FireStateView v;
        v.nx = world.grid.nx;
        v.ny = world.grid.ny;
        v.phase = phase.data();
        v.intensity = intensity.data();
        v.arrival_s = arrival.data();
        v.retardant = retardant.data();
        return v;
    }
};

Command cut(int32_t t, const char* rid, std::vector<std::pair<int32_t, int32_t>> path, const char* method, uint32_t order = 0) {
    Command c;
    c.t_s = t;
    c.kind = "cut_line";
    c.resource_id = rid;
    c.points = std::move(path);
    c.method = method;
    c.order = order;
    return c;
}

// Run ticks until the resource finishes; returns (finish time, all FuelRemoved cells in order).
struct WalkResult { int32_t finished_at = -1; std::vector<uint32_t> cells; int32_t ticks = 0; };
WalkResult walk(SuppressionSim& sim, const FireStateView& v, int32_t dt, int32_t max_t) {
    WalkResult r;
    for (int32_t t = 0; t < max_t; t += dt) {
        auto deltas = sim.tick(t, dt, v);
        ++r.ticks;
        for (auto& d : deltas)
            if (d.kind == DeltaKind::FuelRemoved) r.cells.insert(r.cells.end(), d.cells.begin(), d.cells.end());
        if (!sim.status()[0].busy && sim.pending_commands() == 0) { r.finished_at = t + dt; break; }
    }
    return r;
}

}  // namespace

TEST_CASE("hand crew: 20-cell straight line in GR2 finishes at 20*30000/95 s (17 ch/h -> 95 mm/s)") {
    Fixture f(WorldSpec{});
    SuppressionSim sim;
    sim.init(f.world, f.pack, {{"hc-1", "hand_t1"}}, {cut(0, "hc-1", {{10, 10}, {29, 10}}, "hand")}, 1);
    auto r = walk(sim, f.view(), 60, 100000);
    REQUIRE(r.cells.size() == 20);
    CHECK(r.cells.front() == f.world.grid.index(10, 10));
    CHECK(r.cells.back() == f.world.grid.index(29, 10));
    // 6316 s expected: finished on the tick ending at 6360 (t in [6300, 6360)).
    CHECK(r.finished_at >= 6316);
    CHECK(r.finished_at < 6316 + 60);
    CHECK(sim.status()[0].completed == 1);
    // Cost: busy_s ~ 6316 * 2200 USD/h -> ~ 386,000 cents.
    int64_t expected = 6316LL * 2200 * 100 / 3600;
    CHECK(sim.cost_cents() >= expected - 2200 * 100 / 60);
    CHECK(sim.cost_cents() <= expected + 2200 * 100 / 60);
}

TEST_CASE("diagonal path steps cost 1.414x") {
    Fixture f(WorldSpec{});
    SuppressionSim sim;
    sim.init(f.world, f.pack, {{"hc-1", "hand_t1"}}, {cut(0, "hc-1", {{10, 10}, {29, 29}}, "hand")}, 1);
    auto r = walk(sim, f.view(), 60, 100000);
    REQUIRE(r.cells.size() == 20);
    // first cell axis-cost, 19 diagonal steps: (30000 + 19*42420) / 95 = 8799 s
    int32_t expected = (30000 + 19 * 42420) / 95;
    CHECK(r.finished_at >= expected);
    CHECK(r.finished_at < expected + 60);
}

TEST_CASE("dozer rates: g12 up class 1 mid 105 ch/h; stalls on an 80 % ramp") {
    // dozer_t2 in GR2 (FM1 -> g12), flat: class 1 up (flat counts as up) = (85+125)/2 = 105 ch/h -> 586 mm/s
    {
        Fixture f(WorldSpec{});
        SuppressionSim sim;
        sim.init(f.world, f.pack, {{"dz-1", "dozer_t2"}}, {cut(0, "dz-1", {{5, 5}, {14, 5}}, "dozer")}, 1);
        auto r = walk(sim, f.view(), 60, 100000);
        REQUIRE(r.cells.size() == 10);
        int32_t expected = 10 * 30000 / 586;  // 511 s
        CHECK(r.finished_at >= expected);
        CHECK(r.finished_at < expected + 60);
        CHECK_FALSE(sim.status()[0].stalled);
    }
    {
        WorldSpec s;
        s.slope_pct_x = 80;
        Fixture f(s);
        SuppressionSim sim;
        sim.init(f.world, f.pack, {{"dz-1", "dozer_t2"}}, {cut(0, "dz-1", {{5, 5}, {14, 5}}, "dozer")}, 1);
        auto v = f.view();
        sim.tick(0, 60, v);
        sim.tick(60, 60, v);
        CHECK(sim.status()[0].stalled);
        CHECK(sim.status()[0].busy);
        CHECK(sim.status()[0].completed == 0);
    }
}

TEST_CASE("non-burnable cells cost nothing to cut") {
    WorldSpec s;
    s.barrier_x = 15;  // column of fbfm40=99
    Fixture f(s);
    SuppressionSim sim;
    sim.init(f.world, f.pack, {{"hc-1", "hand_t1"}}, {cut(0, "hc-1", {{10, 10}, {20, 10}}, "hand")}, 1);
    auto r = walk(sim, f.view(), 60, 100000);
    REQUIRE(r.cells.size() == 11);
    int32_t expected = 10 * 30000 / 95;  // 10 burnable cells
    CHECK(r.finished_at >= expected);
    CHECK(r.finished_at < expected + 60);
}

TEST_CASE("air drop resolves after the sortie delay with the right footprint and load") {
    Fixture f(WorldSpec{});
    Command c;
    c.t_s = 0;
    c.kind = "air_drop";
    c.resource_id = "at-1";
    c.points = {{20, 20}, {24, 20}};
    c.agent = "retardant";
    c.volume_class = 3;  // half width 2, load 1000
    SuppressionSim sim;
    sim.init(f.world, f.pack, {{"at-1", "airtanker_large"}}, {c}, 1);
    auto v = f.view();
    bool seen = false;
    for (int32_t t = 0; t < 7200; t += 60) {
        auto deltas = sim.tick(t, 60, v);
        if (deltas.empty()) {
            CHECK(t != 1800);
            continue;
        }
        CHECK(t == 1800);  // sortie_delay_s = 1800
        REQUIRE(deltas.size() == 1);
        const Delta& d = deltas[0];
        CHECK(d.kind == DeltaKind::RetardantApplied);
        CHECK(d.magnitude == 1000);
        CHECK(d.decay_class == 1);
        CHECK(d.resource_idx == 0);
        CHECK(d.cells.size() == 9 * 5);  // 5-cell segment widened by 2 each way: 9 x 5
        CHECK(std::is_sorted(d.cells.begin(), d.cells.end()));
        seen = true;
    }
    CHECK(seen);
    // Busy through turnaround (1800 + 2700 = 4500), free after.
    CHECK_FALSE(sim.status()[0].busy);
    CHECK(sim.cost_cents() >= 15000 * 100);
}

TEST_CASE("water drop is a moisture bump with ttl") {
    Fixture f(WorldSpec{});
    Command c;
    c.t_s = 0;
    c.kind = "air_drop";
    c.resource_id = "h-1";
    c.points = {{20, 20}};
    c.agent = "water";
    c.volume_class = 1;
    SuppressionSim sim;
    sim.init(f.world, f.pack, {{"h-1", "helicopter_bucket"}}, {c}, 1);
    auto v = f.view();
    std::vector<Delta> got;
    for (int32_t t = 0; t < 1200; t += 60) {
        auto d = sim.tick(t, 60, v);
        got.insert(got.end(), d.begin(), d.end());
    }
    REQUIRE(got.size() == 1);
    CHECK(got[0].kind == DeltaKind::MoistureBumped);
    CHECK(got[0].magnitude == 60);
    CHECK(got[0].ttl_s == 1200);
    CHECK(got[0].cells.size() == 1);
    CHECK(got[0].cells[0] == f.world.grid.index(20, 20));
}

TEST_CASE("burnout emits forced ignitions progressively along the anchor path") {
    Fixture f(WorldSpec{});
    Command c;
    c.t_s = 0;
    c.kind = "burnout";
    c.resource_id = "hc-1";
    c.points = {{5, 5}, {14, 5}};
    SuppressionSim sim;
    sim.init(f.world, f.pack, {{"hc-1", "hand_t1"}}, {c}, 1);
    auto v = f.view();
    // 20 ch/h = 111 mm/s -> one cell per 270 s: after 5 ticks of 60 s, 1 cell; all 10 by ~2700 s.
    size_t total = 0;
    int32_t first_t = -1, done_t = -1;
    for (int32_t t = 0; t < 6000 && done_t < 0; t += 60) {
        auto deltas = sim.tick(t, 60, v);
        for (auto& d : deltas) {
            CHECK(d.kind == DeltaKind::IgnitionForced);
            CHECK(d.cause == IgnitionCause::Burnout);
            total += d.cells.size();
            if (first_t < 0) first_t = t;
        }
        if (total == 10) done_t = t;
    }
    CHECK(total == 10);
    CHECK(first_t >= 240);
    CHECK(first_t <= 300);
    CHECK(done_t >= 2600);
    CHECK(done_t <= 2760);
}

TEST_CASE("mop-up extinguishes only burning cells in the region's edge band, ascending, rate-limited") {
    Fixture f(WorldSpec{});
    // Everything inside [10,30]x[10,30] is burning; the band is depth 30 m (one cell) from the edge.
    for (int32_t y = 10; y <= 30; ++y)
        for (int32_t x = 10; x <= 30; ++x) f.phase[f.world.grid.index(x, y)] = 2;
    Command c;
    c.t_s = 0;
    c.kind = "mop_up";
    c.resource_id = "hc-1";
    c.points = {{10, 10}, {30, 10}, {30, 30}, {10, 30}};
    c.depth_m = 30;
    SuppressionSim sim;
    sim.init(f.world, f.pack, {{"hc-1", "hand_t1"}}, {c}, 1);
    auto v = f.view();
    // hand_t1: 12 cells/h -> 1 cell per 300 s. First 5 ticks: nothing; tick at t=240 (acc=300*12=3600)... check totals.
    std::vector<uint32_t> all;
    for (int32_t t = 0; t < 3600; t += 60) {
        auto deltas = sim.tick(t, 60, v);
        for (auto& d : deltas) {
            CHECK(d.kind == DeltaKind::ExtinguishForced);
            all.insert(all.end(), d.cells.begin(), d.cells.end());
            for (uint32_t c : d.cells) { CHECK(f.phase[c] == 2); f.phase[c] = 3; }  // the model would cool it
        }
    }
    CHECK(all.size() == 12);  // one hour at 12 cells/h
    CHECK(std::is_sorted(all.begin(), all.end()));
    for (uint32_t i : all) {
        uint32_t x = i % f.world.grid.nx, y = i / f.world.grid.nx;
        CHECK(f.phase[i] == 3);
        bool on_edge = x == 10 || x == 30 || y == 10 || y == 30;
        CHECK(on_edge);
    }
    // An unburned band cell is never targeted: mark the whole band unburned except one cell.
    Fixture g(WorldSpec{});
    g.phase[g.world.grid.index(12, 10)] = 2;
    SuppressionSim sim2;
    sim2.init(g.world, g.pack, {{"hc-1", "hand_t1"}}, {c}, 1);
    auto v2 = g.view();
    std::vector<uint32_t> all2;
    for (int32_t t = 0; t < 3600; t += 60) {
        auto deltas = sim2.tick(t, 60, v2);
        for (auto& d : deltas)
            for (uint32_t c : d.cells) { all2.push_back(c); g.phase[c] = 3; }
    }
    REQUIRE(all2.size() == 1);
    CHECK(all2[0] == g.world.grid.index(12, 10));
    CHECK(sim2.status()[0].completed == 1);  // band went cold -> task done
}

TEST_CASE("commands queue FIFO per resource; canonical delta ordering across resources") {
    Fixture f(WorldSpec{});
    std::vector<Command> cmds = {
        cut(0, "hc-2", {{40, 40}, {41, 40}}, "hand", 0),
        cut(0, "hc-1", {{10, 10}, {11, 10}}, "hand", 1),
        cut(0, "hc-1", {{20, 20}, {21, 20}}, "hand", 2),
    };
    SuppressionSim sim;
    sim.init(f.world, f.pack, {{"hc-1", "hand_t1"}, {"hc-2", "hand_t1"}}, cmds, 1);
    auto v = f.view();
    std::vector<std::pair<uint16_t, uint32_t>> order;  // (resource idx, cell)
    for (int32_t t = 0; t < 3000; t += 60) {
        auto deltas = sim.tick(t, 60, v);
        // within a tick: resources in scenario order
        uint16_t last = 0;
        for (auto& d : deltas) {
            CHECK(d.resource_idx >= last);
            last = d.resource_idx;
            for (uint32_t c : d.cells) order.push_back({d.resource_idx, c});
        }
        if (t == 0) CHECK(sim.status()[0].queued == 1);
    }
    // hc-1 cut (10,10),(11,10) before (20,20),(21,20)
    std::vector<uint32_t> hc1;
    for (auto& p : order) if (p.first == 0) hc1.push_back(p.second);
    REQUIRE(hc1.size() == 4);
    CHECK(hc1[0] == f.world.grid.index(10, 10));
    CHECK(hc1[1] == f.world.grid.index(11, 10));
    CHECK(hc1[2] == f.world.grid.index(20, 20));
    CHECK(hc1[3] == f.world.grid.index(21, 20));
    CHECK(sim.status()[0].completed == 2);
    CHECK(sim.status()[1].completed == 1);
    CHECK(sim.pending_commands() == 0);
}

TEST_CASE("validation: unknown resource, method/type mismatch, hold, off-grid") {
    Fixture f(WorldSpec{});
    SuppressionSim sim;
    CHECK_THROWS_WITH_AS(sim.init(f.world, f.pack, {{"hc-1", "hand_t1"}}, {cut(0, "nope", {{1, 1}, {2, 2}}, "hand")}, 1),
                         doctest::Contains("unknown resource id"), std::runtime_error);
    CHECK_THROWS_WITH_AS(sim.init(f.world, f.pack, {{"hc-1", "hand_t1"}}, {cut(0, "hc-1", {{1, 1}, {2, 2}}, "dozer")}, 1),
                         doctest::Contains("does not match"), std::runtime_error);
    Command h;
    h.kind = "hold";
    h.resource_id = "hc-1";
    CHECK_THROWS_WITH_AS(sim.init(f.world, f.pack, {{"hc-1", "hand_t1"}}, {h}, 1), doctest::Contains("reserved"), std::runtime_error);
    CHECK_THROWS_WITH_AS(sim.init(f.world, f.pack, {{"hc-1", "hand_t1"}}, {cut(0, "hc-1", {{1, 1}, {99, 2}}, "hand")}, 1),
                         doctest::Contains("off-grid"), std::runtime_error);
    CHECK_THROWS_AS(sim.init(f.world, f.pack, {{"x", "bulldozer"}}, {}, 1), std::runtime_error);
}

TEST_CASE("deterministic: two identical runs emit identical deltas") {
    auto run = [] {
        Fixture f(WorldSpec{});
        SuppressionSim sim;
        sim.init(f.world, f.pack, {{"hc-1", "hand_t2"}, {"dz", "dozer_t1"}},
                 {cut(0, "hc-1", {{3, 3}, {20, 9}}, "hand"), cut(120, "dz", {{30, 30}, {30, 50}}, "dozer")}, 7);
        auto v = f.view();
        std::vector<uint32_t> cells;
        for (int32_t t = 0; t < 7200; t += 60)
            for (auto& d : sim.tick(t, 60, v)) cells.insert(cells.end(), d.cells.begin(), d.cells.end());
        return std::make_pair(cells, sim.cost_cents());
    };
    auto a = run(), b = run();
    CHECK(a.first == b.first);
    CHECK(a.second == b.second);
    CHECK(a.second > 0);
}

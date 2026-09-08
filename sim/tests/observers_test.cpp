// Observer tests (docs/sim/suppression.md §5) on contrived 5x5 states.
#include "doctest.h"
#include "observers.h"
#include "testworld.h"

using namespace embersim;
using namespace embersim::test;

namespace {
struct State {
    World world;
    std::vector<uint8_t> phase, intensity;
    std::vector<int32_t> arrival;
    std::vector<uint16_t> retardant;
    explicit State(bool structures = false) {
        WorldSpec s;
        s.nx = s.ny = 5;
        s.with_structures = structures;
        world = make_world(s);
        phase.assign(25, 1);
        intensity.assign(25, 0);
        arrival.assign(25, -1);
        retardant.assign(25, 0);
    }
    void set(int x, int y, uint8_t p) { phase[y * 5 + x] = p; }
    FireStateView view(bool with_ret = true) const {
        FireStateView v;
        v.nx = v.ny = 5;
        v.phase = phase.data();
        v.intensity = intensity.data();
        v.arrival_s = arrival.data();
        v.retardant = with_ret ? retardant.data() : nullptr;
        return v;
    }
};
}  // namespace

TEST_CASE("containment 0 %: burning blob with unburned fuel all around") {
    State s;
    s.set(2, 2, 2);
    Metrics m = compute_metrics(s.view(), s.world, 500);
    CHECK(m.burning == 1);
    CHECK(m.burned == 0);
    CHECK(m.perimeter == 1);
    CHECK(m.contained_perimeter == 0);
    CHECK(m.containment_permyriad == 0);
    CHECK(m.structures_lost == -1);
    CHECK(m.structures_threatened == -1);
}

TEST_CASE("containment 100 %: all perimeter cells cold") {
    State s;
    s.set(1, 1, 3); s.set(2, 1, 3); s.set(1, 2, 3); s.set(2, 2, 2);  // interior burning cell surrounded by cold? no: (2,2) touches (3,2),(2,3) unburned
    Metrics m = compute_metrics(s.view(), s.world, 500);
    CHECK(m.perimeter == 4);
    CHECK(m.contained_perimeter == 3);
    CHECK(m.containment_permyriad == 7500);
    s.set(2, 2, 3);
    m = compute_metrics(s.view(), s.world, 500);
    CHECK(m.containment_permyriad == 10000);
    CHECK(m.burned == 4);
}

TEST_CASE("no perimeter: fully burned grid is 100 %, empty grid is 0 %") {
    State s;
    Metrics m = compute_metrics(s.view(), s.world, 500);
    CHECK(m.containment_permyriad == 0);
    for (auto& p : s.phase) p = 3;
    m = compute_metrics(s.view(), s.world, 500);
    CHECK(m.perimeter == 0);
    CHECK(m.containment_permyriad == 10000);
}

TEST_CASE("retardant-protected neighbours count as contained; null retardant means unprotected") {
    State s;
    s.set(0, 0, 2);  // corner burning cell: neighbours (1,0) and (0,1)
    s.retardant[0 * 5 + 1] = 600;
    s.retardant[1 * 5 + 0] = 600;
    Metrics m = compute_metrics(s.view(), s.world, 500);
    CHECK(m.perimeter == 1);
    CHECK(m.containment_permyriad == 10000);
    s.retardant[1 * 5 + 0] = 400;  // below threshold
    m = compute_metrics(s.view(), s.world, 500);
    CHECK(m.containment_permyriad == 0);
    s.retardant[1 * 5 + 0] = 600;
    m = compute_metrics(s.view(false), s.world, 500);
    CHECK(m.containment_permyriad == 0);
}

TEST_CASE("unburnable neighbours are not perimeter") {
    State s;
    s.set(2, 2, 2);
    s.set(1, 2, 0); s.set(3, 2, 0); s.set(2, 1, 0); s.set(2, 3, 0);
    Metrics m = compute_metrics(s.view(), s.world, 500);
    CHECK(m.perimeter == 0);
    CHECK(m.containment_permyriad == 10000);
}

TEST_CASE("structures: lost, threatened, safe") {
    State s(true);
    s.world.structures[0 * 5 + 0] = 1;  // (0,0) lost (burned)
    s.world.structures[2 * 5 + 2] = 1;  // (2,2) threatened (burning at (4,4): Chebyshev 2)
    s.world.structures[4 * 5 + 0] = 1;  // (0,4) safe
    s.set(0, 0, 3);
    s.set(4, 4, 2);
    Metrics m = compute_metrics(s.view(), s.world, 500);
    CHECK(m.structures_lost == 1);
    CHECK(m.structures_threatened == 1);
    s.set(4, 4, 3);  // cold: no longer threatening
    m = compute_metrics(s.view(), s.world, 500);
    CHECK(m.structures_threatened == 0);
    CHECK(m.structures_lost == 1);
}

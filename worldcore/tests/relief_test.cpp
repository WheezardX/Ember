#include <algorithm>
#include <cmath>
#include <array>

#include "doctest.h"
#include "emberworld/relief.h"

using namespace emberworld;

TEST_CASE("relief: >= 0, bounded by the ground's amplitude, mounds on a flat-ish floor") {
    const std::array<float, 4> litter{1, 0, 0, 0}, grass{0, 1, 0, 0};
    const ReliefParams P;
    int n = 0, flat = 0, high = 0;
    double max_l = 0, max_g = 0;
    for (double y = 0; y < 60; y += 0.5)
        for (double x = 0; x < 60; x += 0.5) {
            const double rl = micro_relief(x, y, litter), rg = micro_relief(x, y, grass);
            CHECK(rl >= 0.0);
            CHECK(rl <= P.amp_m[0] + 1e-9);
            max_l = std::max(max_l, rl);
            max_g = std::max(max_g, rg);
            ++n;
            flat += rl < 0.25 * P.amp_m[0];
            high += rl > 0.6 * P.amp_m[0];
        }
    CHECK(max_l > 0.7 * P.amp_m[0]);       // real hummocks reach most of the amplitude
    CHECK(max_g < max_l * 0.5);            // grass is smoother than the forest floor
    CHECK(flat > n / 4);                   // floor between the mounds
    CHECK(high > n / 20);                  // and mounds
    CHECK(micro_relief(12.3, 45.6, litter) == micro_relief(12.3, 45.6, litter));
    CHECK(micro_relief(3, 4, {0, 0, 0, 0}, ReliefParams{4.0, 1.1, {0, 0, 0, 0}, 0.0}) == 0.0);
}

TEST_CASE("survival field: coherent patches, sheltered draws and north slopes favoured") {
    // flat ground: the noise alone, spread over 0..1 and smooth (neighbours 5 m apart agree)
    const HeightFn flat = [](double, double, double& z) { z = 1000.0; return true; };
    int lo = 0, hi = 0, n = 0;
    double jump = 0.0;
    for (double y = 0; y < 2000; y += 25)
        for (double x = 0; x < 2000; x += 25) {
            const double u = survival_field(x, y, flat);
            CHECK(u >= 0.0);
            CHECK(u <= 1.0);
            lo += u < 0.3;
            hi += u > 0.7;
            ++n;
            jump = std::max(jump, std::abs(survival_field(x + 5.0, y, flat) - u));
        }
    CHECK(lo > n / 10);
    CHECK(hi > n / 10);
    CHECK(jump < 0.15);                      // patches, not single trees
    // a V-shaped draw along y = 0 (walls rising 0.4 m per m): the floor beats the ridge shoulders
    const HeightFn draw = [](double, double y, double& z) { z = 1000.0 + 0.4 * std::abs(y); return true; };
    double floor = 0, shoulder = 0;
    for (double x = 0; x < 3000; x += 50) {
        floor += survival_field(x, 0.0, draw);
        shoulder += survival_field(x, 300.0, draw);
    }
    CHECK(floor > shoulder + 0.1 * 60);
    // a north-facing slope (falls toward the north) vs a south-facing one, same steepness
    const HeightFn north_facing = [](double, double y, double& z) { z = 1000.0 - 0.3 * y; return true; };
    const HeightFn south_facing = [](double, double y, double& z) { z = 1000.0 + 0.3 * y; return true; };
    double sn = 0, ss = 0;
    for (double x = 0; x < 3000; x += 50) {
        sn += survival_field(x, 0.0, north_facing);
        ss += survival_field(x, 0.0, south_facing);
    }
    CHECK(sn > ss + 0.1 * 60);
    // no height data: the noise alone, never a crash
    CHECK(survival_field(10, 10, HeightFn()) == survival_field(10, 10, [](double, double, double&) { return false; }));
}

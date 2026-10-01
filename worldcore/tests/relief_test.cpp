#include <algorithm>
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

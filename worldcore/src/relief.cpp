#include "emberworld/relief.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "emberworld/scatter.h"

namespace emberworld {

namespace {

constexpr uint64_t kMound = 0x4E11EF01, kMound2 = 0x4E11EF02, kLump = 0x4E11EF03;

double vnoise(uint64_t seed, double x, double y) {
    const double fx = std::floor(x), fy = std::floor(y);
    const int64_t ix = static_cast<int64_t>(fx), iy = static_cast<int64_t>(fy);
    double tx = x - fx, ty = y - fy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    auto h = [&](int64_t a, int64_t b) {
        return scatter::u01(scatter::hash64({seed, static_cast<uint64_t>(a), static_cast<uint64_t>(b)}));
    };
    const double a = h(ix, iy) + (h(ix + 1, iy) - h(ix, iy)) * tx;
    const double b = h(ix, iy + 1) + (h(ix + 1, iy + 1) - h(ix, iy + 1)) * tx;
    return a + (b - a) * ty;
}

double smoothstep(double a, double b, double x) {
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

}  // namespace

double micro_relief(double x, double y, const std::array<float, 4>& w, const ReliefParams& p) {
    const double soil = std::max(0.0, 1.0 - (w[0] + w[1] + w[2] + w[3]));
    double amp = soil * p.soil_amp_m;
    for (int s = 0; s < 4; ++s) amp += w[s] * p.amp_m[s];
    if (amp <= 0.0) return 0.0;
    // Hummocks: the upper part of a two-octave field, so mounds rise out of flat-ish floor
    // rather than an egg-crate of equal bumps.
    const double n = (2.0 * vnoise(kMound, x / p.mound_m, y / p.mound_m) +
                      vnoise(kMound2, x / (p.mound_m * 0.45), y / (p.mound_m * 0.45))) / 3.0;
    const double mound = smoothstep(0.38, 0.8, n);
    const double lump = vnoise(kLump, x / p.lump_m, y / p.lump_m);
    return amp * (0.8 * mound + 0.2 * lump);
}

}  // namespace emberworld

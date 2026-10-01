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

double survival_field(double x, double y, const HeightFn& height) {
    constexpr uint64_t kSurv = 0x5A1F0001, kSurv2 = 0x5A1F0002;
    // patch structure: two octaves, stretched so the sum is spread like the single-octave field
    const double nraw = (2.0 * vnoise(kSurv, x / 150.0, y / 150.0) + vnoise(kSurv2, x / 50.0, y / 50.0)) / 3.0;
    const double noise = std::clamp(0.5 + (nraw - 0.5) * 1.8, 0.0, 1.0);
    double z0 = 0.0;
    if (!height || !height(x, y, z0)) return noise;
    // shelter 1: below the surroundings (topographic position against a 75 m ring; -15 m -> 1, +15 m -> 0)
    double sum = 0.0;
    int n = 0;
    for (int k = 0; k < 8; ++k) {
        const double a = k * 0.7853981633974483;
        double z = 0.0;
        if (height(x + 75.0 * std::cos(a), y + 75.0 * std::sin(a), z)) { sum += z; ++n; }
    }
    const double tpi = n ? z0 - sum / n : 0.0;
    const double s_low = std::clamp(0.5 - tpi / 30.0, 0.0, 1.0);
    // shelter 2: north-facing (the ground falls toward the north), weighted by how steep it is
    double zn = z0, zs = z0, ze = z0, zw = z0;
    height(x, y + 15.0, zn);
    height(x, y - 15.0, zs);
    height(x + 15.0, y, ze);
    height(x - 15.0, y, zw);
    const double gx = (ze - zw) / 30.0, gy = (zn - zs) / 30.0;   // rise per metre east / north
    const double slope = std::sqrt(gx * gx + gy * gy);
    const double north = slope > 1e-4 ? -gy / slope : 0.0;      // +1: descends toward the north
    const double s_north = std::clamp(0.5 + 0.5 * north * std::min(1.0, slope / 0.25), 0.0, 1.0);
    const double shelter = 0.65 * s_low + 0.35 * s_north;
    const double u = 0.55 * noise + 0.45 * shelter;
    return std::clamp(0.5 + (u - 0.5) * 1.4, 0.0, 1.0);
}

}  // namespace emberworld

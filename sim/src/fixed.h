#pragma once
// Integer helpers for the deterministic tick path (spec §0). No float anywhere in here.
#include <cstdint>
#include <cstdlib>

#include "tables.h"

namespace embersim {

// floor(sqrt(n)) for u64 by Newton iteration — platform independent.
inline uint64_t isqrt64(uint64_t n) noexcept {
    if (n < 2) return n;
    // Start from an estimate >= sqrt(n) that cannot overflow (n/2 + 1 >= sqrt(n) for n >= 2).
    uint64_t x = n / 2 + 1;
    uint64_t y = (x + n / x) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    // x = floor(sqrt(n)) except possibly off by one; correct it without overflowing
    // (sqrt(UINT64_MAX) = 4294967295, so x never legitimately exceeds 0xFFFFFFFF).
    if (x > 0xFFFFFFFFull) x = 0xFFFFFFFFull;
    while (x * x > n) --x;
    while (x < 0xFFFFFFFFull && (x + 1) * (x + 1) <= n) ++x;
    return x;
}

// Round-half-away-from-zero division for signed 64-bit (denominator > 0).
inline int64_t div_round(int64_t num, int64_t den) noexcept {
    if (num >= 0) return (num + den / 2) / den;
    return -((-num + den / 2) / den);
}

// Linear interpolation on integers with a Q16 fraction: a + round((b-a)*frac / 65536).
inline int32_t lerp_q16(int32_t a, int32_t b, int64_t frac_q16) noexcept {
    return static_cast<int32_t>(a + div_round(static_cast<int64_t>(b - a) * frac_q16, 65536));
}

// Integer atan2 in whole degrees [0, 360), y positive = counter-clockwise from +x when
// the caller uses a mathematical frame; the caller decides the frame. 1° resolution via
// the ATAN_Q8 table with linear ratio quantisation (256 steps per octant).
inline int32_t iatan2_deg(int64_t y, int64_t x) noexcept {
    if (x == 0 && y == 0) return 0;
    int64_t ax = x < 0 ? -x : x;
    int64_t ay = y < 0 ? -y : y;
    int32_t deg_q8;
    if (ay <= ax) {
        int64_t r = (ay * 256) / ax;  // 0..256
        deg_q8 = tables::ATAN_Q8[r];
    } else {
        int64_t r = (ax * 256) / ay;
        deg_q8 = 90 * 256 - tables::ATAN_Q8[r];
    }
    int32_t deg = (deg_q8 + 128) / 256;  // round to whole degrees
    if (x < 0 && y >= 0) deg = 180 - deg;
    else if (x < 0 && y < 0) deg = 180 + deg;
    else if (x >= 0 && y < 0) deg = 360 - deg;
    return ((deg % 360) + 360) % 360;
}

inline int32_t sin_q14(int32_t deg) noexcept { return tables::SIN_Q14[((deg % 360) + 360) % 360]; }
inline int32_t cos_q14(int32_t deg) noexcept { return tables::SIN_Q14[(((deg + 90) % 360) + 360) % 360]; }

template <class T>
inline T clampi(T v, T lo, T hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace embersim

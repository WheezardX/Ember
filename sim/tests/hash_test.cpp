// Golden vectors: the C++ hash/fixed-point primitives vs Terrain's Python reference
// (terrain/veg/hashing.py) and Python math. Values were computed with
//   C:\Users\xthat\miniforge3\envs\terrain\python.exe -c "from terrain.veg.hashing import ..."
// on 2026-09-07. If any of these change, golden state hashes change everywhere.
#include "doctest.h"
#include "fixed.h"
#include "hash.h"

using namespace embersim;

TEST_CASE("splitmix64 matches Terrain's Python") {
    CHECK(splitmix64(0) == 16294208416658607535ull);
    CHECK(splitmix64(1) == 10451216379200822465ull);
    CHECK(splitmix64(0xFFFFFFFFFFFFFFFFull) == 16490336266968443936ull);
}

TEST_CASE("hash64 fold matches Terrain's Python") {
    CHECK(hash64() == 0ull);
    CHECK(hash64(0) == 16294208416658607535ull);
    CHECK(hash64(1, 2, 3) == 15020427595393229491ull);
    CHECK(hash64(20260907, 3, 12345, 7, 0) == 5744833831944632264ull);
    CHECK(hash64(0xFFFFFFFFFFFFFFFFull, 5) == 3846658174030194800ull);
    // Order matters.
    CHECK(hash64(1, 2) != hash64(2, 1));
}

TEST_CASE("isqrt64 is floor(sqrt)") {
    CHECK(isqrt64(0) == 0);
    CHECK(isqrt64(1) == 1);
    CHECK(isqrt64(3) == 1);
    CHECK(isqrt64(4) == 2);
    CHECK(isqrt64(15) == 3);
    CHECK(isqrt64(16) == 4);
    CHECK(isqrt64(1000000) == 1000);
    CHECK(isqrt64(999999) == 999);
    CHECK(isqrt64(0xFFFFFFFFFFFFFFFFull) == 4294967295ull);
    // round(30000*sqrt2), round(30000*sqrt5) via the model's formula
    CHECK((isqrt64(30000ull * 30000ull * 2 * 4) + 1) / 2 == 42426);
    CHECK((isqrt64(30000ull * 30000ull * 5 * 4) + 1) / 2 == 67082);
    // Q16 unit-vector constants used by the CA (Python: round(65536*dx/sqrt(dx^2+dy^2)))
    CHECK((isqrt64(65536ull * 65536ull * 2 * 4) + 1) / 2 / 2 == 46341);
}

TEST_CASE("iatan2_deg matches Python round(degrees(atan2)) within 1 degree") {
    struct V { int64_t y, x; int32_t deg; };
    const V cases[] = {{1, 1, 45}, {3, 4, 37}, {-2, 5, 338}, {-7, -1, 262}, {100, -3, 92}, {0, -5, 180},
                       {-5, 0, 270}, {2, -9, 167}, {1, 0, 90}, {0, 1, 0}, {-1, -1, 225}, {1, -1, 135}};
    for (const V& c : cases) {
        int32_t d = iatan2_deg(c.y, c.x);
        int32_t diff = (d - c.deg + 360) % 360;
        if (diff > 180) diff = 360 - diff;
        CHECK_MESSAGE(diff <= 1, "atan2(", c.y, ",", c.x, ") = ", d, " expected ", c.deg);
    }
    CHECK(iatan2_deg(0, 0) == 0);
}

TEST_CASE("sin/cos tables and lerp_q16 / div_round") {
    CHECK(sin_q14(0) == 0);
    CHECK(sin_q14(90) == 16384);
    CHECK(sin_q14(30) == 8192);
    CHECK(cos_q14(0) == 16384);
    CHECK(cos_q14(180) == -16384);
    CHECK(sin_q14(-90) == -16384);
    CHECK(lerp_q16(0, 100, 32768) == 50);
    CHECK(lerp_q16(100, 0, 32768) == 50);
    CHECK(lerp_q16(-100, 100, 16384) == -50);
    CHECK(lerp_q16(7, 7, 12345) == 7);
    CHECK(div_round(7, 2) == 4);
    CHECK(div_round(-7, 2) == -4);
    CHECK(div_round(5, 10) == 1);
    CHECK(div_round(4, 10) == 0);
}

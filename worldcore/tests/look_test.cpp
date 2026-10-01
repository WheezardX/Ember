#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

#include "doctest.h"
#include "emberworld/look.h"
#include "emberworld/region.h"

using namespace emberworld;

namespace {

std::string look_path() { return std::string(EMBERWORLD_REPO_DIR) + "/viz/looks/terrain_default.toml"; }

// Uniform 5x5 inputs; returns centre pixel BGRA.
std::array<uint8_t, 4> centre(const TerrainLook& L, int fb, float cc, float ndvi, float slope_rise_per_px = 0) {
    LookInputs in;
    in.width = in.height = 5;
    in.pixel_m = 10.0;
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 5; ++x) in.dem.push_back(1000.0f + slope_rise_per_px * x);
    in.fbfm40.assign(25, fb);
    in.cc.assign(25, cc);
    in.ndvi.assign(25, ndvi);
    TerrainLook native = L;             // rule tests run on the native grid
    native.supersample = 1;
    native.boundary_warp_px = 0;
    native.blur_radius_px = 0;
    Albedo a = compose_albedo(native, in);
    const uint8_t* p = &a.bgra[(2 * 5 + 2) * 4];
    return {p[0], p[1], p[2], p[3]};
}

int lum(const std::array<uint8_t, 4>& p) { return p[0] + p[1] + p[2]; }

}  // namespace

TEST_CASE("look: FBFM40 classes") {
    CHECK(classify_fbfm40(98) == LandClass::Water);
    CHECK(classify_fbfm40(165) == LandClass::TimberUnderstory);
    CHECK(classify_fbfm40(142) == LandClass::Shrub);
    CHECK(classify_fbfm40(102) == LandClass::Grass);
    CHECK(classify_fbfm40(0) == LandClass::Unknown);
    CHECK(is_vegetated(LandClass::TimberLitter));
    CHECK_FALSE(is_vegetated(LandClass::Water));
}

TEST_CASE("look: sRGB round trip") {
    Rgb c;
    REQUIRE(parse_srgb_hex("#4E6B32", c));
    CHECK(linear_to_srgb8(c.r) == 0x4E);
    CHECK(linear_to_srgb8(c.g) == 0x6B);
    CHECK(linear_to_srgb8(c.b) == 0x32);
    CHECK_FALSE(parse_srgb_hex("4E6B32", c));
    CHECK_FALSE(parse_srgb_hex("#4E6B3Z", c));
}

TEST_CASE("look: the default look loads and each rule moves colour the right way") {
    auto lr = load_look(look_path());
    REQUIRE_MESSAGE(lr.ok(), lr.error);
    const TerrainLook& L = lr.look;

    // Water is its class colour exactly (no greening, darkening or rock).
    auto w = centre(L, 98, 80.0f, 0.9f, 20.0f);
    CHECK(w[2] == 0x2F);
    CHECK(w[1] == 0x4A);
    CHECK(w[0] == 0x5A);
    CHECK(w[3] == 255);

    // NDVI greens vegetated classes: more green channel relative to red.
    auto dry = centre(L, 102, 0.0f, 0.1f);
    auto lush = centre(L, 102, 0.0f, 0.9f);
    CHECK(lush[1] - lush[2] > dry[1] - dry[2]);

    // Canopy cover darkens.
    CHECK(lum(centre(L, 165, 90.0f, 0.6f)) < lum(centre(L, 165, 0.0f, 0.6f)));

    // Steep slope (20 m rise per 10 m px ~ 63 deg) becomes rock colour.
    auto rock = centre(L, 165, 50.0f, 0.8f, 20.0f);
    CHECK(rock[2] == 0xBD);   // [slope] rock = #BDB2A0 (palette vs NAIP, 2026-09-30)
    CHECK(rock[1] == 0xB2);
    CHECK(rock[0] == 0xA0);
}

TEST_CASE("look: DEM nodata stays transparent") {
    auto lr = load_look(look_path());
    REQUIRE(lr.ok());
    LookInputs in;
    in.width = in.height = 2;
    in.dem = {std::numeric_limits<float>::quiet_NaN(), 900.0f, 900.0f, 900.0f};
    in.fbfm40 = {165, 165, 165, 165};
    in.cc = {30, 30, 30, 30};
    TerrainLook native = lr.look;
    native.supersample = 1;
    native.boundary_warp_px = 0;
    native.blur_radius_px = 0;
    Albedo a = compose_albedo(native, in);
    CHECK(a.bgra[3] == 0);
    CHECK(a.bgra[7] == 255);
    Albedo fine = compose_albedo(lr.look, in);   // supersampled: nodata quadrant stays clear
    CHECK(fine.width == 2 * lr.look.supersample);
    CHECK(fine.bgra[3] == 0);
    CHECK(fine.bgra[(fine.width * fine.height - 1) * 4 + 3] == 255);
}

TEST_CASE("look: teanaway_dev tiles compose (real layers, skipped when absent)") {
    std::string dir = std::string(EMBERWORLD_REPO_DIR) + "/../Terrain/store/teanaway_dev";
    if (const char* s = std::getenv("EMBER_TERRAIN_STORE")) dir = std::string(s) + "/teanaway_dev";
    auto rr = load_region(dir);
    if (!rr) {
        MESSAGE("teanaway_dev not found - skipped");
        return;
    }
    auto lr = load_look(look_path());
    REQUIRE(lr.ok());
    const Region& R = *rr.region;
    const TileEntry* t = R.find(14, 1, 1);  // interior tile: all valid
    REQUIRE(t);
    LookInputs in;
    std::string err;
    REQUIRE_MESSAGE(load_look_inputs(R, *t, in, err), err);
    CHECK(in.width == 80);
    CHECK_FALSE(in.ndvi.empty());
    Albedo a = compose_albedo(lr.look, in);
    const size_t s = static_cast<size_t>(lr.look.supersample);
    CHECK(a.width == static_cast<int>(80 * s));
    size_t opaque = 0;
    for (size_t i = 3; i < a.bgra.size(); i += 4) opaque += a.bgra[i] == 255;
    CHECK(opaque == 80u * 80u * s * s);
}

TEST_CASE("look: mip chain is complete and nodata never darkens valid texels") {
    Albedo a;
    a.width = 80;
    a.height = 80;
    a.bgra.assign(80 * 80 * 4, 0);
    for (int y = 0; y < 80; ++y)
        for (int x = 0; x < 80; ++x) {
            uint8_t* p = &a.bgra[(y * 80 + x) * 4];
            if (x < 8) continue;               // an apron strip of nodata (alpha 0)
            p[0] = 40; p[1] = 120; p[2] = 90; p[3] = 255;
        }
    auto mips = build_mips(a);
    CHECK(mips.size() == 7);                   // 80 40 20 10 5 2 1
    CHECK(mips.back().width == 1);
    CHECK(mips.back().height == 1);
    for (const Albedo& m : mips) {
        const uint8_t* p = &m.bgra[(m.height / 2 * m.width + m.width - 1) * 4];  // a valid-side texel
        CHECK(p[1] == 120);                    // colour preserved exactly (alpha-weighted)
        CHECK(p[3] > 0);
    }
    CHECK(mips[1].bgra[3] == 0);               // fully-nodata 2x2 block stays transparent
}

TEST_CASE("look: supersampled composition is seamless across a tile edge") {
    std::string dir = std::string(EMBERWORLD_REPO_DIR) + "/../Terrain/store/teanaway_dev";
    if (const char* s = std::getenv("EMBER_TERRAIN_STORE")) dir = std::string(s) + "/teanaway_dev";
    auto rr = load_region(dir);
    if (!rr) {
        MESSAGE("teanaway_dev not found - skipped");
        return;
    }
    auto lr = load_look(look_path());
    REQUIRE(lr.ok());
    const Region& R = *rr.region;
    const TileEntry* a = R.find(14, 0, 1);
    const TileEntry* b = R.find(14, 1, 1);  // east neighbour
    LookInputs ia, ib;
    std::string err;
    REQUIRE(load_look_inputs(R, *a, ia, err));
    REQUIRE(load_look_inputs(R, *b, ib, err));
    TerrainLook L = lr.look;
    L.blur_radius_px = 0;  // blur reads neighbours inside each raster only; test the warp alone
    Albedo A = compose_albedo(L, ia), B = compose_albedo(L, ib);
    const int s = L.supersample, tp = R.tile_px, ov = R.overlap_px;
    // A's content columns [ov+tp-4, ov+tp) are B's apron columns [ov-4, ov): same world pixels.
    int compared = 0, equal = 0;
    for (int row = ov * s; row < (ov + tp) * s; ++row)
        for (int c = 0; c < 4 * s; ++c) {
            const int ca = (ov + tp - 4) * s + c, cb = (ov - 4) * s + c;
            const uint8_t* pa = &A.bgra[(static_cast<size_t>(row) * A.width + ca) * 4];
            const uint8_t* pb = &B.bgra[(static_cast<size_t>(row) * B.width + cb) * 4];
            ++compared;
            equal += pa[0] == pb[0] && pa[1] == pb[1] && pa[2] == pb[2];
        }
    CHECK(compared > 0);
    CHECK(equal == compared);
}

namespace {

// Uniform 5x5 inputs on the native grid; returns the centre pixel's ground mix (0..1).
std::array<float, 4> centre_mix(const TerrainLook& L, int fb, float cc, float slope_rise_per_px = 0) {
    LookInputs in;
    in.width = in.height = 5;
    in.pixel_m = 10.0;
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 5; ++x) in.dem.push_back(1000.0f + slope_rise_per_px * x);
    in.fbfm40.assign(25, fb);
    in.cc.assign(25, cc);
    TerrainLook native = L;
    native.supersample = 1;
    native.boundary_warp_px = 0;
    native.blur_radius_px = 0;
    Albedo a = compose_albedo(native, in);
    REQUIRE(a.mix.size() == a.bgra.size());
    const uint8_t* p = &a.mix[(2 * 5 + 2) * 4];
    return {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
}

}  // namespace

TEST_CASE("look: ground mix follows the fuel model, canopy and slope (ground plane v1)") {
    LookResult lr = load_look(look_path());
    REQUIRE_MESSAGE(lr.ok(), lr.error);
    const TerrainLook& L = lr.look;
    REQUIRE(L.has_ground);
    const auto litter = centre_mix(L, 183, 0.0f);          // TL3
    CHECK(litter[kLitter] > 0.8f);
    const auto grass = centre_mix(L, 102, 0.0f);           // GR2, open
    CHECK(grass[kGrass] > 0.85f);
    CHECK(grass[kLitter] < 0.01f);
    const auto shaded = centre_mix(L, 102, 100.0f);        // same grass under full canopy
    CHECK(shaded[kGrass] < grass[kGrass] * 0.3f);
    CHECK(shaded[kLitter] > 0.6f);
    const auto cliff = centre_mix(L, 102, 0.0f, 20.0f);    // 63 deg: fully rock
    CHECK(cliff[kRock] > 0.99f);
    const auto water = centre_mix(L, 98, 0.0f);            // no detail on water
    CHECK(water[0] + water[1] + water[2] + water[3] == 0.0f);
}

TEST_CASE("look: supersampled ground mix is aligned with the albedo, and mips are plain averages") {
    LookResult lr = load_look(look_path());
    REQUIRE(lr.ok());
    LookInputs in;
    in.width = in.height = 8;
    in.pixel_m = 10.0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            in.dem.push_back(x == 0 && y == 0 ? std::numeric_limits<float>::quiet_NaN() : 1000.0f);
            in.fbfm40.push_back(x < 4 ? 102 : 183);
            in.cc.push_back(0.0f);
        }
    Albedo a = compose_albedo(lr.look, in);
    REQUIRE(a.mix.size() == a.bgra.size());
    for (size_t k = 0; k < a.bgra.size() / 4; ++k) {
        if (a.bgra[k * 4 + 3] == 0) {   // nodata texels carry no ground either
            CHECK(a.mix[k * 4 + 0] + a.mix[k * 4 + 1] + a.mix[k * 4 + 2] + a.mix[k * 4 + 3] == 0);
        }
    }
    const auto mips = build_mix_mips(a);
    CHECK(mips.back().width == 1);
    CHECK(mips.back().height == 1);
    CHECK(mips[1].bgra.size() == static_cast<size_t>(mips[1].width) * mips[1].height * 4);
}

TEST_CASE("look: ladder-fuel weight from canopy base height") {
    CHECK(ladder_weight(0.3f, 60.0f) == 1.0f);       // crowns start low: ladder fuel
    CHECK(ladder_weight(1.5f, 60.0f) == doctest::Approx(0.5f));
    CHECK(ladder_weight(3.0f, 60.0f) == 0.0f);       // high crowns
    CHECK(ladder_weight(0.3f, 10.0f) == 0.0f);       // no canopy to climb into
    CHECK(ladder_weight(std::nanf(""), 60.0f) == 0.0f);
    CHECK(ladder_weight(-9999.0f, 60.0f) == 0.0f);
}

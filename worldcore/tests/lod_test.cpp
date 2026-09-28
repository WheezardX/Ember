#include <cmath>

#include "doctest.h"
#include "emberworld/lod.h"
#include "emberworld/region.h"
#include "testutil.h"

using namespace emberworld;

namespace {

double flat(double, double) { return 700.0; }

// Every finest tile (i.e. all the data; Terrain skips all-nodata tiles, so coarse tiles may
// cover more area than their children) lies inside exactly one selected tile, and no two
// selected tiles overlap.
void check_partition(const Region& R, const std::vector<const TileEntry*>& sel) {
    for (const TileEntry* f : R.tiles_at(R.finest_lod())) {
        int covering = 0;
        for (const TileEntry* t : sel)
            if (t->content.min_x <= f->content.min_x + 1e-6 && t->content.max_x >= f->content.max_x - 1e-6 &&
                t->content.min_y <= f->content.min_y + 1e-6 && t->content.max_y >= f->content.max_y - 1e-6)
                ++covering;
        CHECK(covering == 1);
    }
    for (size_t a = 0; a < sel.size(); ++a)
        for (size_t b = a + 1; b < sel.size(); ++b) {
            const Bounds& p = sel[a]->content;
            const Bounds& q = sel[b]->content;
            const double ox = std::min(p.max_x, q.max_x) - std::max(p.min_x, q.min_x);
            const double oy = std::min(p.max_y, q.max_y) - std::max(p.min_y, q.min_y);
            CHECK_FALSE((ox > 1e-6 && oy > 1e-6));
        }
}

}  // namespace

TEST_CASE("lod: close camera refines to the finest level, far camera stays coarse") {
    const auto root = testutil::write_synth_region("lod", flat);
    auto rr = load_region(root.string());
    REQUIRE(rr);
    const Region& R = *rr.region;  // lod 5: 2x2 tiles of 80 m; lod 4: 1 tile of 160 m

    auto near = select_tiles(R, 500040.0, 5200040.0, 710.0);
    REQUIRE(near.size() == 4);
    for (const TileEntry* t : near) CHECK(t->lod == 5);
    check_partition(R, near);

    auto far = select_tiles(R, 500080.0, 5200080.0, 700.0 + 5000.0);
    REQUIRE(far.size() == 1);
    CHECK(far[0]->lod == 4);
    check_partition(R, far);

    LodOptions clamp;
    clamp.max_lod = 4;
    CHECK(select_tiles(R, 500040.0, 5200040.0, 710.0, clamp).size() == 1);
}

TEST_CASE("lod: teanaway_dev selections partition the region at any camera") {
    std::string dir = std::string(EMBERWORLD_REPO_DIR) + "/../Terrain/store/teanaway_dev";
    if (const char* s = std::getenv("EMBER_TERRAIN_STORE")) dir = std::string(s) + "/teanaway_dev";
    auto rr = load_region(dir);
    if (!rr) {
        MESSAGE("teanaway_dev not found - skipped");
        return;
    }
    const Region& R = *rr.region;
    const Bounds e = R.extent();
    const double cx = 0.5 * (e.min_x + e.max_x), cy = 0.5 * (e.min_y + e.max_y);
    for (double h : {50.0, 400.0, 1500.0, 6000.0}) {
        auto sel = select_tiles(R, cx, cy, R.heightmap.z_max + h);
        INFO("height " << h << " -> " << sel.size() << " tiles");
        check_partition(R, sel);
    }
    // Close to the ground at the centre: all 9 finest tiles are needed only near the camera;
    // the far corner of the 1.9 km grid may stay coarser.
    auto low = select_tiles(R, cx, cy, R.heightmap.z_max + 50.0);
    int finest = 0;
    for (const TileEntry* t : low) finest += t->lod == R.finest_lod();
    CHECK(finest >= 4);
}

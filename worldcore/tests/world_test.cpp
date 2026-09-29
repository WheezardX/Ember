#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "doctest.h"
#include "emberworld/heightfield.h"
#include "emberworld/region.h"
#include "emberworld/tiff.h"
#include "testutil.h"

using namespace emberworld;
namespace fs = std::filesystem;

namespace {

// Terrain's real Teanaway dev region, when the sibling checkout exists (never in CI).
std::string teanaway_dir() {
    if (const char* s = std::getenv("EMBER_TERRAIN_STORE")) return std::string(s) + "/teanaway_dev";
    return std::string(EMBERWORLD_REPO_DIR) + "/../Terrain/store/teanaway_dev";
}
bool have_teanaway() { return fs::exists(teanaway_dir() + "/manifest.json"); }

double hill(double x, double y) {
    return 700.0 + 0.05 * (x - 500000.0) + 30.0 * std::sin((y - 5200000.0) / 25.0);
}

}  // namespace

TEST_CASE("tiff: strips, tiles, sample formats, geo tags round-trip") {
    const auto dir = testutil::temp_dir("tiff");
    std::vector<double> v(7 * 5);
    for (int i = 0; i < 35; ++i) v[i] = i * 1.5;
    v[3] = -9999;
    testutil::write_tiff(dir / "f32.tif", 7, 5, v, 3, 32, 1000.0, 2000.0, 2.5, "-9999", 2);
    testutil::write_tiff(dir / "f32t.tif", 7, 5, v, 3, 32, 1000.0, 2000.0, 2.5, "-9999", 0, 16);
    std::vector<double> u(7 * 5);
    for (int i = 0; i < 35; ++i) u[i] = 60000 + i;
    testutil::write_tiff(dir / "u16.tif", 7, 5, u, 1, 16, 0.0, 0.0, 1.0, "0", 5);

    for (const char* name : {"f32.tif", "f32t.tif"}) {
        auto r = read_tiff((dir / name).string());
        REQUIRE_MESSAGE(r, r.error.message);
        const Raster& R = *r.raster;
        CHECK(R.width == 7);
        CHECK(R.height == 5);
        CHECK(R.type == SampleType::F32);
        CHECK(R.at(6, 4) == doctest::Approx(34 * 1.5));
        CHECK(R.at(2, 1) == doctest::Approx(9 * 1.5));
        CHECK(R.is_nodata(R.at(3, 0)));
        CHECK(R.geo.has_transform);
        CHECK(R.geo.origin_x == 1000.0);
        CHECK(R.geo.origin_y == 2000.0);
        CHECK(R.geo.pixel_w == 2.5);
        CHECK(*R.geo.nodata == -9999.0);
    }
    auto r = read_tiff((dir / "u16.tif").string());
    REQUIRE_MESSAGE(r, r.error.message);
    CHECK(r.raster->type == SampleType::U16);
    CHECK(r.raster->at(4, 3) == 60000 + 25);
}

TEST_CASE("tiff: refuses what it does not support, never throws") {
    const uint8_t mm[8] = {'M', 'M', 0, 42, 0, 0, 0, 8};
    CHECK_FALSE(read_tiff_memory(mm, 8));
    const uint8_t big[8] = {'I', 'I', 43, 0, 8, 0, 0, 0};
    CHECK(read_tiff_memory(big, 8).error.message.find("BigTIFF") != std::string::npos);
    const uint8_t junk[4] = {1, 2, 3, 4};
    CHECK_FALSE(read_tiff_memory(junk, 4));
    CHECK_FALSE(read_tiff("does/not/exist.tif"));
}

TEST_CASE("region: synthetic manifest loads, backslash paths normalised, extent from finest LOD") {
    const auto root = testutil::write_synth_region("region", hill);
    auto rr = load_region(root.string());
    REQUIRE_MESSAGE(rr, rr.error);
    const Region& R = *rr.region;
    CHECK(R.tiles.size() == 5);
    CHECK(R.finest_lod() == 5);
    CHECK(R.coarsest_lod() == 4);
    CHECK(R.tiles_at(5).size() == 4);
    CHECK(R.find(5, 1, 1) != nullptr);
    CHECK(R.find(5, 1, 1)->height_tif == "tiles/z5/x1/y1/height.tif");
    const Bounds e = R.extent();
    CHECK(e.min_x == doctest::Approx(500000.0));
    CHECK(e.max_x == doctest::Approx(500160.0));
    CHECK(e.max_y == doctest::Approx(5200160.0));
}

TEST_CASE("heightfield: same-LOD neighbours share edge vertices and normals bit-exactly") {
    const auto root = testutil::write_synth_region("seams", hill);
    auto rr = load_region(root.string());
    REQUIRE(rr);
    const Region& R = *rr.region;
    const Frame fr = region_frame(R);
    MeshOptions opt;
    opt.skirts = false;
    auto m00 = load_tile_mesh(R, *R.find(5, 0, 0), fr, opt);
    auto m10 = load_tile_mesh(R, *R.find(5, 1, 0), fr, opt);  // east neighbour
    auto m01 = load_tile_mesh(R, *R.find(5, 0, 1), fr, opt);  // north neighbour
    REQUIRE_MESSAGE(m00.ok(), m00.error);
    REQUIRE(m10.ok());
    REQUIRE(m01.ok());
    const int N = m00.mesh.grid_n;
    CHECK(N == 9);
    int checked = 0;
    for (int j = 0; j < N; ++j) {
        // east edge of (0,0) == west edge of (1,0)
        const auto& a = m00.mesh.positions[j * N + (N - 1)];
        const auto& b = m10.mesh.positions[j * N + 0];
        CHECK(a.x == b.x);
        CHECK(a.y == b.y);
        CHECK(a.z == b.z);
        const auto& na = m00.mesh.normals[j * N + (N - 1)];
        const auto& nb = m10.mesh.normals[j * N + 0];
        CHECK(na.x == nb.x);
        CHECK(na.y == nb.y);
        CHECK(na.z == nb.z);
        ++checked;
    }
    for (int i = 0; i < N; ++i) {
        // north edge of (0,0) (row 0) == south edge of (0,1) (row N-1): y grows north
        const auto& a = m00.mesh.positions[0 * N + i];
        const auto& b = m01.mesh.positions[(N - 1) * N + i];
        CHECK(a.z == b.z);
        CHECK(a.x == b.x);
        CHECK(a.y == b.y);
    }
    CHECK(checked == N);
}

TEST_CASE("heightfield: frame, heights, and region-border nodata handling") {
    const auto root = testutil::write_synth_region("frame", hill);
    auto rr = load_region(root.string());
    REQUIRE(rr);
    const Region& R = *rr.region;
    const Frame fr = region_frame(R);
    CHECK(fr.anchor_x == doctest::Approx(500080.0));
    auto m = load_tile_mesh(R, *R.find(5, 0, 0), fr);
    REQUIRE(m.ok());
    const TileMesh& M = m.mesh;
    // Interior corner (4, 4) of tile (0,0): world (500040, 5200040) -> UE (-4000, +4000) cm
    const auto& p = M.positions[4 * M.grid_n + 4];
    CHECK(p.x == doctest::Approx(-4000.0f));
    CHECK(p.y == doctest::Approx(4000.0f));
    // Height: mean of the 4 pixel centres around the corner ~ the analytic value (smooth fn)
    CHECK(p.z / 100.0 == doctest::Approx(hill(500040.0, 5200040.0)).epsilon(0.01));
    // Normal points up and tilts west (height rises east): nx < 0
    CHECK(M.normals[4 * M.grid_n + 4].z > 0.9f);
    CHECK(M.normals[4 * M.grid_n + 4].x < 0.0f);
    // Region-border corners of tile (0,0) average only the valid pixels: all corners valid
    CHECK(M.nodata_corners == 0);
    CHECK(M.surface_triangles == 8 * 8 * 2);
    CHECK(M.skirt_triangles == 4 * 8 * 4);
    CHECK(M.indices.size() == static_cast<size_t>(3 * (M.surface_triangles + M.skirt_triangles)));
    // Coarse tile (lod 4) spans 160 m of which all is inside: valid, 8x8 quads of 20 m
    auto c = load_tile_mesh(R, *R.find(4, 0, 0), fr);
    REQUIRE(c.ok());
    CHECK(c.mesh.surface_triangles == 128);
}

TEST_CASE("heightfield: surface triangles use UE's front-face winding") {
    // Found at HCP0: the first render showed only skirts - the surface was back-face culled.
    const auto root = testutil::write_synth_region("winding", [](double, double) { return 500.0; });
    auto rr = load_region(root.string());
    REQUIRE(rr);
    auto m = load_tile_mesh(*rr.region, *rr.region->find(5, 0, 0), region_frame(*rr.region));
    REQUIRE(m.ok());
    const TileMesh& M = m.mesh;
    for (int t = 0; t < M.surface_triangles; ++t) {
        const Vec3f& p0 = M.positions[M.indices[3 * t]];
        const Vec3f& p1 = M.positions[M.indices[3 * t + 1]];
        const Vec3f& p2 = M.positions[M.indices[3 * t + 2]];
        const double ux = p1.x - p0.x, uy = p1.y - p0.y, vx = p2.x - p0.x, vy = p2.y - p0.y;
        const double cz = ux * vy - uy * vx;
        REQUIRE(cz < 0.0);
    }
}

TEST_CASE("heightfield: wrong raster size is an error, not a crash") {
    const auto root = testutil::write_synth_region("badsize", hill);
    auto rr = load_region(root.string());
    REQUIRE(rr);
    Raster small;
    small.width = small.height = 3;
    small.data.assign(9 * 4, 0);
    auto m = build_tile_mesh(*rr.region, rr.region->tiles[0], small, region_frame(*rr.region));
    CHECK_FALSE(m.ok());
}

TEST_CASE("teanaway_dev (real Terrain store, skipped when absent)") {
    if (!have_teanaway()) {
        MESSAGE("teanaway_dev not found at " << teanaway_dir() << " - skipped");
        return;
    }
    auto rr = load_region(teanaway_dir());
    REQUIRE_MESSAGE(rr, rr.error);
    const Region& R = *rr.region;
    CHECK(R.tile_px == 64);
    CHECK(R.overlap_px == 8);
    CHECK(R.tiles.size() == 14);
    CHECK(R.tiles_at(14).size() == 9);
    CHECK(R.layers.size() == 9);
    CHECK(R.layers.at("fuels_fbfm40").categorical);

    // Reference values read with rasterio (see worldcore/README.md).
    auto h = read_tiff(R.path(R.find(14, 0, 0)->height_tif));
    REQUIRE_MESSAGE(h, h.error.message);
    CHECK(h.raster->width == 80);
    CHECK(h.raster->at(40, 40) == doctest::Approx(725.8921));
    CHECK(h.raster->is_nodata(h.raster->at(0, 0)));
    CHECK(h.raster->geo.origin_x == doctest::Approx(656714.6820252051));
    CHECK(h.raster->geo.origin_y == doctest::Approx(5230601.004653024));
    auto fb = read_tiff(R.path(R.layers.at("fuels_fbfm40").find(14, 1, 1)->path));
    REQUIRE(fb);
    CHECK(fb.raster->type == SampleType::U16);
    CHECK(fb.raster->at(79, 79) == 188);

    // Every finest-LOD tile builds; interior tile has no invalid corners; totals are sane.
    const Frame fr = region_frame(R);
    int tris = 0;
    for (const TileEntry* t : R.tiles_at(14)) {
        auto m = load_tile_mesh(R, *t, fr);
        REQUIRE_MESSAGE(m.ok(), m.error);
        CHECK(m.mesh.min_z >= R.heightmap.z_min - 1.0);
        CHECK(m.mesh.max_z <= R.heightmap.z_max + 1.0);
        tris += m.mesh.surface_triangles;
        if (t->x == 1 && t->y == 1) CHECK(m.mesh.nodata_corners == 0);
    }
    // The AOI is 144x144 px (1.44 km @ 10 m) inside a 3x3 grid of 64 px tiles; the mesh covers
    // exactly the valid pixels: 144*144 quads.
    CHECK(tris == 144 * 144 * 2);
    // Data extent = the 1.44 km AOI anchored at the tile grid's lower-left corner.
    Bounds vb{1e300, 1e300, -1e300, -1e300};
    for (const TileEntry* t : R.tiles_at(14)) {
        auto m = load_tile_mesh(R, *t, fr);
        if (!m.mesh.has_valid) continue;
        vb.min_x = std::min(vb.min_x, m.mesh.valid_bounds.min_x);
        vb.min_y = std::min(vb.min_y, m.mesh.valid_bounds.min_y);
        vb.max_x = std::max(vb.max_x, m.mesh.valid_bounds.max_x);
        vb.max_y = std::max(vb.max_y, m.mesh.valid_bounds.max_y);
    }
    CHECK(vb.width() == doctest::Approx(1440.0));
    CHECK(vb.height() == doctest::Approx(1440.0));
    CHECK(vb.min_x == doctest::Approx(R.extent().min_x));
    CHECK(vb.min_y == doctest::Approx(R.extent().min_y));
    double z;
    const Bounds e = R.extent();
    CHECK(sample_height(R, 0.5 * (e.min_x + e.max_x), 0.5 * (e.min_y + e.max_y), z));
    CHECK(z > R.heightmap.z_min);
    CHECK(z < R.heightmap.z_max);
}

TEST_CASE("heightfield: data_extent matches the union of mesh valid bounds") {
    const auto root = testutil::write_synth_region("extent", hill);
    auto rr = load_region(root.string());
    REQUIRE(rr);
    const Region& R = *rr.region;
    Bounds want{1e300, 1e300, -1e300, -1e300};
    for (const TileEntry* t : R.tiles_at(R.finest_lod())) {
        auto m = load_tile_mesh(R, *t, region_frame(R));
        REQUIRE(m.ok());
        want.min_x = std::min(want.min_x, m.mesh.valid_bounds.min_x);
        want.min_y = std::min(want.min_y, m.mesh.valid_bounds.min_y);
        want.max_x = std::max(want.max_x, m.mesh.valid_bounds.max_x);
        want.max_y = std::max(want.max_y, m.mesh.valid_bounds.max_y);
    }
    Bounds got;
    std::string err;
    REQUIRE(data_extent(R, got, err));
    CHECK(got.min_x == want.min_x);
    CHECK(got.max_x == want.max_x);
    CHECK(got.min_y == want.min_y);
    CHECK(got.max_y == want.max_y);
}

TEST_CASE("heightfield: SurfaceSampler matches mesh vertices at corners and interpolates between") {
    const auto root = testutil::write_synth_region("sampler", hill);
    auto rr = load_region(root.string());
    REQUIRE(rr);
    const Region& R = *rr.region;
    const TileEntry& t = *R.find(5, 0, 0);
    auto tr = read_tiff(R.path(t.height_tif));
    REQUIRE(tr);
    SurfaceSampler s;
    REQUIRE(s.build(R, t, *tr.raster));
    const Frame fr = region_frame(R);
    auto m = build_tile_mesh(R, t, *tr.raster, fr);
    REQUIRE(m.ok());
    const int N = m.mesh.grid_n;
    for (int j = 0; j < N; j += 3)
        for (int i = 0; i < N; i += 3) {
            double z = 0;
            REQUIRE(s.height_at(t.content.min_x + i * 10.0, t.content.max_y - j * 10.0, z));
            CHECK(static_cast<float>((z - fr.anchor_z) * 100.0) == doctest::Approx(m.mesh.positions[j * N + i].z));
        }
    double z0 = 0, z1 = 0, zm = 0;
    REQUIRE(s.height_at(t.content.min_x + 20.0, t.content.max_y - 20.0, z0));
    REQUIRE(s.height_at(t.content.min_x + 30.0, t.content.max_y - 20.0, z1));
    REQUIRE(s.height_at(t.content.min_x + 25.0, t.content.max_y - 20.0, zm));
    CHECK(zm == doctest::Approx(0.5 * (z0 + z1)));
    CHECK_FALSE(s.height_at(t.content.min_x - 5.0, t.content.max_y - 20.0, zm));
}

TEST_CASE("region: null optional fields (Terrain writes \"mesh\": null without glTF tiles) load") {
    const auto root = testutil::write_synth_region("nullmesh", hill);
    const auto mpath = root / "manifest.json";
    std::string text;
    {
        std::ifstream f(mpath);
        text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    const std::string needle = "\"hash\":\"h\"}";
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        text.replace(at, needle.size(), "\"hash\":\"h\",\"mesh\":null,\"heightmap\":null}");
    {
        std::ofstream f(mpath);
        f << text;
    }
    auto rr = load_region(root.string());
    REQUIRE_MESSAGE(rr, rr.error);
    CHECK(rr.region->tiles[0].mesh_glb.empty());
}

TEST_CASE("heightfield: water layer fills DEM holes as a lakebed and builds a flat surface") {
    // Region whose DEM has a nodata "lake" in the middle of tile (5,0,0).
    auto lake = [](double x, double y) {
        const double dx = x - 500040.0, dy = y - 5200040.0;
        return (dx * dx + dy * dy < 20.0 * 20.0) ? -9999.0 : 700.0 + 0.02 * (x - 500000.0);
    };
    const auto root = testutil::write_synth_region("water", lake);
    auto rr = load_region(root.string());
    REQUIRE(rr);
    const Region& R = *rr.region;
    const TileEntry& t = *R.find(5, 0, 0);
    auto hr = read_tiff(R.path(t.height_tif));
    REQUIRE(hr);
    const Frame fr = region_frame(R);
    auto dry = build_tile_mesh(R, t, *hr.raster, fr);
    REQUIRE(dry.ok());
    CHECK(dry.mesh.nodata_corners > 0);  // the hole, without water

    // Water raster: level 699 m on the hole pixels, nodata elsewhere.
    Raster w = *hr.raster;
    for (int y = 0; y < w.height; ++y)
        for (int x = 0; x < w.width; ++x) {
            const bool hole = hr.raster->is_nodata(hr.raster->at(x, y));
            const float v = hole ? 699.0f : -9999.0f;
            std::memcpy(&w.data[(static_cast<size_t>(y) * w.width + x) * 4], &v, 4);
        }
    MeshOptions opt;
    opt.water = &w;
    auto wet = build_tile_mesh(R, t, *hr.raster, fr, opt);
    REQUIRE(wet.ok());
    CHECK(wet.mesh.nodata_corners == 0);                     // no more holes
    CHECK(wet.mesh.min_z <= 699.0 - opt.bed_depth_m + 1e-6); // lakebed below the surface

    auto surf = build_water_mesh(R, t, w, fr);
    REQUIRE(surf.ok());
    CHECK(surf.mesh.surface_triangles > 0);
    for (const auto& p : surf.mesh.positions)
        CHECK(p.z == doctest::Approx(static_cast<float>((699.0 - fr.anchor_z) * 100.0)));
    // every water vertex sits above the lakebed terrain under it
    SurfaceSampler s;
    REQUIRE(s.build(R, t, *hr.raster, &w));
    double zb = 0;
    REQUIRE(s.height_at(500040.0, 5200040.0, zb));
    CHECK(zb < 699.0);
}

TEST_CASE("heightfield: SurfaceSampler::surface_at lies on the rendered triangles") {
    const auto root = testutil::write_synth_region("sampler_tri", hill);
    auto rr = load_region(root.string());
    REQUIRE(rr);
    const Region& R = *rr.region;
    const TileEntry& t = *R.find(5, 0, 0);
    auto tr = read_tiff(R.path(t.height_tif));
    REQUIRE(tr);
    SurfaceSampler s;
    REQUIRE(s.build(R, t, *tr.raster));
    const Frame fr = region_frame(R);
    auto m = build_tile_mesh(R, t, *tr.raster, fr);
    REQUIRE(m.ok());
    // Each surface triangle's centroid: the plane height there is the mean of its vertices.
    double worst_tri = 0;
    for (size_t k = 0; k < static_cast<size_t>(m.mesh.surface_triangles) * 3; k += 3) {
        const auto& p0 = m.mesh.positions[m.mesh.indices[k]];
        const auto& p1 = m.mesh.positions[m.mesh.indices[k + 1]];
        const auto& p2 = m.mesh.positions[m.mesh.indices[k + 2]];
        const double X = (p0.x + p1.x + p2.x) / 3.0, Y = (p0.y + p1.y + p2.y) / 3.0;
        const double Zc = (p0.z + p1.z + p2.z) / 3.0 / 100.0 + fr.anchor_z;
        const double wx = fr.anchor_x + X / 100.0, wy = fr.anchor_y - Y / 100.0;
        double z = 0;
        REQUIRE(s.surface_at(wx, wy, z));
        worst_tri = std::max(worst_tri, std::abs(z - Zc));
    }
    CHECK(worst_tri < 1e-3);        // on the triangles (float vertex positions)
}

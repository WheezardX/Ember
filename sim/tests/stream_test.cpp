#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "stream.h"

using namespace embersim;
namespace fs = std::filesystem;

namespace {
struct Reader {
    std::vector<uint8_t> b;
    size_t p = 0;
    explicit Reader(const fs::path& f) {
        std::ifstream in(f, std::ios::binary);
        b.assign(std::istreambuf_iterator<char>(in), {});
    }
    uint8_t u8() { return b.at(p++); }
    uint16_t u16() { uint16_t v = static_cast<uint16_t>(b.at(p) | (b.at(p + 1) << 8)); p += 2; return v; }
    uint32_t u32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(b.at(p + i)) << (8 * i);
        p += 4;
        return v;
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    uint64_t u64() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b.at(p + i)) << (8 * i);
        p += 8;
        return v;
    }
    int64_t i64() { return static_cast<int64_t>(u64()); }
    std::string str() {
        uint32_t n = u32();
        std::string s(reinterpret_cast<const char*>(b.data() + p), n);
        p += n;
        return s;
    }
};
}  // namespace

TEST_CASE("state stream is byte-exact to formats.md §4") {
    const uint32_t nx = 4, ny = 2;
    std::vector<uint8_t> phase = {1, 1, 2, 2, 0, 0, 0, 3};
    std::vector<uint8_t> inten = {0, 0, 1, 2, 0, 0, 0, 1};
    std::vector<int32_t> arr = {-1, -1, 0, 30, -1, -1, -1, 0};
    FireStateView v;
    v.nx = nx;
    v.ny = ny;
    v.phase = phase.data();
    v.intensity = inten.data();
    v.arrival_s = arr.data();

    StreamHeader h;
    h.nx = nx;
    h.ny = ny;
    h.cell_mm = 30000;
    h.t0_unix = 1502496000;
    h.dt_s = 60;
    h.keyframe_every = 10;
    h.world_pack_sha256_hex = std::string(62, 'a') + "0f";
    h.model_id = "null";
    h.model_version = "1.0.0";
    h.interface_version = "1.0.0";
    h.resources = {{"hc-1", "hand_t1"}};

    fs::path f = fs::temp_directory_path() / "embersim_stream_test.ess";
    {
        StreamWriter w(f, h);
        w.keyframe(0, 0, v);
        TickOutput out;
        out.dirty = {2, 3};
        out.spots.push_back({2, 7, 60, 120, true, false, 99});
        out.rejected.push_back({DeltaKind::RetardantApplied, 5});
        out.diag.push_back({"k", -7});
        std::vector<OverlayItem> ov = {{1, 5, 0, 0}};
        Metrics m;
        m.containment_permyriad = 1234;
        m.burning = 2;
        m.burned = 1;
        m.perimeter = 3;
        m.structures_lost = -1;
        m.structures_threatened = -1;
        m.cost_cents = 12345678901LL;
        m.busy_resources = 1;
        m.wind_u_cms = 300;
        m.wind_v_cms = -50;
        m.m10 = 60;
        w.tick(1, 60, 0xDEADBEEFCAFEBABEull, v, out, ov, m);
        w.end(1, 0xDEADBEEFCAFEBABEull);
        CHECK(w.bytes_written() == fs::file_size(f));
    }
    Reader r(f);
    CHECK(std::string(r.b.begin(), r.b.begin() + 8) == "EMBRSTRM");
    r.p = 8;
    CHECK(r.u32() == 1);
    CHECK(r.u32() == nx);
    CHECK(r.u32() == ny);
    CHECK(r.u32() == 30000);
    CHECK(r.i64() == 1502496000);
    CHECK(r.u32() == 60);
    CHECK(r.u32() == 10);
    CHECK(r.u32() == 0);
    CHECK(r.u8() == 0xaa);
    r.p += 30;
    CHECK(r.u8() == 0x0f);
    CHECK(r.str() == "null");
    CHECK(r.str() == "1.0.0");
    CHECK(r.str() == "1.0.0");
    CHECK(r.u32() == 1);
    CHECK(r.str() == "hc-1");
    CHECK(r.str() == "hand_t1");
    // keyframe
    CHECK(r.u8() == 2);
    CHECK(r.u32() == 0);
    CHECK(r.i32() == 0);
    CHECK(r.u32() == 4);  // phase runs: 1x2, 2x2, 0x3, 3x1
    CHECK(r.u32() == 2); CHECK(r.u8() == 1);
    CHECK(r.u32() == 2); CHECK(r.u8() == 2);
    CHECK(r.u32() == 3); CHECK(r.u8() == 0);
    CHECK(r.u32() == 1); CHECK(r.u8() == 3);
    CHECK(r.u32() == 5);  // intensity runs: 0x2,1x1,2x1,0x3,1x1
    for (int i = 0; i < 5; ++i) { r.u32(); r.u8(); }
    // tick
    CHECK(r.u8() == 1);
    CHECK(r.u32() == 1);
    CHECK(r.i32() == 60);
    CHECK(r.u64() == 0xDEADBEEFCAFEBABEull);
    CHECK(r.u32() == 2);
    CHECK(r.u32() == 2); CHECK(r.u8() == 2); CHECK(r.u8() == 1); CHECK(r.i32() == 0);
    CHECK(r.u32() == 3); CHECK(r.u8() == 2); CHECK(r.u8() == 2); CHECK(r.i32() == 30);
    CHECK(r.u32() == 1);
    CHECK(r.u32() == 2); CHECK(r.u32() == 7); CHECK(r.i32() == 60); CHECK(r.i32() == 120); CHECK(r.u8() == 1); CHECK(r.u8() == 0);
    CHECK(r.u32() == 1);
    CHECK(r.u8() == 1); CHECK(r.u32() == 5); CHECK(r.u16() == 0); CHECK(r.u16() == 0);
    CHECK(r.u32() == 1);
    CHECK(r.u8() == 2); CHECK(r.u32() == 5);
    CHECK(r.i32() == 1234);
    CHECK(r.u32() == 2); CHECK(r.u32() == 1); CHECK(r.u32() == 3);
    CHECK(r.i32() == -1); CHECK(r.i32() == -1);
    CHECK(r.i64() == 12345678901LL);
    CHECK(r.u32() == 1);
    CHECK(r.i32() == 300); CHECK(r.i32() == -50); CHECK(r.i32() == 60);
    CHECK(r.u32() == 1);
    CHECK(r.str() == "k"); CHECK(r.i64() == -7);
    // end
    CHECK(r.u8() == 3);
    CHECK(r.u32() == 1);
    CHECK(r.u64() == 0xDEADBEEFCAFEBABEull);
    CHECK(r.p == r.b.size());
}

TEST_CASE("stream: a bad sha hex writes 32 zero bytes") {
    StreamHeader h;
    h.nx = h.ny = 1;
    h.world_pack_sha256_hex = "test";
    fs::path f = fs::temp_directory_path() / "embersim_stream_test2.ess";
    {
        StreamWriter w(f, h);
        w.end(0, 0);
    }
    Reader r(f);
    r.p = 8 + 4 + 12 + 8 + 12;
    for (int i = 0; i < 32; ++i) CHECK(r.u8() == 0);
}

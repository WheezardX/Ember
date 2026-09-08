#include "worldpack.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "../third_party/json.hpp"
#include "sha256.h"
#include "version.h"

namespace embersim {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("world pack: cannot open " + p.string());
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(n));
    if (n > 0) f.read(reinterpret_cast<char*>(bytes.data()), n);
    return bytes;
}

template <class T>
void load_layer(const fs::path& dir, const json& layers, const char* name, size_t ncells,
                std::vector<T>& out, bool required, Sha256& hasher) {
    if (!layers.contains(name)) {
        if (required) throw std::runtime_error(std::string("world pack: required layer missing: ") + name);
        out.clear();
        return;
    }
    const json& L = layers.at(name);
    std::string file = L.value("file", std::string(name) + ".bin");
    std::vector<uint8_t> bytes = read_file(dir / file);
    hasher.update(bytes.data(), bytes.size());
    if (bytes.size() != ncells * sizeof(T)) {
        std::ostringstream m;
        m << "world pack: layer " << name << " (" << file << ") has " << bytes.size()
          << " bytes, expected " << ncells * sizeof(T) << " (" << sizeof(T) << " B x " << ncells << " cells)";
        throw std::runtime_error(m.str());
    }
    out.resize(ncells);
    // Little-endian on disk; assemble bytes explicitly so big-endian hosts agree.
    for (size_t i = 0; i < ncells; ++i) {
        uint64_t v = 0;
        for (size_t b = 0; b < sizeof(T); ++b) v |= static_cast<uint64_t>(bytes[i * sizeof(T) + b]) << (8 * b);
        out[i] = static_cast<T>(v);
    }
}

}  // namespace

World load_world(const fs::path& pack_dir) {
    fs::path mpath = pack_dir / "world.json";
    std::ifstream mf(mpath);
    if (!mf) throw std::runtime_error("world pack: cannot open " + mpath.string());
    std::stringstream ss;
    ss << mf.rdbuf();
    World w;
    w.manifest_json = ss.str();
    w.pack_dir = pack_dir;
    json m;
    try {
        m = json::parse(w.manifest_json);
    } catch (const std::exception& e) {
        throw std::runtime_error("world pack: world.json is not valid JSON: " + std::string(e.what()));
    }
    if (m.value("format", "") != "ember-world-pack")
        throw std::runtime_error("world pack: world.json format is not ember-world-pack");
    int ver = m.value("version", 0);
    if (ver != WORLDPACK_VERSION)
        throw std::runtime_error("world pack: version " + std::to_string(ver) + " unsupported (expected " +
                                 std::to_string(WORLDPACK_VERSION) + ")");
    w.name = m.value("name", pack_dir.filename().string());
    const json& g = m.at("grid");
    w.grid.nx = g.at("nx").get<uint32_t>();
    w.grid.ny = g.at("ny").get<uint32_t>();
    w.grid.cell_size_m = g.at("cell_size_m").get<double>();
    w.grid.cell_mm = static_cast<uint32_t>(w.grid.cell_size_m * 1000.0 + 0.5);
    w.grid.crs = g.value("crs", "");
    w.grid.origin_x = g.value("origin_x", 0.0);
    w.grid.origin_y = g.value("origin_y", 0.0);
    if (w.grid.nx == 0 || w.grid.ny == 0 || w.grid.cell_mm == 0)
        throw std::runtime_error("world pack: grid nx/ny/cell_size_m must be positive");
    w.t0_unix = m.value("t0_unix", int64_t{0});
    w.t0_utc = m.value("t0_utc", "");
    if (m.contains("source") && m["source"].is_object())
        w.world_manifest_hash = m["source"].value("world_manifest_hash", "");

    const json& layers = m.at("layers");
    size_t n = w.ncells();
    Sha256 hasher;
    // Hash in manifest order: iterate the manifest's layer list (json object preserves
    // insertion order in nlohmann only with ordered_json; we hash in the fixed order below and
    // record that order in formats.md as the canonical one).
    load_layer(pack_dir, layers, "elevation_cm", n, w.elevation_cm, true, hasher);
    load_layer(pack_dir, layers, "fbfm40", n, w.fbfm40, true, hasher);
    load_layer(pack_dir, layers, "cc_pct", n, w.cc_pct, true, hasher);
    load_layer(pack_dir, layers, "ch_dm", n, w.ch_dm, true, hasher);
    load_layer(pack_dir, layers, "cbh_dm", n, w.cbh_dm, true, hasher);
    load_layer(pack_dir, layers, "cbd_gm3", n, w.cbd_gm3, true, hasher);
    load_layer(pack_dir, layers, "evt", n, w.evt, true, hasher);
    load_layer(pack_dir, layers, "greenness", n, w.greenness, false, hasher);
    load_layer(pack_dir, layers, "structures", n, w.structures, false, hasher);
    load_layer(pack_dir, layers, "arrival_s", n, w.arrival_s, false, hasher);
    load_layer(pack_dir, layers, "confidence", n, w.confidence, false, hasher);
    load_layer(pack_dir, layers, "hillshade", n, w.hillshade, false, hasher);
    w.pack_sha256 = Sha256::hex(hasher.digest());

    if (m.contains("weather") && m["weather"].is_string()) {
        w.weather = load_weather_pack(pack_dir, m["weather"].get<std::string>());
    }
    return w;
}

FuelClass fuel_class_of(uint8_t c) {
    if (c >= 101 && c <= 109) return FuelClass::GR;
    if (c >= 121 && c <= 124) return FuelClass::GS;
    if (c >= 141 && c <= 149) return FuelClass::SH;
    if (c >= 161 && c <= 165) return FuelClass::TU;
    if (c >= 181 && c <= 189) return FuelClass::TL;
    if (c >= 201 && c <= 204) return FuelClass::SB;
    return FuelClass::NB;  // 91-99 and anything unknown
}

const char* fuel_class_name(FuelClass c) {
    switch (c) {
        case FuelClass::NB: return "NB";
        case FuelClass::GR: return "GR";
        case FuelClass::GS: return "GS";
        case FuelClass::SH: return "SH";
        case FuelClass::TU: return "TU";
        case FuelClass::TL: return "TL";
        case FuelClass::SB: return "SB";
    }
    return "NB";
}

}  // namespace embersim

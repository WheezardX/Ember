#pragma once
// World pack loader (docs/sim/formats.md §1). The only world input the core ever reads.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "weather.h"

namespace embersim {

struct GridInfo {
    uint32_t nx = 0, ny = 0;
    double cell_size_m = 30.0;
    uint32_t cell_mm = 30000;  // round(cell_size_m * 1000)
    std::string crs;
    double origin_x = 0, origin_y = 0;  // top-left corner, metres in crs
    size_t ncells() const { return static_cast<size_t>(nx) * ny; }
    bool in_bounds(int64_t x, int64_t y) const { return x >= 0 && y >= 0 && x < nx && y < ny; }
    uint32_t index(uint32_t x, uint32_t y) const { return y * nx + x; }
};

inline constexpr int32_t ELEV_NODATA = INT32_MIN;

struct World {
    std::string name;
    std::filesystem::path pack_dir;
    std::string pack_sha256;          // recomputed at load over the manifest's layer files
    std::string world_manifest_hash;  // from source (may be empty for synthetic)
    std::string manifest_json;        // raw world.json text (kept for replay pinning)
    GridInfo grid;
    int64_t t0_unix = 0;
    std::string t0_utc;

    std::vector<int32_t> elevation_cm;
    std::vector<uint8_t> fbfm40, cc_pct;
    std::vector<uint16_t> ch_dm, cbh_dm, cbd_gm3, evt;
    std::vector<uint8_t> greenness, structures, confidence, hillshade;  // optional (empty if absent)
    std::vector<int32_t> arrival_s;                                     // optional
    std::optional<WeatherPack> weather;

    bool has_greenness() const { return !greenness.empty(); }
    bool has_structures() const { return !structures.empty(); }
    bool has_arrival() const { return !arrival_s.empty(); }
    bool has_confidence() const { return !confidence.empty(); }
    bool has_hillshade() const { return !hillshade.empty(); }
    size_t ncells() const { return grid.ncells(); }
};

// Throws std::runtime_error with a message naming the file/field on any problem.
World load_world(const std::filesystem::path& pack_dir);

// Fuel classes (spec §5.1). Order is fixed; the CA params pack indexes by it.
enum class FuelClass : uint8_t { NB = 0, GR = 1, GS = 2, SH = 3, TU = 4, TL = 5, SB = 6 };
inline constexpr int FUEL_CLASS_COUNT = 7;
FuelClass fuel_class_of(uint8_t fbfm40);
const char* fuel_class_name(FuelClass c);
inline bool burnable(uint8_t fbfm40) { return fuel_class_of(fbfm40) != FuelClass::NB; }

}  // namespace embersim

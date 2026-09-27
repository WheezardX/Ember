// Minimal baseline-TIFF / GeoTIFF reader for Terrain's per-tile rasters (EPIC_5_PLAN C1).
//
// Scope, deliberately small: classic (non-Big) little-endian TIFF, one sample per pixel,
// uncompressed (Compression = 1), strips or tiles, 8/16/32/64-bit int/uint/float samples,
// plus the three GeoTIFF bits a renderer needs: ModelPixelScale, ModelTiepoint and the
// GDAL_NODATA string. Terrain writes every per-tile raster in exactly this form; the
// AOI-level COGs (DEFLATE + predictor) are out of scope and are refused with a message.
// Engine-free C++20: compiled by CMake (worldcore_tests) and by the UE EmberWorld module.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace emberworld {

enum class SampleType : uint8_t { U8, I8, U16, I16, U32, I32, F32, F64 };

const char* sample_type_name(SampleType t);
int sample_bytes(SampleType t);

struct GeoInfo {
    bool has_transform = false;
    double origin_x = 0.0;      // world X of the raster's top-left corner (PixelIsArea)
    double origin_y = 0.0;      // world Y of the top-left corner (north edge)
    double pixel_w = 0.0;       // metres per pixel, east
    double pixel_h = 0.0;       // metres per pixel, south (positive number)
    bool pixel_is_point = false;
    std::optional<double> nodata;
};

struct Raster {
    int width = 0;
    int height = 0;
    SampleType type = SampleType::F32;
    std::vector<uint8_t> data;  // width*height samples, row 0 = north, native little-endian
    GeoInfo geo;

    double at(int x, int y) const;               // sample as double
    bool is_nodata(double v) const { return geo.nodata && v == *geo.nodata; }
    std::vector<float> to_float() const;         // all samples as float (nodata kept as-is)
};

struct TiffError {
    std::string message;
};

// Returns the raster, or an error. Never throws.
struct TiffResult {
    std::optional<Raster> raster;
    TiffError error;
    explicit operator bool() const { return raster.has_value(); }
};

TiffResult read_tiff(const std::string& path);
TiffResult read_tiff_memory(const uint8_t* bytes, size_t size);

}  // namespace emberworld

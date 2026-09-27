#include "emberworld/tiff.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

namespace emberworld {

const char* sample_type_name(SampleType t) {
    switch (t) {
        case SampleType::U8: return "u8";
        case SampleType::I8: return "i8";
        case SampleType::U16: return "u16";
        case SampleType::I16: return "i16";
        case SampleType::U32: return "u32";
        case SampleType::I32: return "i32";
        case SampleType::F32: return "f32";
        case SampleType::F64: return "f64";
    }
    return "?";
}

int sample_bytes(SampleType t) {
    switch (t) {
        case SampleType::U8: case SampleType::I8: return 1;
        case SampleType::U16: case SampleType::I16: return 2;
        case SampleType::U32: case SampleType::I32: case SampleType::F32: return 4;
        case SampleType::F64: return 8;
    }
    return 0;
}

double Raster::at(int x, int y) const {
    const size_t i = static_cast<size_t>(y) * width + x;
    const uint8_t* p = data.data() + i * sample_bytes(type);
    switch (type) {
        case SampleType::U8: return *p;
        case SampleType::I8: return static_cast<int8_t>(*p);
        case SampleType::U16: { uint16_t v; std::memcpy(&v, p, 2); return v; }
        case SampleType::I16: { int16_t v; std::memcpy(&v, p, 2); return v; }
        case SampleType::U32: { uint32_t v; std::memcpy(&v, p, 4); return v; }
        case SampleType::I32: { int32_t v; std::memcpy(&v, p, 4); return v; }
        case SampleType::F32: { float v; std::memcpy(&v, p, 4); return v; }
        case SampleType::F64: { double v; std::memcpy(&v, p, 8); return v; }
    }
    return 0.0;
}

std::vector<float> Raster::to_float() const {
    std::vector<float> out(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            out[static_cast<size_t>(y) * width + x] = static_cast<float>(at(x, y));
    return out;
}

namespace {

enum Tag : uint16_t {
    kImageWidth = 256, kImageLength = 257, kBitsPerSample = 258, kCompression = 259,
    kStripOffsets = 273, kSamplesPerPixel = 277, kRowsPerStrip = 278, kStripByteCounts = 279,
    kPlanarConfig = 284, kPredictor = 317, kTileWidth = 322, kTileLength = 323,
    kTileOffsets = 324, kTileByteCounts = 325, kSampleFormat = 339,
    kModelPixelScale = 33550, kModelTiepoint = 33922, kGeoKeyDirectory = 34735,
    kGdalNodata = 42113,
};

struct Entry {
    uint16_t type = 0;
    uint32_t count = 0;
    uint32_t value_or_offset = 0;
    size_t entry_pos = 0;  // position of the 4-byte value field
};

class Reader {
public:
    Reader(const uint8_t* b, size_t n) : b_(b), n_(n) {}
    bool ok(size_t pos, size_t len) const { return pos <= n_ && len <= n_ - pos; }
    uint16_t u16(size_t p) const { uint16_t v; std::memcpy(&v, b_ + p, 2); return v; }
    uint32_t u32(size_t p) const { uint32_t v; std::memcpy(&v, b_ + p, 4); return v; }
    double f64(size_t p) const { double v; std::memcpy(&v, b_ + p, 8); return v; }
    const uint8_t* at(size_t p) const { return b_ + p; }

private:
    const uint8_t* b_;
    size_t n_;
};

int type_size(uint16_t type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1;   // BYTE ASCII SBYTE UNDEFINED
        case 3: case 8: return 2;                   // SHORT SSHORT
        case 4: case 9: case 11: return 4;          // LONG SLONG FLOAT
        case 5: case 10: case 12: return 8;         // RATIONAL SRATIONAL DOUBLE
        default: return 0;
    }
}

TiffResult fail(std::string m) {
    TiffResult r;
    r.error.message = std::move(m);
    return r;
}

}  // namespace

TiffResult read_tiff_memory(const uint8_t* bytes, size_t size) {
    Reader rd(bytes, size);
    if (!rd.ok(0, 8)) return fail("file too small for a TIFF header");
    if (bytes[0] == 'M' && bytes[1] == 'M') return fail("big-endian TIFF not supported");
    if (bytes[0] != 'I' || bytes[1] != 'I') return fail("not a TIFF (bad byte-order mark)");
    const uint16_t magic = rd.u16(2);
    if (magic == 43) return fail("BigTIFF not supported");
    if (magic != 42) return fail("not a TIFF (bad magic)");
    const uint32_t ifd = rd.u32(4);
    if (!rd.ok(ifd, 2)) return fail("IFD offset out of range");
    const uint16_t n = rd.u16(ifd);
    if (!rd.ok(ifd + 2, static_cast<size_t>(n) * 12)) return fail("IFD truncated");

    std::map<uint16_t, Entry> tags;
    for (uint16_t i = 0; i < n; ++i) {
        const size_t p = ifd + 2 + static_cast<size_t>(i) * 12;
        Entry e;
        e.type = rd.u16(p + 2);
        e.count = rd.u32(p + 4);
        e.value_or_offset = rd.u32(p + 8);
        e.entry_pos = p + 8;
        tags[rd.u16(p)] = e;
    }

    // Where an entry's values live (inline when they fit in 4 bytes).
    auto data_pos = [&](const Entry& e) -> std::optional<size_t> {
        const size_t sz = static_cast<size_t>(type_size(e.type)) * e.count;
        if (type_size(e.type) == 0) return std::nullopt;
        const size_t pos = sz <= 4 ? e.entry_pos : e.value_or_offset;
        if (!rd.ok(pos, sz)) return std::nullopt;
        return pos;
    };
    auto uints = [&](uint16_t tag) -> std::optional<std::vector<uint64_t>> {
        auto it = tags.find(tag);
        if (it == tags.end()) return std::nullopt;
        const Entry& e = it->second;
        auto pos = data_pos(e);
        if (!pos) return std::nullopt;
        std::vector<uint64_t> v(e.count);
        for (uint32_t i = 0; i < e.count; ++i) {
            if (e.type == 3) v[i] = rd.u16(*pos + 2 * i);
            else if (e.type == 4) v[i] = rd.u32(*pos + 4 * i);
            else if (e.type == 1) v[i] = *rd.at(*pos + i);
            else return std::nullopt;
        }
        return v;
    };
    auto uint1 = [&](uint16_t tag, uint64_t dflt) -> uint64_t {
        auto v = uints(tag);
        return (v && !v->empty()) ? (*v)[0] : dflt;
    };
    auto doubles = [&](uint16_t tag) -> std::optional<std::vector<double>> {
        auto it = tags.find(tag);
        if (it == tags.end() || it->second.type != 12) return std::nullopt;
        auto pos = data_pos(it->second);
        if (!pos) return std::nullopt;
        std::vector<double> v(it->second.count);
        for (uint32_t i = 0; i < it->second.count; ++i) v[i] = rd.f64(*pos + 8 * i);
        return v;
    };

    Raster r;
    r.width = static_cast<int>(uint1(kImageWidth, 0));
    r.height = static_cast<int>(uint1(kImageLength, 0));
    if (r.width <= 0 || r.height <= 0) return fail("missing or zero image dimensions");
    if (uint1(kSamplesPerPixel, 1) != 1) return fail("only single-sample rasters supported");
    const uint64_t compression = uint1(kCompression, 1);
    if (compression != 1)
        return fail("compressed TIFF (Compression=" + std::to_string(compression) +
                    ") not supported; read the per-tile uncompressed rasters");
    if (uint1(kPredictor, 1) != 1) return fail("TIFF predictor not supported");
    if (uint1(kPlanarConfig, 1) != 1 && uint1(kSamplesPerPixel, 1) != 1)
        return fail("planar configuration not supported");
    const uint64_t bits = uint1(kBitsPerSample, 1);
    const uint64_t fmt = uint1(kSampleFormat, 1);  // 1 uint, 2 int, 3 float
    if (fmt == 3 && bits == 32) r.type = SampleType::F32;
    else if (fmt == 3 && bits == 64) r.type = SampleType::F64;
    else if (fmt == 1 && bits == 8) r.type = SampleType::U8;
    else if (fmt == 2 && bits == 8) r.type = SampleType::I8;
    else if (fmt == 1 && bits == 16) r.type = SampleType::U16;
    else if (fmt == 2 && bits == 16) r.type = SampleType::I16;
    else if (fmt == 1 && bits == 32) r.type = SampleType::U32;
    else if (fmt == 2 && bits == 32) r.type = SampleType::I32;
    else return fail("unsupported sample format " + std::to_string(fmt) + "/" + std::to_string(bits) + " bits");

    const size_t bps = static_cast<size_t>(sample_bytes(r.type));
    const size_t row_bytes = static_cast<size_t>(r.width) * bps;
    r.data.assign(row_bytes * r.height, 0);

    if (tags.count(kTileOffsets)) {
        const size_t tw = uint1(kTileWidth, 0), tl = uint1(kTileLength, 0);
        auto offs = uints(kTileOffsets);
        auto cnts = uints(kTileByteCounts);
        if (!tw || !tl || !offs || !cnts || offs->size() != cnts->size())
            return fail("malformed tile tags");
        const size_t across = (r.width + tw - 1) / tw, down = (r.height + tl - 1) / tl;
        if (offs->size() < across * down) return fail("too few tiles");
        for (size_t ty = 0; ty < down; ++ty)
            for (size_t tx = 0; tx < across; ++tx) {
                const size_t k = ty * across + tx;
                const size_t off = (*offs)[k];
                if (!rd.ok(off, tw * tl * bps)) return fail("tile data out of range");
                for (size_t y = 0; y < tl; ++y) {
                    const size_t gy = ty * tl + y;
                    if (gy >= static_cast<size_t>(r.height)) break;
                    const size_t gx = tx * tw;
                    const size_t ncols = std::min(tw, static_cast<size_t>(r.width) - gx);
                    std::memcpy(r.data.data() + gy * row_bytes + gx * bps,
                                rd.at(off + y * tw * bps), ncols * bps);
                }
            }
    } else {
        auto offs = uints(kStripOffsets);
        auto cnts = uints(kStripByteCounts);
        if (!offs || !cnts || offs->size() != cnts->size()) return fail("malformed strip tags");
        const size_t rps = std::min<uint64_t>(uint1(kRowsPerStrip, r.height), r.height);
        size_t row = 0;
        for (size_t s = 0; s < offs->size() && row < static_cast<size_t>(r.height); ++s) {
            const size_t rows = std::min(rps, static_cast<size_t>(r.height) - row);
            const size_t need = rows * row_bytes;
            if ((*cnts)[s] < need || !rd.ok((*offs)[s], need)) return fail("strip data out of range");
            std::memcpy(r.data.data() + row * row_bytes, rd.at((*offs)[s]), need);
            row += rows;
        }
        if (row != static_cast<size_t>(r.height)) return fail("strips do not cover the image");
    }

    // GeoTIFF: tiepoint (I,J,K,X,Y,Z) + pixel scale (sx, sy, sz); raster type from GeoKeys.
    auto scale = doubles(kModelPixelScale);
    auto tie = doubles(kModelTiepoint);
    if (scale && tie && scale->size() >= 2 && tie->size() >= 6) {
        r.geo.has_transform = true;
        r.geo.pixel_w = (*scale)[0];
        r.geo.pixel_h = (*scale)[1];
        r.geo.origin_x = (*tie)[3] - (*tie)[0] * r.geo.pixel_w;
        r.geo.origin_y = (*tie)[4] + (*tie)[1] * r.geo.pixel_h;
    }
    if (auto keys = uints(kGeoKeyDirectory); keys && keys->size() >= 4) {
        const size_t nk = (*keys)[3];
        for (size_t i = 0; i < nk && 4 + 4 * i + 3 < keys->size(); ++i) {
            const size_t k = 4 + 4 * i;
            if ((*keys)[k] == 1025 && (*keys)[k + 1] == 0) r.geo.pixel_is_point = (*keys)[k + 3] == 2;
        }
    }
    if (auto it = tags.find(kGdalNodata); it != tags.end() && it->second.type == 2) {
        if (auto pos = data_pos(it->second)) {
            std::string s(reinterpret_cast<const char*>(rd.at(*pos)), it->second.count);
            while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
            char* end = nullptr;
            const double v = std::strtod(s.c_str(), &end);
            if (end != s.c_str()) r.geo.nodata = v;
        }
    }

    TiffResult out;
    out.raster = std::move(r);
    return out;
}

TiffResult read_tiff(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("cannot open " + path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    TiffResult r = read_tiff_memory(bytes.data(), bytes.size());
    if (!r) r.error.message = path + ": " + r.error.message;
    return r;
}

}  // namespace emberworld

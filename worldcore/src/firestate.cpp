#include "emberworld/firestate.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "json.hpp"

namespace emberworld::fire {

namespace {
constexpr uint8_t kTick = 1, kKeyframe = 2, kEnd = 3;

// Little-endian cursor over the loaded file; every read is bounds-checked.
struct Cursor {
    const std::vector<uint8_t>& b;
    size_t p = 0;
    bool bad = false;

    bool need(size_t n) {
        if (p + n > b.size()) bad = true;
        return !bad;
    }
    template <class T> T get() {
        T v{};
        if (need(sizeof(T))) {
            std::memcpy(&v, b.data() + p, sizeof(T));  // host is little-endian (x86/ARM)
            p += sizeof(T);
        }
        return v;
    }
    std::string str() {
        const uint32_t n = get<uint32_t>();
        if (!need(n)) return {};
        std::string s(reinterpret_cast<const char*>(b.data() + p), n);
        p += n;
        return s;
    }
    void skip_array(size_t item) {
        const uint32_t n = get<uint32_t>();
        if (need(static_cast<size_t>(n) * item)) p += static_cast<size_t>(n) * item;
    }
};

constexpr size_t kRun = 5, kDirty = 10, kSpot = 18, kOverlay = 9, kRejected = 5;

// Expand one RLE array (u32 n_runs { u32 len, u8 value }) into out; false if it does not cover
// exactly out.size() cells.
bool expand(Cursor& c, std::vector<uint8_t>& out) {
    const uint32_t n = c.get<uint32_t>();
    size_t at = 0;
    for (uint32_t i = 0; i < n && !c.bad; ++i) {
        const uint32_t len = c.get<uint32_t>();
        const uint8_t v = c.get<uint8_t>();
        if (at + len > out.size()) return false;
        std::fill(out.begin() + static_cast<std::ptrdiff_t>(at),
                  out.begin() + static_cast<std::ptrdiff_t>(at + len), v);
        at += len;
    }
    return !c.bad && at == out.size();
}

Metrics read_metrics(Cursor& c) {
    Metrics m;
    m.containment_permyriad = c.get<int32_t>();
    m.burning = c.get<uint32_t>();
    m.burned = c.get<uint32_t>();
    m.perimeter = c.get<uint32_t>();
    m.structures_lost = c.get<int32_t>();
    m.structures_threatened = c.get<int32_t>();
    m.cost_cents = c.get<int64_t>();
    m.busy_resources = c.get<uint32_t>();
    m.wind_u_cms = c.get<int32_t>();
    m.wind_v_cms = c.get<int32_t>();
    m.m10 = c.get<int32_t>();
    return m;
}
}  // namespace

std::string Stream::open(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "cannot open state stream " + path;
    bytes_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    const std::string err = parse();
    return err.empty() ? err : path + ": " + err;
}

std::string Stream::parse() {
    Cursor c{bytes_};
    if (!c.need(8) || std::memcmp(bytes_.data(), "EMBRSTRM", 8) != 0) return "not an ember state stream";
    c.p = 8;
    StreamHeader& h = header_;
    h.version = c.get<uint32_t>();
    if (h.version != 1) return "unsupported state stream version " + std::to_string(h.version);
    h.nx = c.get<uint32_t>();
    h.ny = c.get<uint32_t>();
    h.cell_mm = c.get<uint32_t>();
    h.t0_unix = c.get<int64_t>();
    h.dt_s = c.get<uint32_t>();
    h.keyframe_every = c.get<uint32_t>();
    h.flags = c.get<uint32_t>();
    if (c.need(32)) {
        static const char* hex = "0123456789abcdef";
        for (int i = 0; i < 32; ++i) {
            const uint8_t v = bytes_[c.p + i];
            h.world_pack_sha256 += hex[v >> 4];
            h.world_pack_sha256 += hex[v & 15];
        }
        c.p += 32;
    }
    h.model_id = c.str();
    h.model_version = c.str();
    h.interface_version = c.str();
    const uint32_t nres = c.get<uint32_t>();
    for (uint32_t i = 0; i < nres && !c.bad; ++i) {
        c.str();
        c.str();
    }
    if (c.bad) return "truncated header";

    const size_t n = h.cells();
    arrival_.assign(n, -1);
    std::vector<uint8_t> scratch(n);
    while (c.p < bytes_.size()) {
        const uint8_t kind = c.get<uint8_t>();
        if (kind == kKeyframe) {
            KeyRec k{c.get<uint32_t>(), c.get<int32_t>(), 0};
            k.offset = c.p;
            if (!expand(c, scratch) || !expand(c, scratch)) return "bad keyframe at tick " + std::to_string(k.tick);
            keyframes_.push_back(k);
        } else if (kind == kTick) {
            TickRec t{c.get<uint32_t>(), c.get<int32_t>(), 0, {}};
            c.get<uint64_t>();  // state hash
            t.offset = c.p;
            const uint32_t nd = c.get<uint32_t>();
            if (!c.need(static_cast<size_t>(nd) * kDirty)) break;
            for (uint32_t i = 0; i < nd; ++i) {
                uint32_t idx;
                int32_t arr;
                std::memcpy(&idx, bytes_.data() + c.p, 4);
                std::memcpy(&arr, bytes_.data() + c.p + 6, 4);
                c.p += kDirty;
                if (idx < n && arr >= 0 && arrival_[idx] < 0) arrival_[idx] = arr;  // written once
            }
            c.skip_array(kSpot);
            c.skip_array(kOverlay);
            c.skip_array(kRejected);
            t.metrics = read_metrics(c);
            const uint32_t ndiag = c.get<uint32_t>();
            for (uint32_t i = 0; i < ndiag && !c.bad; ++i) {
                c.str();
                c.get<int64_t>();
            }
            if (c.bad) break;  // tolerate a stream cut mid-record (crash mid-run), like the Python reader
            ticks_.push_back(t);
        } else if (kind == kEnd) {
            break;
        } else {
            return "unknown record kind " + std::to_string(kind);
        }
    }
    if (keyframes_.empty()) return "no keyframe";
    return {};
}

State Stream::at(int32_t t_s) const {
    State s;
    const size_t n = header_.cells();
    s.phase.assign(n, 0);
    s.intensity.assign(n, 0);
    // Nearest keyframe at or before t (the first one if t precedes it).
    size_t k = 0;
    for (size_t i = 0; i < keyframes_.size(); ++i)
        if (keyframes_[i].t_s <= t_s) k = i;
    const KeyRec& kr = keyframes_[k];
    Cursor c{bytes_, kr.offset};
    expand(c, s.phase);
    expand(c, s.intensity);
    s.tick = kr.tick;
    s.t_s = kr.t_s;
    // A keyframe is written after its tick's own record: that record carries the metrics.
    auto own = std::lower_bound(ticks_.begin(), ticks_.end(), kr.tick,
                                [](const TickRec& r, uint32_t tick) { return r.tick < tick; });
    if (own != ticks_.end() && own->tick == kr.tick) s.metrics = own->metrics;
    // Ticks after the keyframe (tick numbers > the keyframe's) up to t.
    auto it = std::upper_bound(ticks_.begin(), ticks_.end(), kr.tick,
                               [](uint32_t tick, const TickRec& r) { return tick < r.tick; });
    for (; it != ticks_.end() && it->t_s <= t_s; ++it) {
        Cursor d{bytes_, it->offset};
        const uint32_t nd = d.get<uint32_t>();
        for (uint32_t i = 0; i < nd; ++i) {
            uint32_t idx;
            std::memcpy(&idx, bytes_.data() + d.p, 4);
            if (idx < n) {
                s.phase[idx] = bytes_[d.p + 4];
                s.intensity[idx] = bytes_[d.p + 5];
            }
            d.p += kDirty;
        }
        s.tick = it->tick;
        s.t_s = it->t_s;
        s.metrics = it->metrics;
    }
    return s;
}

ReplayInfo read_replay(const std::string& path) {
    ReplayInfo r;
    try {
        std::ifstream f(path);
        if (!f) {
            r.error = "cannot open replay " + path;
            return r;
        }
        const nlohmann::json j = nlohmann::json::parse(f);
        if (j.value("format", "") != "ember-replay") {
            r.error = path + ": not an ember-replay";
            return r;
        }
        const auto& w = j.at("world");
        const auto& g = w.at("grid");
        r.grid.origin_x = g.at("origin_x").get<double>();
        r.grid.origin_y = g.at("origin_y").get<double>();
        r.grid.cell_m = g.at("cell_size_m").get<double>();
        r.grid.crs = g.value("crs", "");
        r.t0_unix = w.value("t0_unix", int64_t{0});
        r.model_id = j.at("model").value("id", "");
        const std::string stream = j.value("stream", "");
        if (stream.empty()) {
            r.error = path + ": replay has no state stream";
            return r;
        }
        r.stream_path = (std::filesystem::path(path).parent_path() / stream).string();
    } catch (const std::exception& e) {
        r.error = path + ": " + e.what();
    }
    return r;
}

std::vector<float> spread_rate_mh(const std::vector<int32_t>& arrival, uint32_t nx, uint32_t ny,
                                  double cell_m, float max_mh) {
    std::vector<float> out(arrival.size(), 0.f);
    if (arrival.size() != static_cast<size_t>(nx) * ny) return out;
    for (uint32_t y = 0; y < ny; ++y) {
        for (uint32_t x = 0; x < nx; ++x) {
            const int32_t a0 = arrival[static_cast<size_t>(y) * nx + x];
            if (a0 < 0) continue;
            // Normal equations of a - a0 = gx dx + gy dy over the arrived neighbours (joint fit:
            // separate 1-D fits are biased where the neighbourhood is one-sided).
            double sxa = 0, sya = 0, sxx = 0, syy = 0, sxy = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int64_t xx = static_cast<int64_t>(x) + dx, yy = static_cast<int64_t>(y) + dy;
                    if ((dx == 0 && dy == 0) || xx < 0 || yy < 0 || xx >= nx || yy >= ny) continue;
                    const int32_t a = arrival[static_cast<size_t>(yy) * nx + static_cast<size_t>(xx)];
                    if (a < 0) continue;
                    sxa += dx * static_cast<double>(a - a0);
                    sya += dy * static_cast<double>(a - a0);
                    sxx += dx * dx;
                    syy += dy * dy;
                    sxy += dx * dy;
                }
            }
            if (sxx == 0 && syy == 0) continue;
            double gx = 0, gy = 0;   // s per cell
            const double det = sxx * syy - sxy * sxy;
            if (det > 1e-9) {
                gx = (syy * sxa - sxy * sya) / det;
                gy = (sxx * sya - sxy * sxa) / det;
            } else {  // neighbours on one line through the cell: only the slope along it is known
                double ux = sxx > 0 ? sxx : 0.0, uy = sxx > 0 ? sxy : 1.0;   // the line's direction
                const double un = std::sqrt(ux * ux + uy * uy);
                ux /= un;
                uy /= un;
                const double along = (ux * sxa + uy * sya) / (sxx + syy);   // s per cell along it
                gx = along * ux;
                gy = along * uy;
            }
            const double g = std::sqrt(gx * gx + gy * gy);
            const double r = g > 0 ? cell_m * 3600.0 / g : max_mh;
            out[static_cast<size_t>(y) * nx + x] = static_cast<float>(std::min<double>(r, max_mh));
        }
    }
    return out;
}

}  // namespace emberworld::fire

#include "stream.h"

#include <stdexcept>

#include "version.h"

namespace embersim {

namespace {
int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

StreamWriter::StreamWriter(const std::filesystem::path& path, const StreamHeader& h) {
    f_ = std::fopen(path.string().c_str(), "wb");
    if (!f_) throw std::runtime_error("stream: cannot create " + path.string());
    std::setvbuf(f_, nullptr, _IOFBF, 1 << 20);
    std::fwrite("EMBRSTRM", 1, 8, f_);
    bytes_ += 8;
    u32(static_cast<uint32_t>(STREAM_VERSION));
    u32(h.nx);
    u32(h.ny);
    u32(h.cell_mm);
    i64(h.t0_unix);
    u32(h.dt_s);
    u32(h.keyframe_every);
    u32(h.flags);
    uint8_t sha[32] = {0};
    if (h.world_pack_sha256_hex.size() == 64) {
        bool ok = true;
        for (int i = 0; i < 32; ++i) {
            int a = hexval(h.world_pack_sha256_hex[static_cast<size_t>(i) * 2]);
            int b = hexval(h.world_pack_sha256_hex[static_cast<size_t>(i) * 2 + 1]);
            if (a < 0 || b < 0) { ok = false; break; }
            sha[i] = static_cast<uint8_t>(a * 16 + b);
        }
        if (!ok) for (auto& b : sha) b = 0;
    }
    std::fwrite(sha, 1, 32, f_);
    bytes_ += 32;
    str(h.model_id);
    str(h.model_version);
    str(h.interface_version);
    u32(static_cast<uint32_t>(h.resources.size()));
    for (const auto& [id, type] : h.resources) {
        str(id);
        str(type);
    }
}

StreamWriter::~StreamWriter() {
    if (f_) {
        std::fclose(f_);
        f_ = nullptr;
    }
}

void StreamWriter::u8(uint8_t v) { std::fputc(v, f_); ++bytes_; }
void StreamWriter::u16(uint16_t v) { u8(static_cast<uint8_t>(v & 0xFF)); u8(static_cast<uint8_t>(v >> 8)); }
void StreamWriter::u32(uint32_t v) {
    uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
    std::fwrite(b, 1, 4, f_);
    bytes_ += 4;
}
void StreamWriter::u64(uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(v >> (8 * i));
    std::fwrite(b, 1, 8, f_);
    bytes_ += 8;
}
void StreamWriter::str(const std::string& s) {
    u32(static_cast<uint32_t>(s.size()));
    if (!s.empty()) std::fwrite(s.data(), 1, s.size(), f_);
    bytes_ += s.size();
}

void StreamWriter::keyframe(uint32_t tick, int32_t t_s, const FireStateView& v) {
    if (ended_) throw std::runtime_error("stream: write after end");
    u8(2);
    u32(tick);
    i32(t_s);
    auto rle = [&](const uint8_t* a) {
        size_t n = v.ncells();
        std::vector<std::pair<uint32_t, uint8_t>> runs;
        size_t i = 0;
        while (i < n) {
            size_t j = i + 1;
            while (j < n && a[j] == a[i]) ++j;
            runs.emplace_back(static_cast<uint32_t>(j - i), a[i]);
            i = j;
        }
        u32(static_cast<uint32_t>(runs.size()));
        for (const auto& [len, val] : runs) {
            u32(len);
            u8(val);
        }
    };
    rle(v.phase);
    rle(v.intensity);
}

void StreamWriter::tick(uint32_t tick, int32_t t_s, uint64_t state_hash, const FireStateView& v, const TickOutput& out,
                        const std::vector<OverlayItem>& overlay, const Metrics& m) {
    if (ended_) throw std::runtime_error("stream: write after end");
    u8(1);
    u32(tick);
    i32(t_s);
    u64(state_hash);
    u32(static_cast<uint32_t>(out.dirty.size()));
    for (uint32_t idx : out.dirty) {
        u32(idx);
        u8(v.phase[idx]);
        u8(v.intensity[idx]);
        i32(v.arrival_s[idx]);
    }
    u32(static_cast<uint32_t>(out.spots.size()));
    for (const SpotEvent& s : out.spots) {
        u32(s.src);
        u32(s.dst);
        i32(s.launch_s);
        i32(s.land_s);
        u8(s.landed ? 1 : 0);
        u8(s.ignited ? 1 : 0);
    }
    u32(static_cast<uint32_t>(overlay.size()));
    for (const OverlayItem& o : overlay) {
        u8(o.delta_kind);
        u32(o.idx);
        u16(o.magnitude);
        u16(o.resource_idx);
    }
    u32(static_cast<uint32_t>(out.rejected.size()));
    for (const RejectedDelta& r : out.rejected) {
        u8(static_cast<uint8_t>(r.kind));
        u32(r.count);
    }
    i32(m.containment_permyriad);
    u32(m.burning);
    u32(m.burned);
    u32(m.perimeter);
    i32(m.structures_lost);
    i32(m.structures_threatened);
    i64(m.cost_cents);
    u32(m.busy_resources);
    i32(m.wind_u_cms);
    i32(m.wind_v_cms);
    i32(m.m10);
    u32(static_cast<uint32_t>(out.diag.size()));
    for (const Diag& d : out.diag) {
        str(d.key);
        i64(d.value);
    }
}

void StreamWriter::end(uint32_t ticks, uint64_t final_hash) {
    if (ended_) return;
    u8(3);
    u32(ticks);
    u64(final_hash);
    ended_ = true;
    std::fflush(f_);
    std::fclose(f_);
    f_ = nullptr;
}

}  // namespace embersim

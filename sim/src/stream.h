#pragma once
// State stream writer (formats.md §4). Byte-exact; the Python reader in ember/sim/stream.py
// is the other half of the contract.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "interface.h"
#include "observers.h"

namespace embersim {

struct OverlayItem {
    uint8_t delta_kind = 0;
    uint32_t idx = 0;
    uint16_t magnitude = 0;
    uint16_t resource_idx = 0xFFFF;
};

struct StreamHeader {
    uint32_t nx = 0, ny = 0, cell_mm = 0;
    int64_t t0_unix = 0;
    uint32_t dt_s = 0, keyframe_every = 0, flags = 0;
    std::string world_pack_sha256_hex;  // 64 hex chars -> 32 raw bytes
    std::string model_id, model_version, interface_version;
    std::vector<std::pair<std::string, std::string>> resources;  // (id, type)
};

class StreamWriter {
public:
    StreamWriter(const std::filesystem::path& path, const StreamHeader& h);
    ~StreamWriter();
    StreamWriter(const StreamWriter&) = delete;
    StreamWriter& operator=(const StreamWriter&) = delete;

    void keyframe(uint32_t tick, int32_t t_s, const FireStateView& v);
    void tick(uint32_t tick, int32_t t_s, uint64_t state_hash, const FireStateView& v, const TickOutput& out,
              const std::vector<OverlayItem>& overlay, const Metrics& m);
    void end(uint32_t ticks, uint64_t final_hash);
    uint64_t bytes_written() const { return bytes_; }

private:
    void u8(uint8_t v);
    void u16(uint16_t v);
    void u32(uint32_t v);
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void u64(uint64_t v);
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
    void str(const std::string& s);
    std::FILE* f_ = nullptr;
    uint64_t bytes_ = 0;
    bool ended_ = false;
};

}  // namespace embersim

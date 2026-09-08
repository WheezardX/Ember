#pragma once
// Fire Model Interface v1 — the C++ implementor contract (ADR 0008). Everything a model can
// see or say goes through these types. Keep this header engine-free and dependency-free.
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "version.h"

namespace embersim {

struct World;          // worldpack.h
class WeatherSampler;  // weather.h
struct ParamsPack;     // params.h

enum class Phase : uint8_t { Unburnable = 0, Unburned = 1, Burning = 2, Burned = 3 };

enum class DeltaKind : uint8_t {
    FuelRemoved = 1,
    RetardantApplied = 2,
    MoistureBumped = 3,
    IgnitionForced = 4,
    ExtinguishForced = 5,
};
inline constexpr uint8_t accepts_bit(DeltaKind k) { return static_cast<uint8_t>(1u << (static_cast<uint8_t>(k) - 1)); }
inline constexpr uint8_t ACCEPTS_ALL = 0x1F;
inline constexpr uint8_t ACCEPTS_NONE = 0;
const char* delta_kind_name(DeltaKind k);

enum class IgnitionCause : uint8_t { Scenario = 0, Burnout = 1, Spot = 2, Playback = 3, Other = 4 };

// One world mutation. `cells` are grid indices (y*nx+x) in the order the producer listed them.
struct Delta {
    DeltaKind kind{};
    std::vector<uint32_t> cells;
    uint16_t magnitude = 0;       // RetardantApplied: load permille; MoistureBumped: m10
    uint8_t decay_class = 0;      // RetardantApplied
    int32_t ttl_s = 0;            // MoistureBumped
    IgnitionCause cause = IgnitionCause::Other;  // IgnitionForced
    uint16_t resource_idx = 0xFFFF;              // who caused it (suppression sim), 0xFFFF = none
};

struct SpotEvent {
    uint32_t src = 0, dst = 0;
    int32_t launch_s = 0, land_s = 0;
    bool landed = false, ignited = false;
    uint64_t stream_key = 0;
};

struct RejectedDelta { DeltaKind kind; uint32_t count; };
struct Diag { std::string key; int64_t value; };

struct TickOutput {
    std::vector<uint32_t> dirty;          // sorted, unique cell indices whose state changed
    std::vector<SpotEvent> spots;         // launches this tick (landing may be later)
    std::vector<RejectedDelta> rejected;  // delta kinds the model does not accept
    std::vector<Diag> diag;
};

// Structure-of-arrays view of fire state, valid until the next advance(). `retardant` may be
// null for models that do not track it (observers treat null as zero coverage).
struct FireStateView {
    uint32_t nx = 0, ny = 0;
    const uint8_t* phase = nullptr;
    const uint8_t* intensity = nullptr;
    const int32_t* arrival_s = nullptr;
    const uint16_t* retardant = nullptr;
    size_t ncells() const { return static_cast<size_t>(nx) * ny; }
};

struct Caps {
    std::string interface_version = INTERFACE_VERSION;
    std::string model_id;
    std::string model_version;
    uint8_t accepts = ACCEPTS_NONE;  // bitmask over accepts_bit(kind)
    bool provides_intensity = false;
    bool provides_spotting = false;
    bool deterministic = true;
    bool supports_rewind = false;
    bool uses_weather = false;
    int32_t max_dt_s = 0;  // 0 = unbounded
    std::vector<std::string> rng_streams;
    bool accepts_kind(DeltaKind k) const { return (accepts & accepts_bit(k)) != 0; }
};

struct Seeds {
    uint64_t run_seed = 0;
};

class IFireModel {
public:
    virtual ~IFireModel() = default;
    virtual Caps caps() const = 0;
    // `weather` may be null when caps().uses_weather is false or the scenario has none.
    virtual void init(const World& world, const WeatherSampler* weather, const ParamsPack& params,
                      const Seeds& seeds, int32_t t_start_s) = 0;
    virtual TickOutput advance(int32_t dt_s, std::span<const Delta> deltas) = 0;
    virtual FireStateView state() const = 0;
    virtual int32_t now_s() const = 0;
    // Jump to an absolute time; only legal when caps().supports_rewind. Returns false otherwise.
    virtual bool rewind(int32_t /*t_s*/) { return false; }
    // XOR over all cells of cell_hash(i, phase, intensity, arrival_s) (ADR 0008 §3). Models may
    // override with a cheaper equivalent; the runner maintains it incrementally from dirty lists.
    virtual uint64_t state_hash() const;
};

// Registry. Known ids: "null", "arrival-playback", "ember-ca". Returns null for unknown ids.
std::unique_ptr<IFireModel> make_model(const std::string& id);
std::vector<std::string> model_ids();

// The canonical state hash: XOR over cells of cell_hash(...). Incrementally updatable — a cell
// change contributes cell_hash(old) ^ cell_hash(new) — so the runner keeps it in O(dirty).
uint64_t cell_hash(uint32_t index, uint8_t phase, uint8_t intensity, int32_t arrival_s);
uint64_t hash_state(const FireStateView& v);

}  // namespace embersim

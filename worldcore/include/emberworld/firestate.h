// Fire state player core (EPIC_5_PLAN D1): Epic 4 state streams (`.ess` v1) and replays.
//
// Implemented from the PUBLISHED format (docs/sim/formats.md §4-5 + implementor guide §4), never
// Epic 4's internals. A stream is a header, then records: KEYFRAME (RLE phase + intensity over
// all cells), TICK (dirty cells, spots, overlay, metrics, diag), END. Phases are monotone
// (0 unburnable, 1 unburned, 2 burning, 3 burned) and arrival_s is written exactly once per cell,
// so:
//   * the whole file is indexed once (record offsets, tick times, metrics) and every cell's
//     final arrival time collected;
//   * state at any time t = nearest keyframe at or before t, then the (<= keyframe_every) ticks
//     after it; "arrived by t" is simply final_arrival <= t.
// Seeking is O(cells + dirty since the keyframe), so scrubbing is cheap in both directions (D7:
// the player owns the clock, everything else follows it).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "emberworld/api.h"

namespace emberworld::fire {

enum Phase : uint8_t { Unburnable = 0, Unburned = 1, Burning = 2, Burned = 3 };

struct StreamHeader {
    uint32_t version = 0;
    uint32_t nx = 0, ny = 0, cell_mm = 0;
    int64_t t0_unix = 0;
    uint32_t dt_s = 0, keyframe_every = 0, flags = 0;
    std::string world_pack_sha256;  // hex
    std::string model_id, model_version, interface_version;
    size_t cells() const { return static_cast<size_t>(nx) * ny; }
};

struct Metrics {
    int32_t containment_permyriad = 0;
    uint32_t burning = 0, burned = 0, perimeter = 0;
    int32_t structures_lost = -1, structures_threatened = 0;
    int64_t cost_cents = 0;
    uint32_t busy_resources = 0;
    int32_t wind_u_cms = 0, wind_v_cms = 0, m10 = 0;
};

// World placement of the fire grid (from the replay's world.grid / the world pack).
struct GridGeo {
    double origin_x = 0, origin_y = 0;  // top-left corner, metres (region CRS)
    double cell_m = 30.0;
    std::string crs;
};

struct State {
    int32_t t_s = 0;              // seconds since t0 of the tick this state belongs to
    uint32_t tick = 0;
    std::vector<uint8_t> phase;
    std::vector<uint8_t> intensity;
    Metrics metrics;
};

class EMBERWORLD_CORE_API Stream {
public:
    // Loads and indexes the whole file. Empty string = ok, else the reason.
    std::string open(const std::string& path);

    const StreamHeader& header() const { return header_; }
    uint32_t ticks() const { return static_cast<uint32_t>(ticks_.size()); }
    int32_t start_s() const { return keyframes_.empty() ? 0 : keyframes_.front().t_s; }
    int32_t end_s() const { return ticks_.empty() ? start_s() : ticks_.back().t_s; }
    // Every cell's arrival time (seconds since t0), -1 = never, over the whole run.
    const std::vector<int32_t>& final_arrival() const { return arrival_; }

    // The state as of time t_s: the last tick whose t_s <= t (the initial keyframe before any).
    State at(int32_t t_s) const;

private:
    struct KeyRec { uint32_t tick; int32_t t_s; size_t offset; };   // offset of the RLE payload
    struct TickRec { uint32_t tick; int32_t t_s; size_t offset; Metrics metrics; };  // dirty list
    std::string parse();
    StreamHeader header_;
    std::vector<uint8_t> bytes_;
    std::vector<KeyRec> keyframes_;
    std::vector<TickRec> ticks_;
    std::vector<int32_t> arrival_;
};

struct ReplayInfo {
    std::string stream_path;   // resolved against the replay's directory
    std::string model_id;
    GridGeo grid;
    int64_t t0_unix = 0;
    std::string error;
    bool ok() const { return error.empty(); }
};

// Reads `<name>.replay.json` (formats.md §5): the stream it points to and its world grid.
EMBERWORLD_CORE_API ReplayInfo read_replay(const std::string& path);

// Local rate of spread (m/h) per cell from the arrival field (HCP4 H4-3, "where it's heading":
// the head is where the fire moves fastest). A least-squares plane through the cell and its
// arrived 3 x 3 neighbours gives the arrival gradient (s per cell); rate = cell / |gradient|,
// capped at max_mh (neighbours arriving on the same tick, e.g. a spot fire). 0 = never arrived
// or no arrived neighbour.
EMBERWORLD_CORE_API std::vector<float> spread_rate_mh(const std::vector<int32_t>& arrival, uint32_t nx,
                                                      uint32_t ny, double cell_m, float max_mh = 2000.f);

}  // namespace emberworld::fire

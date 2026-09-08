#pragma once
// Weather pack + per-tick sampler (ADR 0009 amendments A–C; spec §8). Integer only.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace embersim {

struct GridInfo;

inline constexpr int WX_VARS = 5;  // u, v, t2, rh2, precip — fixed order (formats.md §2)
enum WxVar : int { WX_U = 0, WX_V = 1, WX_T2 = 2, WX_RH2 = 3, WX_PRECIP = 4 };

struct WeatherPack {
    uint32_t nx = 0, ny = 0;
    double dx_m = 0, dy_m = 0, origin_x = 0, origin_y = 0;  // top-left corner, world crs
    int64_t t0_unix = 0;
    int32_t step_s = 3600;
    uint32_t num_steps = 0;
    std::vector<int16_t> data;         // [step][WX_VARS][ny][nx]
    std::vector<int32_t> held_from;    // per step: -1 or the index it was held from
    bool precip_present = false;
    std::string manifest_json;
    const int16_t* step_var(uint32_t step, int var) const {
        return data.data() + (static_cast<size_t>(step) * WX_VARS + var) * nx * ny;
    }
};

WeatherPack load_weather_pack(const std::filesystem::path& pack_dir, const std::string& manifest_name);

struct WeatherSample {
    int32_t u_cms = 0, v_cms = 0, t2_dk = 2981, rh2_dpct = 300, precip_cmm = 0;
};

class WeatherSampler {
public:
    static WeatherSampler constant(const WeatherSample& s);
    // world_t0_unix: the sim's t0 so t_s maps onto the pack's time axis. Throws if the world
    // starts before the timeline (ADR 0009 C).
    static WeatherSampler from_pack(const WeatherPack& pack, const GridInfo& world_grid, int64_t world_t0_unix);

    // Precompute the field for time t (called once per tick by the runner before models tick).
    void set_time(int32_t t_s);
    int32_t time() const { return t_s_; }

    uint32_t wcells() const { return wnx_ * wny_; }
    uint32_t wnx() const { return wnx_; }
    uint32_t wny() const { return wny_; }
    // Weather cell containing the centre of world cell (x, y) — nearest, no bilinear.
    uint32_t wcell_of(uint32_t x, uint32_t y) const { return wmap_.empty() ? 0 : wmap_[static_cast<size_t>(y) * world_nx_ + x]; }
    const WeatherSample& at_wcell(uint32_t w) const { return cur_[w]; }
    const WeatherSample& at_cell(uint32_t x, uint32_t y) const { return cur_[wcell_of(x, y)]; }
    WeatherSample mean() const;

    // True on the first tick at/after a step boundary; precip for that step is then applied.
    bool step_started() const { return step_started_; }
    int32_t current_step() const { return step_; }
    int32_t held_steps() const { return held_steps_; }
    int32_t held_tail_s() const { return held_tail_s_; }
    bool is_constant() const { return constant_; }

private:
    bool constant_ = true;
    const WeatherPack* pack_ = nullptr;
    uint32_t wnx_ = 1, wny_ = 1, world_nx_ = 0;
    std::vector<uint32_t> wmap_;  // world cell -> weather cell
    std::vector<WeatherSample> cur_;
    int64_t t_offset_s_ = 0;  // world t0 - weather t0
    int32_t t_s_ = INT32_MIN;
    int32_t step_ = -1, last_step_applied_ = -1;
    bool step_started_ = false;
    int32_t held_steps_ = 0, held_tail_s_ = 0;
};

}  // namespace embersim

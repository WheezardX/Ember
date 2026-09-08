#include "weather.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "../third_party/json.hpp"
#include "fixed.h"
#include "version.h"
#include "worldpack.h"

namespace embersim {

using json = nlohmann::json;
namespace fs = std::filesystem;

WeatherPack load_weather_pack(const fs::path& pack_dir, const std::string& manifest_name) {
    fs::path mpath = pack_dir / manifest_name;
    std::ifstream mf(mpath);
    if (!mf) throw std::runtime_error("weather pack: cannot open " + mpath.string());
    std::stringstream ss;
    ss << mf.rdbuf();
    WeatherPack p;
    p.manifest_json = ss.str();
    json m = json::parse(p.manifest_json);
    if (m.value("format", "") != "ember-weather-pack")
        throw std::runtime_error("weather pack: format is not ember-weather-pack");
    if (m.value("version", 0) != WEATHERPACK_VERSION)
        throw std::runtime_error("weather pack: unsupported version");
    const json& g = m.at("grid");
    p.nx = g.at("nx").get<uint32_t>();
    p.ny = g.at("ny").get<uint32_t>();
    p.dx_m = g.at("dx_m").get<double>();
    p.dy_m = g.at("dy_m").get<double>();
    p.origin_x = g.at("origin_x").get<double>();
    p.origin_y = g.at("origin_y").get<double>();
    p.t0_unix = m.at("t0_unix").get<int64_t>();
    p.step_s = m.at("step_s").get<int32_t>();
    p.num_steps = m.at("num_steps").get<uint32_t>();
    p.precip_present = m.value("precip_present", false);
    if (p.nx == 0 || p.ny == 0 || p.num_steps == 0 || p.step_s <= 0)
        throw std::runtime_error("weather pack: grid/steps must be positive");
    p.held_from.assign(p.num_steps, -1);
    if (m.contains("steps")) {
        for (const auto& s : m["steps"]) {
            uint32_t idx = s.value("index", 0u);
            if (idx < p.num_steps && s.contains("held_from") && !s["held_from"].is_null())
                p.held_from[idx] = s["held_from"].get<int32_t>();
        }
    }
    std::string file = m.value("file", "weather.bin");
    std::ifstream bf(pack_dir / file, std::ios::binary);
    if (!bf) throw std::runtime_error("weather pack: cannot open " + (pack_dir / file).string());
    size_t count = static_cast<size_t>(p.num_steps) * WX_VARS * p.nx * p.ny;
    std::vector<uint8_t> bytes(count * 2);
    bf.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<size_t>(bf.gcount()) != bytes.size())
        throw std::runtime_error("weather pack: weather.bin size mismatch (expected " + std::to_string(bytes.size()) + " bytes)");
    p.data.resize(count);
    for (size_t i = 0; i < count; ++i)
        p.data[i] = static_cast<int16_t>(static_cast<uint16_t>(bytes[2 * i]) | (static_cast<uint16_t>(bytes[2 * i + 1]) << 8));
    return p;
}

WeatherSampler WeatherSampler::constant(const WeatherSample& s) {
    WeatherSampler w;
    w.constant_ = true;
    w.wnx_ = w.wny_ = 1;
    w.cur_.assign(1, s);
    w.step_ = 0;
    return w;
}

WeatherSampler WeatherSampler::from_pack(const WeatherPack& pack, const GridInfo& grid, int64_t world_t0_unix) {
    WeatherSampler w;
    w.constant_ = false;
    w.pack_ = &pack;
    w.wnx_ = pack.nx;
    w.wny_ = pack.ny;
    w.world_nx_ = grid.nx;
    w.t_offset_s_ = world_t0_unix - pack.t0_unix;
    w.cur_.assign(static_cast<size_t>(pack.nx) * pack.ny, WeatherSample{});
    // Geometry: world cell centre -> weather cell (nearest). Done once in double here (init
    // precomputation, ADR 0008 §5) and quantised into an integer map.
    w.wmap_.resize(grid.ncells());
    for (uint32_t y = 0; y < grid.ny; ++y) {
        double cy = grid.origin_y - (y + 0.5) * grid.cell_size_m;  // y axis points south
        int64_t wy = static_cast<int64_t>((pack.origin_y - cy) / pack.dy_m);
        wy = clampi<int64_t>(wy, 0, static_cast<int64_t>(pack.ny) - 1);
        for (uint32_t x = 0; x < grid.nx; ++x) {
            double cx = grid.origin_x + (x + 0.5) * grid.cell_size_m;
            int64_t wx = static_cast<int64_t>((cx - pack.origin_x) / pack.dx_m);
            wx = clampi<int64_t>(wx, 0, static_cast<int64_t>(pack.nx) - 1);
            w.wmap_[static_cast<size_t>(y) * grid.nx + x] = static_cast<uint32_t>(wy * pack.nx + wx);
        }
    }
    return w;
}

void WeatherSampler::set_time(int32_t t_s) {
    if (constant_) {
        step_started_ = (t_s_ == INT32_MIN);  // apply "step 0" precip exactly once
        t_s_ = t_s;
        return;
    }
    t_s_ = t_s;
    int64_t tw = static_cast<int64_t>(t_s) + t_offset_s_;  // seconds on the weather axis
    if (tw < 0) throw std::runtime_error("weather: run starts before the timeline (ADR 0009 C)");
    int64_t step = tw / pack_->step_s;
    int64_t last = static_cast<int64_t>(pack_->num_steps) - 1;
    int64_t frac_q16 = 0;
    int64_t s0, s1;
    if (step >= last) {
        s0 = s1 = last;
        held_tail_s_ = static_cast<int32_t>(tw - last * pack_->step_s);
    } else {
        s0 = step;
        s1 = step + 1;
        frac_q16 = ((tw - step * pack_->step_s) * 65536) / pack_->step_s;
    }
    step_started_ = (static_cast<int32_t>(s0) != last_step_applied_);
    if (step_started_) {
        last_step_applied_ = static_cast<int32_t>(s0);
        if (pack_->held_from[static_cast<size_t>(s0)] >= 0) ++held_steps_;
    }
    step_ = static_cast<int32_t>(s0);
    const size_t n = static_cast<size_t>(wnx_) * wny_;
    const int16_t* a[WX_VARS];
    const int16_t* b[WX_VARS];
    for (int v = 0; v < WX_VARS; ++v) {
        a[v] = pack_->step_var(static_cast<uint32_t>(s0), v);
        b[v] = pack_->step_var(static_cast<uint32_t>(s1), v);
    }
    for (size_t i = 0; i < n; ++i) {
        WeatherSample& s = cur_[i];
        s.u_cms = lerp_q16(a[WX_U][i], b[WX_U][i], frac_q16);
        s.v_cms = lerp_q16(a[WX_V][i], b[WX_V][i], frac_q16);
        s.t2_dk = lerp_q16(a[WX_T2][i], b[WX_T2][i], frac_q16);
        s.rh2_dpct = lerp_q16(a[WX_RH2][i], b[WX_RH2][i], frac_q16);
        s.precip_cmm = a[WX_PRECIP][i];  // per-step accumulation, applied at step start
    }
}

WeatherSample WeatherSampler::mean() const {
    WeatherSample m{0, 0, 0, 0, 0};
    if (cur_.empty()) return m;
    int64_t u = 0, v = 0, t = 0, r = 0, p = 0;
    for (const auto& s : cur_) {
        u += s.u_cms; v += s.v_cms; t += s.t2_dk; r += s.rh2_dpct; p += s.precip_cmm;
    }
    int64_t n = static_cast<int64_t>(cur_.size());
    m.u_cms = static_cast<int32_t>(div_round(u, n));
    m.v_cms = static_cast<int32_t>(div_round(v, n));
    m.t2_dk = static_cast<int32_t>(div_round(t, n));
    m.rh2_dpct = static_cast<int32_t>(div_round(r, n));
    m.precip_cmm = static_cast<int32_t>(div_round(p, n));
    return m;
}

}  // namespace embersim

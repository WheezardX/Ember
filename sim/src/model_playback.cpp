// "arrival-playback" (story 4.3): replays world.arrival_s through the interface. Accepts no
// deltas (reported, never dropped), supports rewind, exposes the Epic 3 confidence raster as
// diagnostics. It is deliberately unable to be fought — that honesty is the point.
#include <algorithm>
#include <stdexcept>

#include "interface.h"
#include "models.h"
#include "params.h"
#include "worldpack.h"

namespace embersim {

namespace {

class PlaybackModel final : public IFireModel {
public:
    Caps caps() const override {
        Caps c;
        c.model_id = "arrival-playback";
        c.model_version = "1.0.0";
        c.accepts = ACCEPTS_NONE;
        c.provides_intensity = false;
        c.provides_spotting = false;
        c.deterministic = true;
        c.supports_rewind = true;
        c.uses_weather = false;
        c.max_dt_s = 0;
        return c;
    }

    void init(const World& world, const WeatherSampler*, const ParamsPack& params, const Seeds&, int32_t t_start_s) override {
        if (!world.has_arrival())
            throw std::runtime_error("arrival-playback: world pack '" + world.name + "' has no arrival_s layer");
        nx_ = world.grid.nx;
        ny_ = world.grid.ny;
        size_t n = world.ncells();
        residence_s_ = static_cast<int32_t>(params.get_int("playback.residence_s", 3600));
        if (residence_s_ <= 0) throw std::runtime_error("arrival-playback: playback.residence_s must be > 0");
        arrival_in_ = &world.arrival_s;
        confidence_ = world.has_confidence() ? &world.confidence : nullptr;
        burnable_.assign(n, 0);
        order_.clear();
        for (size_t i = 0; i < n; ++i) {
            int32_t a = world.arrival_s[i];
            // The raster is truth: a cell that burned is burnable regardless of the fuel layer.
            burnable_[i] = (a >= 0) || (world.elevation_cm[i] != ELEV_NODATA && burnable(world.fbfm40[i]));
            if (a >= 0) order_.push_back(static_cast<uint32_t>(i));
        }
        std::stable_sort(order_.begin(), order_.end(), [&](uint32_t x, uint32_t y) {
            return world.arrival_s[x] < world.arrival_s[y];
        });
        phase_.assign(n, 0);
        intensity_.assign(n, 0);
        arrival_.assign(n, -1);
        recompute(t_start_s);
    }

    TickOutput advance(int32_t dt_s, std::span<const Delta> deltas) override {
        TickOutput out;
        uint32_t rej[6] = {0, 0, 0, 0, 0, 0};
        for (const Delta& d : deltas) rej[static_cast<uint8_t>(d.kind)] += static_cast<uint32_t>(d.cells.size());
        for (uint8_t k = 1; k <= 5; ++k)
            if (rej[k]) out.rejected.push_back({static_cast<DeltaKind>(k), rej[k]});
        t_ += dt_s;
        const std::vector<int32_t>& A = *arrival_in_;
        while (ign_cursor_ < order_.size() && A[order_[ign_cursor_]] <= t_) {
            uint32_t i = order_[ign_cursor_++];
            phase_[i] = 2;
            intensity_[i] = 1;
            arrival_[i] = A[i];
            count_conf(i, +1);
            out.dirty.push_back(i);
        }
        while (burn_cursor_ < order_.size() && A[order_[burn_cursor_]] + residence_s_ <= t_) {
            uint32_t i = order_[burn_cursor_++];
            phase_[i] = 3;
            out.dirty.push_back(i);
        }
        std::sort(out.dirty.begin(), out.dirty.end());
        out.dirty.erase(std::unique(out.dirty.begin(), out.dirty.end()), out.dirty.end());
        diag(out);
        return out;
    }

    bool rewind(int32_t t_s) override {
        recompute(t_s);
        return true;
    }

    FireStateView state() const override {
        FireStateView v;
        v.nx = nx_;
        v.ny = ny_;
        v.phase = phase_.data();
        v.intensity = intensity_.data();
        v.arrival_s = arrival_.data();
        v.retardant = nullptr;
        return v;
    }
    int32_t now_s() const override { return t_; }

private:
    void count_conf(uint32_t i, int d) {
        if (!confidence_) return;
        uint8_t c = (*confidence_)[i];
        if (c >= 1 && c <= 3) conf_[c] += d;
    }
    void diag(TickOutput& out) const {
        out.diag.push_back({"confidence_class_1", conf_[1]});
        out.diag.push_back({"confidence_class_2", conf_[2]});
        out.diag.push_back({"confidence_class_3", conf_[3]});
        out.diag.push_back({"pending_cells", static_cast<int64_t>(order_.size() - ign_cursor_)});
    }
    void recompute(int32_t t) {
        t_ = t;
        const std::vector<int32_t>& A = *arrival_in_;
        conf_[1] = conf_[2] = conf_[3] = 0;
        for (size_t i = 0; i < phase_.size(); ++i) {
            int32_t a = A[i];
            if (a >= 0 && a <= t_) {
                phase_[i] = (a + residence_s_ <= t_) ? 3 : 2;
                intensity_[i] = 1;
                arrival_[i] = a;
                count_conf(static_cast<uint32_t>(i), +1);
            } else {
                phase_[i] = burnable_[i] ? 1 : 0;
                intensity_[i] = 0;
                arrival_[i] = -1;
            }
        }
        ign_cursor_ = 0;
        while (ign_cursor_ < order_.size() && A[order_[ign_cursor_]] <= t_) ++ign_cursor_;
        burn_cursor_ = 0;
        while (burn_cursor_ < order_.size() && A[order_[burn_cursor_]] + residence_s_ <= t_) ++burn_cursor_;
    }

    uint32_t nx_ = 0, ny_ = 0;
    int32_t t_ = 0;
    int32_t residence_s_ = 3600;
    const std::vector<int32_t>* arrival_in_ = nullptr;
    const std::vector<uint8_t>* confidence_ = nullptr;
    std::vector<uint8_t> burnable_, phase_, intensity_;
    std::vector<int32_t> arrival_;
    std::vector<uint32_t> order_;  // burned cells sorted by (arrival, index)
    size_t ign_cursor_ = 0, burn_cursor_ = 0;
    int64_t conf_[4] = {0, 0, 0, 0};
};

}  // namespace

std::unique_ptr<IFireModel> make_playback_model() { return std::make_unique<PlaybackModel>(); }

}  // namespace embersim

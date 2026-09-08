// "null" model: the smallest honest implementor. Never spreads. Accepts IgnitionForced and
// ExtinguishForced (so the conformance suite can exercise phase transitions) and rejects the
// rest. Reference for anyone implementing IFireModel against ADR 0008.
#include <algorithm>

#include "interface.h"
#include "models.h"
#include "params.h"
#include "worldpack.h"

namespace embersim {

namespace {

class NullModel final : public IFireModel {
public:
    Caps caps() const override {
        Caps c;
        c.model_id = "null";
        c.model_version = "1.0.0";
        c.accepts = accepts_bit(DeltaKind::IgnitionForced) | accepts_bit(DeltaKind::ExtinguishForced);
        c.provides_intensity = false;
        c.provides_spotting = false;
        c.deterministic = true;
        c.supports_rewind = false;
        c.uses_weather = false;
        c.max_dt_s = 0;
        return c;
    }

    void init(const World& world, const WeatherSampler*, const ParamsPack&, const Seeds&, int32_t t_start_s) override {
        nx_ = world.grid.nx;
        ny_ = world.grid.ny;
        size_t n = world.ncells();
        phase_.assign(n, 0);
        intensity_.assign(n, 0);
        arrival_.assign(n, -1);
        for (size_t i = 0; i < n; ++i)
            phase_[i] = (world.elevation_cm[i] != ELEV_NODATA && burnable(world.fbfm40[i])) ? 1 : 0;
        t_ = t_start_s;
    }

    TickOutput advance(int32_t dt_s, std::span<const Delta> deltas) override {
        TickOutput out;
        uint32_t rej[6] = {0, 0, 0, 0, 0, 0};
        for (const Delta& d : deltas) {
            if (d.kind == DeltaKind::IgnitionForced) {
                for (uint32_t c : d.cells) {
                    if (c < phase_.size() && phase_[c] == 1) {
                        phase_[c] = 2;
                        arrival_[c] = t_;
                        out.dirty.push_back(c);
                    }
                }
            } else if (d.kind == DeltaKind::ExtinguishForced) {
                for (uint32_t c : d.cells) {
                    if (c < phase_.size() && phase_[c] == 2) {
                        phase_[c] = 3;
                        out.dirty.push_back(c);
                    }
                }
            } else {
                rej[static_cast<uint8_t>(d.kind)] += static_cast<uint32_t>(d.cells.size());
            }
        }
        for (uint8_t k = 1; k <= 5; ++k)
            if (rej[k]) out.rejected.push_back({static_cast<DeltaKind>(k), rej[k]});
        std::sort(out.dirty.begin(), out.dirty.end());
        out.dirty.erase(std::unique(out.dirty.begin(), out.dirty.end()), out.dirty.end());
        t_ += dt_s;
        out.diag.push_back({"burning_cells", static_cast<int64_t>(std::count(phase_.begin(), phase_.end(), 2))});
        return out;
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
    uint32_t nx_ = 0, ny_ = 0;
    int32_t t_ = 0;
    std::vector<uint8_t> phase_, intensity_;
    std::vector<int32_t> arrival_;
};

}  // namespace

std::unique_ptr<IFireModel> make_null_model() { return std::make_unique<NullModel>(); }

}  // namespace embersim

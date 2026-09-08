// Suppression sim v1 (docs/sim/suppression.md §3–§4). Integer state, no random draws.
// Emits world deltas only; never touches a model.
#include "suppression.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>

#include "fixed.h"
#include "worldpack.h"

namespace embersim {

namespace {

// 1 chain = 20.1168 m. Rates are converted ONCE at init from the pack's chains/hour (double)
// to integer mm/s, rounded to nearest: mm_s = (int)(ch_per_h * 20116.8 / 3600 + 0.5), so the
// published 17 ch/h is 95 mm/s (truncation would give 94.996 -> 94). Init-time float,
// quantised before any tick (ADR 0008 §5).
int32_t chains_per_hour_to_mms(double ch) { return static_cast<int32_t>(ch * 20116.8 / 3600.0 + 0.5); }

constexpr int SLOPE_CLASSES = 4;
constexpr int FM_COUNT = 14;  // index 1..13

struct DozerRates {
    int32_t up[SLOPE_CLASSES] = {0, 0, 0, 0};
    int32_t down[SLOPE_CLASSES] = {0, 0, 0, 0};
};

struct TypeRates {
    bool is_hand = false, is_dozer = false, is_air = false;
    int32_t hand_mms[FM_COUNT] = {};                 // hand: per FM13
    std::map<std::string, DozerRates> dozer_groups;  // dozer: per group
    int64_t cost_per_hour_usd = 0;
    // air
    int32_t sortie_delay_s = 0, turnaround_s = 0;
    int32_t half_width[4] = {0, 0, 0, 0};    // by volume class 1..3
    int32_t retardant_load[4] = {0, 0, 0, 0};
    int32_t water_bump_m10[4] = {0, 0, 0, 0};
    int32_t water_ttl_s[4] = {0, 0, 0, 0};
    uint8_t decay_class = 1;
    int64_t cost_per_drop_usd = 0;
    int32_t mopup_cells_per_hour = 0;
};

struct Cell { int32_t x, y; };

enum class TaskKind { Line, AirDrop, Burnout, MopUp };

struct Task {
    TaskKind kind{};
    size_t cmd_index = 0;
    // Line / Burnout
    std::vector<uint32_t> cells;      // rasterised path, in order
    std::vector<uint8_t> diagonal;    // per cell: 1 if reached by a diagonal step
    size_t cursor = 0;                // next cell to reach
    int64_t progress_mm = 0;
    // AirDrop
    int32_t resolve_at_s = 0, busy_until_s = 0;
    bool dropped = false;
    Delta drop;                       // prepared delta
    // MopUp
    std::vector<uint32_t> band;       // candidate cells (sorted)
    int64_t acc = 0;                  // cells*3600 accumulator
    bool started = false;
};

struct Resource {
    ResourceSpec spec;
    const TypeRates* rates = nullptr;
    std::deque<size_t> queue;         // command indices
    std::optional<Task> task;
    ResourceStatus status;
    uint32_t drops = 0;
};

std::vector<Cell> bresenham(Cell a, Cell b) {
    std::vector<Cell> out;
    int32_t dx = std::abs(b.x - a.x), dy = -std::abs(b.y - a.y);
    int32_t sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
    int32_t err = dx + dy;
    Cell c = a;
    for (;;) {
        out.push_back(c);
        if (c.x == b.x && c.y == b.y) break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; c.x += sx; }
        if (e2 <= dx) { err += dx; c.y += sy; }
    }
    return out;
}

// Rasterise a polyline: Bresenham per segment, consecutive duplicates removed, path order.
std::vector<Cell> rasterise(const std::vector<std::pair<int32_t, int32_t>>& pts) {
    std::vector<Cell> out;
    if (pts.empty()) return out;
    if (pts.size() == 1) { out.push_back({pts[0].first, pts[0].second}); return out; }
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        auto seg = bresenham({pts[i].first, pts[i].second}, {pts[i + 1].first, pts[i + 1].second});
        for (const Cell& c : seg) {
            if (!out.empty() && out.back().x == c.x && out.back().y == c.y) continue;
            out.push_back(c);
        }
    }
    return out;
}

// Even-odd point-in-polygon on doubled coordinates (cell centres at 2x+1). Integer only.
bool point_in_polygon(int64_t px, int64_t py, const std::vector<std::pair<int32_t, int32_t>>& poly) {
    bool inside = false;
    size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        int64_t xi = 2LL * poly[i].first + 1, yi = 2LL * poly[i].second + 1;
        int64_t xj = 2LL * poly[j].first + 1, yj = 2LL * poly[j].second + 1;
        if ((yi > py) != (yj > py)) {
            // x of the edge at py: xi + (py - yi) * (xj - xi) / (yj - yi); compare px < that
            // without division: sign-aware cross multiplication.
            int64_t num = (py - yi) * (xj - xi);
            int64_t den = (yj - yi);
            // px < xi + num/den  <=>  (px - xi) * den < num  (if den > 0) else >
            int64_t lhs = (px - xi) * den;
            if (den > 0 ? (lhs < num) : (lhs > num)) inside = !inside;
        }
    }
    return inside;
}

// Squared distance (mm²) from point p to segment ab; all coordinates in mm.
int64_t dist2_point_segment(int64_t px, int64_t py, int64_t ax, int64_t ay, int64_t bx, int64_t by) {
    int64_t abx = bx - ax, aby = by - ay;
    int64_t apx = px - ax, apy = py - ay;
    int64_t len2 = abx * abx + aby * aby;
    int64_t qx, qy;
    if (len2 == 0) { qx = ax; qy = ay; }
    else {
        int64_t t = apx * abx + apy * aby;  // projection numerator
        if (t <= 0) { qx = ax; qy = ay; }
        else if (t >= len2) { qx = bx; qy = by; }
        else {
            qx = ax + div_round(abx * t, len2);
            qy = ay + div_round(aby * t, len2);
        }
    }
    int64_t dx = px - qx, dy = py - qy;
    return dx * dx + dy * dy;
}

}  // namespace

struct SuppressionSim::Impl {
    const World* world = nullptr;
    uint32_t nx = 0, ny = 0;
    int64_t cell_mm = 30000;
    std::vector<int32_t> gx, gy;  // Q8 rise/run per cell (central differences)
    std::map<std::string, TypeRates> types;
    std::map<uint8_t, int> fm13_of_code;  // FBFM40 code -> FM13 (0 = NB / instantaneous)
    int32_t burnout_mms = 0;
    std::vector<Command> commands;
    size_t next_cmd = 0;
    std::vector<Resource> resources;
    std::vector<ResourceStatus> status_cache;
    int64_t cost_cents = 0;

    // ---- init ------------------------------------------------------------------------
    void compute_gradients() {
        size_t n = static_cast<size_t>(nx) * ny;
        gx.assign(n, 0);
        gy.assign(n, 0);
        auto elev = [&](int64_t x, int64_t y, int32_t self) -> int64_t {
            if (x < 0 || y < 0 || x >= nx || y >= ny) return self;
            int32_t e = world->elevation_cm[static_cast<size_t>(y) * nx + x];
            return e == ELEV_NODATA ? self : e;
        };
        int64_t cell_cm = cell_mm / 10;
        for (uint32_t y = 0; y < ny; ++y)
            for (uint32_t x = 0; x < nx; ++x) {
                size_t i = static_cast<size_t>(y) * nx + x;
                int32_t self = world->elevation_cm[i];
                if (self == ELEV_NODATA) continue;
                int64_t ex1 = elev(static_cast<int64_t>(x) + 1, y, self), ex0 = elev(static_cast<int64_t>(x) - 1, y, self);
                int64_t ey1 = elev(x, static_cast<int64_t>(y) + 1, self), ey0 = elev(x, static_cast<int64_t>(y) - 1, self);
                int64_t spanx = (x + 1 < nx ? 1 : 0) + (x > 0 ? 1 : 0);
                int64_t spany = (y + 1 < ny ? 1 : 0) + (y > 0 ? 1 : 0);
                gx[i] = spanx ? static_cast<int32_t>((ex1 - ex0) * 256 / (spanx * cell_cm)) : 0;
                gy[i] = spany ? static_cast<int32_t>((ey1 - ey0) * 256 / (spany * cell_cm)) : 0;
            }
    }

    static double num(const toml::node* n, const std::string& what) {
        if (!n) throw std::runtime_error("production pack: missing " + what);
        if (auto d = n->value<double>()) return *d;
        if (auto i = n->value<int64_t>()) return static_cast<double>(*i);
        throw std::runtime_error("production pack: " + what + " is not a number");
    }

    void load_pack(const ParamsPack& pack) {
        // FM13 mapping.
        static const char* class_names[] = {"NB", "GR", "GS", "SH", "TU", "TL", "SB"};
        int class_fm[FUEL_CLASS_COUNT] = {0, 0, 0, 0, 0, 0, 0};
        for (int c = 1; c < FUEL_CLASS_COUNT; ++c)
            class_fm[c] = static_cast<int>(pack.get_int(std::string("fm13_from_fbfm40.") + class_names[c]));
        for (int code = 0; code < 256; ++code) {
            FuelClass fc = fuel_class_of(static_cast<uint8_t>(code));
            int fm = class_fm[static_cast<int>(fc)];
            std::string key = "fm13_overrides_by_fbfm40." + std::to_string(code);
            if (pack.has(key)) fm = static_cast<int>(pack.get_int(key));
            if (fm < 0 || fm >= FM_COUNT) throw std::runtime_error("production pack: bad FM13 for code " + std::to_string(code));
            fm13_of_code[static_cast<uint8_t>(code)] = fc == FuelClass::NB ? 0 : fm;
        }
        // Hand crews.
        for (const char* t : {"hand_t1", "hand_t2"}) {
            TypeRates r;
            r.is_hand = true;
            for (int fm = 1; fm < FM_COUNT; ++fm) {
                std::string key = std::string(t) + ".FM" + std::to_string(fm) + ".mid";
                r.hand_mms[fm] = chains_per_hour_to_mms(num(pack.find(key), key));
            }
            r.cost_per_hour_usd = pack.get_int(std::string(t) + ".cost_per_hour_usd", 0);
            r.mopup_cells_per_hour = static_cast<int32_t>(pack.get_int(std::string("mop_up.cells_per_hour.") + t, 0));
            types[t] = r;
        }
        // Dozers.
        for (const char* t : {"dozer_t1", "dozer_t2", "dozer_t3"}) {
            TypeRates r;
            r.is_dozer = true;
            const toml::node* tn = pack.find(t);
            if (!tn || !tn->as_table()) throw std::runtime_error(std::string("production pack: missing [") + t + "]");
            for (auto&& [k, v] : *tn->as_table()) {
                std::string key(k.str());
                if (key.rfind("g", 0) != 0) continue;
                const toml::table* g = v.as_table();
                if (!g) continue;
                DozerRates dr;
                for (int dir = 0; dir < 2; ++dir) {
                    const toml::array* arr = g->get(dir == 0 ? "up" : "down") ? g->get(dir == 0 ? "up" : "down")->as_array() : nullptr;
                    if (!arr || arr->size() != SLOPE_CLASSES)
                        throw std::runtime_error(std::string("production pack: ") + t + "." + key + " needs 4 slope classes");
                    for (int sc = 0; sc < SLOPE_CLASSES; ++sc) {
                        const toml::array* pr = arr->get(sc)->as_array();
                        if (!pr || pr->size() != 2) throw std::runtime_error("production pack: bad dozer range");
                        double lo = num(pr->get(0), "lo"), hi = num(pr->get(1), "hi");
                        int32_t mms = chains_per_hour_to_mms((lo + hi) / 2.0);
                        (dir == 0 ? dr.up : dr.down)[sc] = mms;
                    }
                }
                r.dozer_groups[key] = dr;
            }
            r.cost_per_hour_usd = pack.get_int(std::string(t) + ".cost_per_hour_usd", 0);
            r.mopup_cells_per_hour = static_cast<int32_t>(pack.get_int(std::string("mop_up.cells_per_hour.") + t, 0));
            types[t] = r;
        }
        // dozer group per FM13, stored as a name per FM in a side map.
        for (int fm = 1; fm < FM_COUNT; ++fm) {
            std::string key = "dozer_groups.FM" + std::to_string(fm);
            dozer_group_of_fm[fm] = pack.get_str(key, "");
            if (dozer_group_of_fm[fm].empty()) throw std::runtime_error("production pack: missing " + key);
        }
        // Air.
        for (const char* t : {"airtanker_large", "airtanker_seat", "helicopter_bucket"}) {
            TypeRates r;
            r.is_air = true;
            std::string p(t);
            r.sortie_delay_s = static_cast<int32_t>(pack.get_int(p + ".sortie_delay_s"));
            r.turnaround_s = static_cast<int32_t>(pack.get_int(p + ".turnaround_s"));
            for (int vc = 1; vc <= 3; ++vc) {
                std::string s = std::to_string(vc);
                r.half_width[vc] = static_cast<int32_t>(pack.get_int(p + ".footprint_half_width_cells." + s));
                r.retardant_load[vc] = static_cast<int32_t>(pack.get_int(p + ".retardant_load_permille." + s, 0));
                r.water_bump_m10[vc] = static_cast<int32_t>(pack.get_int(p + ".water_bump_m10." + s, 0));
                r.water_ttl_s[vc] = static_cast<int32_t>(pack.get_int(p + ".water_ttl_s." + s, 0));
            }
            r.decay_class = static_cast<uint8_t>(pack.get_int(p + ".decay_class", 1));
            r.cost_per_drop_usd = pack.get_int(p + ".cost_per_drop_usd", 0);
            types[t] = r;
        }
        burnout_mms = chains_per_hour_to_mms(num(pack.find("burnout.firing_rate_chains_per_hour"), "burnout.firing_rate_chains_per_hour"));
    }
    std::string dozer_group_of_fm[FM_COUNT];

    uint32_t idx(int32_t x, int32_t y) const { return static_cast<uint32_t>(y) * nx + static_cast<uint32_t>(x); }
    bool on_grid(int32_t x, int32_t y) const { return x >= 0 && y >= 0 && x < static_cast<int32_t>(nx) && y < static_cast<int32_t>(ny); }

    int slope_class(uint32_t cell) const {
        int64_t g = isqrt64(static_cast<int64_t>(gx[cell]) * gx[cell] + static_cast<int64_t>(gy[cell]) * gy[cell]);
        int64_t pct = g * 100 / 256;
        if (pct <= 25) return 0;
        if (pct <= 40) return 1;
        if (pct <= 55) return 2;
        if (pct <= 74) return 3;
        return -1;  // impassable
    }

    // Production rate (mm/s) for a resource entering `cell` from `prev` (UINT32_MAX = none).
    // 0 = impassable/stalled; -1 = instantaneous (non-burnable cell: nothing to cut).
    int32_t line_rate(const Resource& r, uint32_t cell, uint32_t prev) const {
        int fm = fm13_of_code.at(world->fbfm40[cell]);
        if (fm == 0) return -1;
        if (r.rates->is_hand) return r.rates->hand_mms[fm];
        if (r.rates->is_dozer) {
            int sc = slope_class(cell);
            if (sc < 0) return 0;
            auto it = r.rates->dozer_groups.find(dozer_group_of_fm[fm]);
            if (it == r.rates->dozer_groups.end()) return 0;
            // Up/down from the elevation change along the step; flat counts as "up"
            // (the conservative, lower rate).
            bool down = false;
            if (prev != UINT32_MAX) {
                int32_t e0 = world->elevation_cm[prev], e1 = world->elevation_cm[cell];
                if (e0 != ELEV_NODATA && e1 != ELEV_NODATA && e1 < e0) down = true;
            }
            return down ? it->second.down[sc] : it->second.up[sc];
        }
        return 0;
    }

    void build_task(Resource& r, size_t ci) {
        const Command& c = commands[ci];
        Task t;
        t.cmd_index = ci;
        if (c.kind == "cut_line" || c.kind == "burnout") {
            t.kind = c.kind == "cut_line" ? TaskKind::Line : TaskKind::Burnout;
            auto cells = rasterise(c.points);
            Cell prev{cells.front().x, cells.front().y};
            for (size_t i = 0; i < cells.size(); ++i) {
                t.cells.push_back(idx(cells[i].x, cells[i].y));
                bool diag = i > 0 && cells[i].x != prev.x && cells[i].y != prev.y;
                t.diagonal.push_back(diag ? 1 : 0);
                prev = cells[i];
            }
        } else if (c.kind == "air_drop") {
            t.kind = TaskKind::AirDrop;
            const TypeRates& tr = *r.rates;
            int vc = clampi(c.volume_class, 1, 3);
            int32_t h = tr.half_width[vc];
            auto seg = rasterise(c.points);
            std::vector<uint32_t> cells;
            for (const Cell& s : seg)
                for (int32_t dy = -h; dy <= h; ++dy)
                    for (int32_t dx = -h; dx <= h; ++dx)
                        if (on_grid(s.x + dx, s.y + dy)) cells.push_back(idx(s.x + dx, s.y + dy));
            std::sort(cells.begin(), cells.end());
            cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
            t.drop.cells = std::move(cells);
            if (c.agent == "retardant") {
                t.drop.kind = DeltaKind::RetardantApplied;
                t.drop.magnitude = static_cast<uint16_t>(tr.retardant_load[vc]);
                t.drop.decay_class = tr.decay_class;
            } else {
                t.drop.kind = DeltaKind::MoistureBumped;
                t.drop.magnitude = static_cast<uint16_t>(tr.water_bump_m10[vc]);
                t.drop.ttl_s = tr.water_ttl_s[vc];
            }
        } else if (c.kind == "mop_up") {
            t.kind = TaskKind::MopUp;
            int64_t depth2 = static_cast<int64_t>(c.depth_m) * 1000;
            depth2 *= depth2;
            // Bounding box of the polygon to limit the scan.
            int32_t minx = INT32_MAX, miny = INT32_MAX, maxx = INT32_MIN, maxy = INT32_MIN;
            for (auto& p : c.points) { minx = std::min(minx, p.first); maxx = std::max(maxx, p.first); miny = std::min(miny, p.second); maxy = std::max(maxy, p.second); }
            minx = std::max(minx, 0); miny = std::max(miny, 0);
            maxx = std::min(maxx, static_cast<int32_t>(nx) - 1); maxy = std::min(maxy, static_cast<int32_t>(ny) - 1);
            for (int32_t y = miny; y <= maxy; ++y)
                for (int32_t x = minx; x <= maxx; ++x) {
                    if (!point_in_polygon(2LL * x + 1, 2LL * y + 1, c.points)) continue;
                    // distance from the cell centre to the nearest polygon edge, in mm
                    int64_t px = (2LL * x + 1) * cell_mm / 2, py = (2LL * y + 1) * cell_mm / 2;
                    int64_t best = INT64_MAX;
                    size_t n = c.points.size();
                    for (size_t i = 0, j = n - 1; i < n; j = i++) {
                        int64_t ax = (2LL * c.points[j].first + 1) * cell_mm / 2, ay = (2LL * c.points[j].second + 1) * cell_mm / 2;
                        int64_t bx = (2LL * c.points[i].first + 1) * cell_mm / 2, by = (2LL * c.points[i].second + 1) * cell_mm / 2;
                        best = std::min(best, dist2_point_segment(px, py, ax, ay, bx, by));
                    }
                    if (best <= depth2) t.band.push_back(idx(x, y));
                }
            std::sort(t.band.begin(), t.band.end());
        }
        r.task = std::move(t);
    }

    // ---- tick ------------------------------------------------------------------------
    void advance_walk(Resource& r, Task& t, int32_t t_s, int32_t dt_s, bool burnout, std::vector<uint32_t>& reached) {
        (void)t_s;
        int64_t remaining = dt_s;
        int64_t spent = 0;
        r.status.stalled = false;
        while (remaining > 0 && t.cursor < t.cells.size()) {
            uint32_t cell = t.cells[t.cursor];
            uint32_t prev = t.cursor > 0 ? t.cells[t.cursor - 1] : UINT32_MAX;
            int32_t rate = burnout ? burnout_mms : line_rate(r, cell, prev);
            if (rate == 0) { r.status.stalled = true; break; }
            int64_t step_len = t.diagonal[t.cursor] ? cell_mm * 1414 / 1000 : cell_mm;
            if (rate < 0) step_len = 0;  // non-burnable: nothing to cut
            int64_t need = step_len - t.progress_mm;
            if (need <= 0) { reached.push_back(cell); ++t.cursor; t.progress_mm = 0; continue; }
            int64_t time_needed = (need + rate - 1) / rate;  // ceil
            if (time_needed <= remaining) {
                remaining -= time_needed;
                spent += time_needed;
                reached.push_back(cell);
                ++t.cursor;
                t.progress_mm = 0;
            } else {
                t.progress_mm += static_cast<int64_t>(rate) * remaining;
                spent += remaining;
                remaining = 0;
            }
        }
        r.status.busy_s += spent;
    }

    std::vector<Delta> tick(int32_t t_s, int32_t dt_s, const FireStateView& fire) {
        // 1. Activate commands whose time has come.
        while (next_cmd < commands.size() && commands[next_cmd].t_s <= t_s) {
            const Command& c = commands[next_cmd];
            for (Resource& r : resources)
                if (r.spec.id == c.resource_id) { r.queue.push_back(next_cmd); break; }
            ++next_cmd;
        }
        std::vector<Delta> out;
        for (size_t ri = 0; ri < resources.size(); ++ri) {
            Resource& r = resources[ri];
            // 2. Start the next queued task if idle.
            if (!r.task && !r.queue.empty()) {
                size_t ci = r.queue.front();
                r.queue.pop_front();
                build_task(r, ci);
                Task& t = *r.task;
                if (t.kind == TaskKind::AirDrop) {
                    t.resolve_at_s = t_s + r.rates->sortie_delay_s;
                    t.busy_until_s = t.resolve_at_s + r.rates->turnaround_s;
                }
            }
            std::vector<uint32_t> removed, ignited, extinguished;
            std::optional<Delta> drop;
            if (r.task) {
                Task& t = *r.task;
                bool done = false;
                switch (t.kind) {
                    case TaskKind::Line:
                        advance_walk(r, t, t_s, dt_s, false, removed);
                        done = t.cursor >= t.cells.size();
                        break;
                    case TaskKind::Burnout:
                        advance_walk(r, t, t_s, dt_s, true, ignited);
                        done = t.cursor >= t.cells.size();
                        break;
                    case TaskKind::AirDrop:
                        r.status.busy_s += dt_s;
                        if (!t.dropped && t_s >= t.resolve_at_s) {
                            t.dropped = true;
                            drop = t.drop;
                            ++r.drops;
                            cost_cents += r.rates->cost_per_drop_usd * 100;
                        }
                        done = t.dropped && t_s + dt_s >= t.busy_until_s;
                        break;
                    case TaskKind::MopUp: {
                        r.status.busy_s += dt_s;
                        t.acc += static_cast<int64_t>(r.rates->mopup_cells_per_hour) * dt_s;
                        int64_t budget = t.acc / 3600;
                        t.acc -= budget * 3600;
                        size_t burning_in_band = 0;
                        for (uint32_t c : t.band) {
                            if (fire.phase[c] != 2) continue;
                            ++burning_in_band;
                            if (static_cast<int64_t>(extinguished.size()) < budget) extinguished.push_back(c);
                        }
                        t.started = true;
                        done = burning_in_band == 0;
                        break;
                    }
                }
                if (done) {
                    r.task.reset();
                    ++r.status.completed;
                    r.status.stalled = false;
                }
            }
            // 3. Emit in canonical kind order 1..5.
            auto emit = [&](DeltaKind k, std::vector<uint32_t>&& cells) {
                if (cells.empty()) return;
                Delta d;
                d.kind = k;
                d.cells = std::move(cells);
                d.resource_idx = static_cast<uint16_t>(ri);
                if (k == DeltaKind::IgnitionForced) d.cause = IgnitionCause::Burnout;
                out.push_back(std::move(d));
            };
            emit(DeltaKind::FuelRemoved, std::move(removed));
            if (drop && drop->kind == DeltaKind::RetardantApplied) { drop->resource_idx = static_cast<uint16_t>(ri); out.push_back(*drop); }
            if (drop && drop->kind == DeltaKind::MoistureBumped) { drop->resource_idx = static_cast<uint16_t>(ri); out.push_back(*drop); }
            emit(DeltaKind::IgnitionForced, std::move(ignited));
            emit(DeltaKind::ExtinguishForced, std::move(extinguished));
            r.status.busy = r.task.has_value();
            r.status.queued = static_cast<uint32_t>(r.queue.size());
        }
        return out;
    }

    void refresh_status() {
        status_cache.clear();
        for (const Resource& r : resources) status_cache.push_back(r.status);
    }
    int64_t total_cost() const {
        int64_t c = cost_cents;
        for (const Resource& r : resources) c += r.status.busy_s * r.rates->cost_per_hour_usd * 100 / 3600;
        return c;
    }
};

SuppressionSim::SuppressionSim() : impl_(std::make_unique<Impl>()) {}
SuppressionSim::~SuppressionSim() = default;
SuppressionSim::SuppressionSim(SuppressionSim&&) noexcept = default;
SuppressionSim& SuppressionSim::operator=(SuppressionSim&&) noexcept = default;

void SuppressionSim::init(const World& world, const ParamsPack& production_pack, const std::vector<ResourceSpec>& resources,
                          const std::vector<Command>& commands, uint64_t /*run_seed*/) {
    Impl& im = *impl_;
    im.world = &world;
    im.nx = world.grid.nx;
    im.ny = world.grid.ny;
    im.cell_mm = world.grid.cell_mm;
    im.compute_gradients();
    im.load_pack(production_pack);
    im.resources.clear();
    for (const ResourceSpec& rs : resources) {
        auto it = im.types.find(rs.type);
        if (it == im.types.end()) throw std::runtime_error("suppression: resource '" + rs.id + "' has unknown type '" + rs.type + "'");
        Resource r;
        r.spec = rs;
        r.rates = &it->second;
        r.status.id = rs.id;
        r.status.type = rs.type;
        im.resources.push_back(std::move(r));
    }
    im.commands = commands;
    im.next_cmd = 0;
    im.cost_cents = 0;
    // Validate every command now so a bad script fails at load, not mid-run.
    for (size_t i = 0; i < im.commands.size(); ++i) {
        const Command& c = im.commands[i];
        std::ostringstream where;
        where << "suppression: command #" << i << " (" << c.kind << " @ t=" << c.t_s << ", resource '" << c.resource_id << "')";
        if (c.kind == "hold") throw std::runtime_error(where.str() + ": 'hold' is reserved in v1");
        const Resource* r = nullptr;
        for (const Resource& rr : im.resources) if (rr.spec.id == c.resource_id) { r = &rr; break; }
        if (!r) throw std::runtime_error(where.str() + ": unknown resource id");
        for (auto& p : c.points)
            if (!im.on_grid(p.first, p.second))
                throw std::runtime_error(where.str() + ": point (" + std::to_string(p.first) + ", " + std::to_string(p.second) + ") is off-grid");
        if (c.kind == "cut_line") {
            if (c.points.size() < 2) throw std::runtime_error(where.str() + ": path needs >= 2 points");
            bool hand = c.method == "hand", dozer = c.method == "dozer";
            if (!hand && !dozer) throw std::runtime_error(where.str() + ": method must be hand|dozer");
            if ((hand && !r->rates->is_hand) || (dozer && !r->rates->is_dozer))
                throw std::runtime_error(where.str() + ": method '" + c.method + "' does not match resource type '" + r->spec.type + "'");
        } else if (c.kind == "air_drop") {
            if (!r->rates->is_air) throw std::runtime_error(where.str() + ": resource is not an air resource");
            if (c.points.empty() || c.points.size() > 2) throw std::runtime_error(where.str() + ": target needs 1 or 2 points");
            if (c.agent != "retardant" && c.agent != "water") throw std::runtime_error(where.str() + ": agent must be retardant|water");
            if (c.agent == "retardant" && r->rates->retardant_load[clampi(c.volume_class, 1, 3)] == 0)
                throw std::runtime_error(where.str() + ": resource type '" + r->spec.type + "' carries no retardant");
            if (c.agent == "water" && r->rates->water_bump_m10[clampi(c.volume_class, 1, 3)] == 0)
                throw std::runtime_error(where.str() + ": resource type '" + r->spec.type + "' carries no water");
        } else if (c.kind == "burnout") {
            if (c.points.size() < 2) throw std::runtime_error(where.str() + ": anchor_path needs >= 2 points");
            if (!r->rates->is_hand && !r->rates->is_dozer) throw std::runtime_error(where.str() + ": burnout needs a ground resource");
            if (c.firing_pattern != "strip") throw std::runtime_error(where.str() + ": firing_pattern must be strip (v1)");
        } else if (c.kind == "mop_up") {
            if (c.points.size() < 3) throw std::runtime_error(where.str() + ": region needs >= 3 points");
            if (r->rates->mopup_cells_per_hour <= 0)
                throw std::runtime_error(where.str() + ": resource type '" + r->spec.type + "' cannot mop up (rate 0)");
        } else {
            throw std::runtime_error(where.str() + ": unknown command kind");
        }
    }
    im.refresh_status();
}

std::vector<Delta> SuppressionSim::tick(int32_t t_s, int32_t dt_s, const FireStateView& fire) {
    std::vector<Delta> out = impl_->tick(t_s, dt_s, fire);
    impl_->refresh_status();
    return out;
}

const std::vector<ResourceStatus>& SuppressionSim::status() const { return impl_->status_cache; }
int64_t SuppressionSim::cost_cents() const { return impl_->total_cost(); }
uint32_t SuppressionSim::busy_count() const {
    uint32_t n = 0;
    for (const Resource& r : impl_->resources) n += r.task.has_value() ? 1 : 0;
    return n;
}
uint32_t SuppressionSim::pending_commands() const {
    uint32_t n = static_cast<uint32_t>(impl_->commands.size() - impl_->next_cmd);
    for (const Resource& r : impl_->resources) n += static_cast<uint32_t>(r.queue.size());
    return n;
}

}  // namespace embersim

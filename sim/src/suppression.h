#pragma once
// Suppression sim v1 (docs/sim/suppression.md §3). Produces world deltas only; never touches a
// model. Integer state, no random draws in v1 (SYS_SUPPRESSION reserved).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "interface.h"
#include "params.h"
#include "scenario.h"

namespace embersim {

struct World;

struct ResourceStatus {
    std::string id;
    std::string type;
    bool busy = false;
    bool stalled = false;     // current task cannot progress (e.g. dozer on > 74 % slope)
    int64_t busy_s = 0;       // accumulated busy seconds (cost basis)
    uint32_t queued = 0;      // commands waiting behind the current task
    uint32_t completed = 0;
};

class SuppressionSim {
public:
    SuppressionSim();
    ~SuppressionSim();
    SuppressionSim(SuppressionSim&&) noexcept;
    SuppressionSim& operator=(SuppressionSim&&) noexcept;

    // `commands` must already be sorted by (t_s, order) and validated against the world.
    void init(const World& world, const ParamsPack& production_pack, const std::vector<ResourceSpec>& resources,
              const std::vector<Command>& commands, uint64_t run_seed);

    // Advance all resources over [t_s, t_s + dt_s). Commands with t_s <= t become active.
    // `fire` is the fire state at the START of the tick (mop-up needs burning cells). Returned
    // deltas are in canonical order: resources in scenario order, then delta kind order.
    std::vector<Delta> tick(int32_t t_s, int32_t dt_s, const FireStateView& fire);

    const std::vector<ResourceStatus>& status() const;
    int64_t cost_cents() const;
    uint32_t busy_count() const;
    uint32_t pending_commands() const;  // not yet started

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace embersim

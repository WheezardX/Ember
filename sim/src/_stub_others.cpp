// STUB — deleted at merge. Stands in for model_ca.cpp (workstream D), suppression.cpp and
// observers.cpp (workstream E) so the runner/CLI link in this worktree. Nothing here is real.
#include <vector>

#include "interface.h"
#include "models.h"
#include "observers.h"
#include "suppression.h"
#include "worldpack.h"

namespace embersim {

std::unique_ptr<IFireModel> make_ca_model() { return nullptr; }  // STUB: CA not in this build

struct SuppressionSim::Impl {
    std::vector<ResourceStatus> status;
};
SuppressionSim::SuppressionSim() : impl_(std::make_unique<Impl>()) {}
SuppressionSim::~SuppressionSim() = default;
SuppressionSim::SuppressionSim(SuppressionSim&&) noexcept = default;
SuppressionSim& SuppressionSim::operator=(SuppressionSim&&) noexcept = default;

void SuppressionSim::init(const World&, const ParamsPack&, const std::vector<ResourceSpec>& resources,
                          const std::vector<Command>&, uint64_t) {
    impl_->status.clear();
    for (const auto& r : resources) impl_->status.push_back({r.id, r.type, false, false, 0, 0, 0});
}
std::vector<Delta> SuppressionSim::tick(int32_t, int32_t, const FireStateView&) { return {}; }  // STUB
const std::vector<ResourceStatus>& SuppressionSim::status() const { return impl_->status; }
int64_t SuppressionSim::cost_cents() const { return 0; }
uint32_t SuppressionSim::busy_count() const { return 0; }
uint32_t SuppressionSim::pending_commands() const { return 0; }

Metrics compute_metrics(const FireStateView& fire, const World&, uint16_t) {  // STUB: counts only
    Metrics m;
    for (size_t i = 0; i < fire.ncells(); ++i) {
        if (fire.phase[i] == 2) ++m.burning;
        else if (fire.phase[i] == 3) ++m.burned;
    }
    return m;
}

}  // namespace embersim

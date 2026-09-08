#include "interface.h"

#include "hash.h"
#include "models.h"

namespace embersim {

const char* delta_kind_name(DeltaKind k) {
    switch (k) {
        case DeltaKind::FuelRemoved: return "FuelRemoved";
        case DeltaKind::RetardantApplied: return "RetardantApplied";
        case DeltaKind::MoistureBumped: return "MoistureBumped";
        case DeltaKind::IgnitionForced: return "IgnitionForced";
        case DeltaKind::ExtinguishForced: return "ExtinguishForced";
    }
    return "?";
}

uint64_t hash_state(const FireStateView& v) {
    size_t n = v.ncells();
    uint64_t h = FNV_OFFSET;
    h = fnv1a_array(v.phase, n, h);
    h = fnv1a_array(v.intensity, n, h);
    h = fnv1a_array(v.arrival_s, n, h);
    return h;
}

uint64_t IFireModel::state_hash() const { return hash_state(state()); }

std::unique_ptr<IFireModel> make_model(const std::string& id) {
    if (id == "null") return make_null_model();
    if (id == "arrival-playback") return make_playback_model();
    if (id == "ember-ca") return make_ca_model();
    return nullptr;
}

std::vector<std::string> model_ids() { return {"null", "arrival-playback", "ember-ca"}; }

}  // namespace embersim

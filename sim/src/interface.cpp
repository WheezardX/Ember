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

uint64_t cell_hash(uint32_t index, uint8_t phase, uint8_t intensity, int32_t arrival_s) {
    return hash64(static_cast<uint64_t>(index), static_cast<uint64_t>(phase), static_cast<uint64_t>(intensity),
                  static_cast<uint64_t>(static_cast<uint32_t>(arrival_s)));
}

uint64_t hash_state(const FireStateView& v) {
    size_t n = v.ncells();
    uint64_t h = 0;
    for (size_t i = 0; i < n; ++i) h ^= cell_hash(static_cast<uint32_t>(i), v.phase[i], v.intensity[i], v.arrival_s[i]);
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

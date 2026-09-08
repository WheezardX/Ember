#pragma once
// Factories for the built-in implementors of IFireModel. Each lives in its own .cpp:
//   model_null.cpp      "null"              never spreads; accepts Ignition/Extinguish only
//   model_playback.cpp  "arrival-playback"  replays world.arrival_s; accepts nothing; rewinds
//   model_ca.cpp        "ember-ca"          docs/sim/default-model-spec.md
#include <memory>

#include "interface.h"

namespace embersim {
std::unique_ptr<IFireModel> make_null_model();
std::unique_ptr<IFireModel> make_playback_model();
std::unique_ptr<IFireModel> make_ca_model();
}  // namespace embersim

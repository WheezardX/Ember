// LINK STUB for the E-workstream worktree only: the CA and playback models are written by other
// agents. Deleted at merge.
#include "models.h"

namespace embersim {
std::unique_ptr<IFireModel> make_playback_model() { return nullptr; }
std::unique_ptr<IFireModel> make_ca_model() { return nullptr; }
}  // namespace embersim

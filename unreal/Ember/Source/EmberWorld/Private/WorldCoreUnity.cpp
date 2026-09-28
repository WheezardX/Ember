// Compiles the engine-free worldcore sources (<repo>/worldcore/src) into this module.
// Edit them there; worldcore_tests covers them outside the engine.
#include "CoreMinimal.h"

THIRD_PARTY_INCLUDES_START
#include "src/tiff.cpp"
#include "src/region.cpp"
#include "src/heightfield.cpp"
THIRD_PARTY_INCLUDES_END

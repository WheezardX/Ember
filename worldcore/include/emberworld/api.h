// Symbol export for the engine-free core. Empty in the CMake build; inside UE the EmberWorld
// module defines EMBERWORLD_CORE_API=EMBERWORLD_API so other modules (DLLs) can link to it.
#pragma once
#ifndef EMBERWORLD_CORE_API
#define EMBERWORLD_CORE_API
#endif

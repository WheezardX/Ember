// embersim CLI — placeholder until the runner workstream lands (run / replay / info / models / version).
#include <cstdio>

#include "version.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    std::printf("embersim %s (interface %s) — runner not built yet\n", embersim::EMBERSIM_VERSION,
                embersim::INTERFACE_VERSION);
    return 0;
}

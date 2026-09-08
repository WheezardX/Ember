"""ember.sim — Python tooling around the C++ sim core (Epic 4, plan decision D1).

The core (`sim/`, `embersim`) is engine-free and reads only flat **world packs**; Python
owns everything geospatial: exporting Epic 1-3 stores/bundles into packs (`worldpack`,
`weatherpack`), the synthetic-world kit, reading state streams (`stream`), and the
disposable 2D debug renderer (`render`). Formats: docs/sim/formats.md; contract: adr/0008.
"""

WORLD_PACK_FORMAT = "ember-world-pack"
WORLD_PACK_VERSION = 1
WEATHER_PACK_FORMAT = "ember-weather-pack"
WEATHER_PACK_VERSION = 1
STREAM_VERSION = 1

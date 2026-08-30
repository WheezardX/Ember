"""ember — the wildfire product (Project Ember), built ON TOP OF the `terrain`
world-data engine.

Ember consumes terrain: it calls `terrain.run_pipeline` to bake terrain/fuels/veg
for an incident's AOI, then adds the wildfire-specific layers (fire data, weather,
progression, and later the sim). The dependency is STRICTLY one-way — `ember`
imports `terrain`, `terrain` never imports `ember` (enforced by a test). That keeps
`terrain` a standalone engine (also the "model your land" product) and makes
extracting `ember` into its own repo cheap. See adr/0005.

Epic 3 (fire data) lives in `ember.incidents`; Epics 4-6 (sim/render/game) land here too.
"""

__version__ = "0.0.1"

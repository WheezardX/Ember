# ADR 0005 — Split the project at the Terrain / Ember boundary

**Status:** Accepted (2026-08-29). Supersedes the "not a new repo" note in EPIC_3_PLAN.md.

## Context
Epics 1–2 built the shared **World Data Layer** (terrain, fuels, vegetation) — used by
BOTH the wildfire game/visualizer AND the "model your land" physical-print product
(the "print with trees" hook). Epic 3 (live/historic fire data) is the first thing
used by *only* the wildfire product; the physical-model business has no use for
incident feeds, weather, or fire progression. Structurally, Epic 3 also *consumes*
the Epic 1–2 pipeline (its A2 bakes terrain/fuels for an incident AOI on demand) —
it sits on top of the engine rather than being another layer of it.

## Decision
Split the codebase into two packages in ONE repo (monorepo), with a strictly
one-way dependency:

- **`terrain/`** — the world-data engine: AOI in → terrain/fuels/vegetation tiles +
  STL/GLB. Standalone; the model-your-land product ships this alone. Never imports
  `ember`.
- **`ember/`** — the wildfire product (Project Ember): fire data (Epic 3), sim
  (Epic 4), rendering (Epic 5), game (Epic 6). Imports `terrain`; calls
  `terrain.run_pipeline` for incident AOIs (`ember/incidents/bake.py` is the seam).

The one-way rule is enforced by `tests/test_ember_boundary.py`.

## Alternatives considered
- **Separate repos now** — truest separation, but adds cross-repo dependency
  management (two CIs, version pinning) that a solo dev carries. Deferred: the
  monorepo boundary already gives clean separation, and extraction later is cheap
  BECAUSE the dependency is one-way and enforced.
- **Keep everything in `terrain`** (as EPIC_3_PLAN.md assumed) — simplest today, but
  lets wildfire-specific code accrete into the shared engine and blurs the product
  line. Rejected.

## Consequences
- Two console scripts: `terrain` (engine) and `ember` (product).
- `ember` is the home for Epics 3–6; `terrain` freezes as a reusable engine (bug
  fixes + world-data features only).
- Extracting `ember` to its own repo later = move the package + depend on `terrain`
  as an installed package; no code untangling, because the boundary is already clean.
- The plan's `terrain incident` command becomes `ember incident`.

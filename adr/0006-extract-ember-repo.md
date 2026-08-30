# ADR 0006 — Extract Ember into its own repository

**Status:** Accepted (2026-08-30). Follows through on adr/0005.

## Context
adr/0005 split the codebase into `terrain/` (world-data engine) and `ember/`
(wildfire product) as two packages in one repo, with a strictly one-way,
test-enforced dependency (`ember` → `terrain`). It explicitly deferred separate
repos, noting extraction would be cheap *because* the boundary was already clean:
"move the package + depend on terrain as an installed package; no code untangling."

With Epic 3 Phase 1 done and Epics 4–6 (sim/render/game) all landing in ember, the
wildfire product now has enough independent surface to warrant its own repo, release
cadence, and issue tracker — while `terrain` freezes as a reusable engine.

## Decision
Extract `ember` into `github.com/WheezardX/Ember`, a standalone repo.

- **Fresh git init**, not a history rewrite. Ember's file history remains in the
  Terrain repo up to the removal commit; the new repo starts with a clean initial
  commit that references the extraction point. Ember's history was thin (Epic 3
  Phase 1), so little continuity is lost and no `filter-repo`/subtree surgery is
  needed.
- **`terrain` is a git dependency** (`terrain @ git+…/Terrain.git`) declared in
  `pyproject.toml` for reproducible external builds. Day-to-day development uses an
  editable side-by-side install (`pip install -e ../Terrain`) in a shared conda env,
  which satisfies the requirement. terrain is not published to any index.
- Wildfire-only docs move with the product: `WILDFIRE_DESIGN.md`, `EPIC_3_PLAN.md`.
  adr/0005 is copied here for context; the terrain-side design docs stay in Terrain.

## Alternatives considered
- **Preserve history via subtree/filter-repo** — keeps `git blame` continuity, but
  `git subtree split` flattens the `ember/` dir to the repo root (needs cleanup) and
  `git filter-repo` may not be installed. Not worth it for a thin history that lives
  on in Terrain anyway. Rejected.
- **Publish terrain to a package index** — cleanest dependency story, but overkill
  for a solo, proprietary engine consumed by one product. Deferred.
- **Stay a monorepo** (adr/0005's status quo) — fine until now; the growing,
  independent Epics 4–6 surface tips the balance toward a separate repo. Superseded.

## Consequences
- Two repos, two `pip install -e`. The one-way boundary is now also a repo boundary:
  terrain can't accidentally depend on ember.
- Terrain drops the `ember` package, its console script, and the wildfire docs;
  Terrain keeps only its half of the boundary test (`terrain never imports ember`).
  Ember keeps the mirror (`ember runs on terrain`) plus the incident tests.
- Bumping terrain means bumping the git ref in ember's `pyproject.toml` (or just
  pulling the sibling checkout in the shared dev env).

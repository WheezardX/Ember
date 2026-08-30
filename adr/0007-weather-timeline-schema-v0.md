# ADR 0007 — Weather timeline schema v0

**Status:** Accepted (2026-08-30), **provisional**. Authored in Epic 3 (workstream C1);
explicitly renegotiable by Epic 4.1 (fire behavior), which is the real consumer.

## Context
Epic 3 attaches the weather a fire experienced to its scenario bundle (HRRR gridded
fields via C2, RAWS/Synoptic station series via C3). Fire behavior (Epic 4.1) will
consume this weather — but Epic 4 does not exist yet. Someone has to define the format
first, or the C-stream adapters have nothing to target. Per plan decision D2 (⚑,
confirmed), Epic 3 authors a **versioned** schema now and accepts that 4.1 will revise it.

## Decision
A weather timeline is a **manifest + sidecar** structure (`ember/weather/schema.py`):

- **Time axis:** `t0` (UTC) + `step_minutes` + `num_steps` — a regular grid of steps
  over the incident window.
- **Two aligned representations:** a low-res **gridded** field over the incident AOI
  (`GridSpec`: metric CRS, bbox, nx/ny, cell size, cell-centered — no Arakawa
  staggering at v0), and point **station** series (`StationSeries`). At least one is
  required.
- **Variables** as u/v wind components + t2/rh2/precip, in canonical units
  (m/s, K, %, mm/step) recorded in the manifest. u/v avoids direction-averaging bugs.
- **Provenance per step** (`StepProvenance.gridded_source`): `hrrr:anl` (analysis F00,
  historic truth) vs `hrrr:fNN` (forecast, live) — honoring D4.
- **Numeric payload lives in sidecars** (`grid_data` npz, `station_data` parquet) the
  manifest points at, so the manifest stays small, git-diffable, and self-describing
  (shapes/units/provenance without opening the arrays). The array stack (xarray/pandas/
  pyarrow) is only needed to *write/read* the sidecars, not to validate the manifest.
- **Gaps are explicit** (`gaps: [...]`), never silently interpolated (C4 rule).
- Everything carries `schema_version` (0) and a fixed `format` tag.

## Alternatives considered
- **One monolithic parquet/NetCDF** with everything inline — couples validation to the
  heavy array stack and makes the contract opaque to diff. Rejected for v0; the manifest
  is the contract, sidecars are the payload.
- **Degrees Celsius / wind speed+direction** — friendlier to eyeball but invites unit
  drift and vector-averaging errors. Kept SI/component form; QA renders human units.
- **Wait for Epic 4.1 to define it** — blocks the entire C-stream. Rejected (D2).

## Consequences
- C2/C3/C4 target this manifest; they own writing the sidecars (deferred until the
  array stack is provisioned in the env).
- Bundles can reference a `weather` timeline manifest that validates with zero heavy
  deps.
- Epic 4.1 is expected to supersede this with a v1 (likely: staggering, more variables,
  vertical levels). The `schema_version`/`format` tags make that a clean migration.

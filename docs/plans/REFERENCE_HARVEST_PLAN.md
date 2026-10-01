# Reference harvesting plan (EPIC_5_PLAN 8i R3/R4, 2026-09-30)

Brad: "a lot of our visual tweaking is wasted without [reference material]". Goal: a library of
real imagery, tagged by subject and licence, with as many photos as possible tied to a place and
pose so `ember-dev ref-pair` can render the same view (R1) and the A3 reviewer can diff pairs (R5).

## Library

- Images: `store/reference/<source>/<id>.jpg` (outside git; Ember's own store beside incidents / sim, bucket later).
- Manifest: `viz/reference/manifest.jsonl` in git, one line per image: id, source, source URL,
  author / credit, licence class, date taken, GPS + bearing + 35 mm focal length when known
  (EXIF), subject tags, AOI hit (inside a world's extent -> reference-camera candidate),
  perceptual hash (dedupe), notes (e.g. "before 2025 thinning").
- Licence classes: `shippable` (BNE-owned, US federal works) / `reference-only` (everything else:
  look at it, never feed it into a shipped asset). Unknown = reference-only.
- Every fetch goes through `NetMeter` (Terrain `store/downloads.jsonl`) with a size estimate
  first; each harvest has a byte cap.

## Tooling (agent, about one evening)

- `ember-dev ref add <files|dir> --source --licence --tags`: copy in, read EXIF, hash, manifest.
- `ember-dev ref harvest <adapter>`: one small adapter per source (list -> estimate -> fetch with
  a cap and polite rate limit). Licence and credit are recorded by the adapter, never guessed.
- `ember-dev ref sheet [--tag ..]`: contact sheet (HTML) for Brad's keep / reject / tag pass.
- Auto-tagging by the model reviewer (subject tags only, advisory); Brad's pass is authoritative.
- Photos with GPS inside an AOI get a reference-camera bookmark generated (R1 already exists).

## Sources, in order

| # | Source | Licence | Size (est.) | Value |
|---|---|---|---|---|
| 1 | Brad's Kachess album "Camp site" (142 photos, 2014-2025, Pixel phones) | shippable (BNE) | ~0.7 GB originals | the stand we render, by year; 2025 = after fire-risk thinning |
| 2 | InciWeb Three Queens photo gallery (~6 pages, Sep 2026) | federal work -> shippable, per-photo credit checked | ~50-100 MB | the exact incident: crews, lookouts, smoke, terrain |
| 3 | Brad's field shoot (R4): named viewpoints, GPS on, bearing + lens noted, per fuel class, a burn scar | shippable | Brad's phone | the ground-level look targets |
| 4 | Facebook group "threequeensfire" | reference-only (posters' copyright) | - | exact-scenario fire behaviour, smoke from the valley |
| 5 | USFS / BLM / NIFC Flickr (US-government-work licence filter) | shippable | cap 0.5 GB at 2048 px | PNW fire behaviour, burn scars, smoke columns, crown vs surface |
| 6 | Mapillary street-level along I-90 / Kachess Lake Rd (geotagged + bearing) | CC BY-SA, reference-only | cap 200 MB | free reference-camera pairs for roadside stands |
| 7 | Wikimedia Commons (PNW conifer forest, wildfire categories) | per file | cap 200 MB | gap filling |
| 8 | Sentinel-2 / Landsat scenes of the burn (open data) | shippable | per scene ~100 MB COG windows | scar look from altitude, progression |

Notes:
- **1:** the shared album keeps lens, date and compass bearing but strips GPS. Turning on "share
  location" in the album options keeps it; otherwise positions are set by hand (the property is
  known). Tag by year so the pre/post-thinning look is not mixed.
- **4:** no automated harvesting - Facebook's terms forbid scraping and it needs a login. Options:
  Brad saves the best posts by hand into a folder (`ref add` takes it from there), or asks the
  admins / posters for originals (which also brings EXIF/GPS and might make some shippable).
- **5-7:** licence filter at the API, credit recorded per image; nothing without a licence field.

## Status

- 2026-09-30: library tooling in (`ember/dev/reference.py`, `ember-dev ref-add`, `ref-sheet`).
  Kachess album: 142 items screened from thumbnails -> 54 stills filed (people, vehicles,
  construction, trail-cam, video left out). InciWeb: 59 screened -> 14 filed (smoke columns,
  smoky valleys, burnout, fuel break, vistas), reference-only until each credit is checked.
  68 images, 66 MB on disk, ~330 MB fetched (ledger).
- 2026-09-30 night: Facebook "Three Queens Fire Information 2026" (the incident team's page, not
  a community group) screened photo by photo in Brad's browser with his rules (keep fire, line
  building, hose/sprinklers, wrapped structures, comms, incident reports; skip team / T-shirt
  shots): 247 kept (shortlist `viz/reference/facebook_3q_shortlist.txt`); 204 downloaded and
  filed (reference-only). 43 remain - the browser tool blocked itself on a localhost hand-off
  page; mostly Aug 6-13 maps/updates, the two Lake Kachess hero aerials and the Thorp repeater
  shots. InciWeb +10 (Brad's rules) and all 42 daily update texts. Library 282 images, 182 MB.
  Timeline + comparison: `docs/reference/THREE_QUEENS_2026_TIMELINE.md`.

## Order

1. Tooling + #1 + #2 (tonight / next session), contact sheet for Brad.
2. Brad: keep/reject/tag pass; turns on album location sharing; FB picks; field shoot when he can.
3. #5-#8 per subject gaps the contact sheet shows (smoke column, crown fire, scar, litter, hung-up
   trees, stand density).
4. First photo/render pairs (R1) for every located image inside an AOI; then R5 (model diff).

# HCP4 — "The fire reads" (Jolly Mountain, Epic 4 CA model run)

**Ask:** sign-off (or redirect) on the fire's legibility pass before the fundamentals round (8g).
Exit criteria (EPIC_5_PLAN §5): a viewer can point at the screen and say where it's hot, where
it's heading and what's already lost; the plume never contradicts the HUD wind; the budget holds.

**Data:** HCP4 renders Epic 4's CA model run of Jolly (`runs/cp5/cp5-jolly-shadow-ca`), not the
HCP3 playback: the playback has one intensity class everywhere, no spot fires and one constant
wind, so there was nothing to show "hot" or "heading" with.

## What to look at

| File | What it shows |
|---|---|
| `ab/*_ab.jpg` | Same run, same views: left drawn the HCP3 way (no classes), right the HCP4 way |
| `stills/*.jpg` | The six HCP4 views (460 h mixed fire + spotting, 503 h crown-fire run, 459 h spotting, eye level at the head) |
| `orbits/plume_vs_wind_720p.mp4` | 494-510 h timelapse over a ~90° wind shift, the stream's wind burned in as a vane |
| `perf/perf_head_eye_503h.csv` / `.json` | 600-frame perf window at the eye-level head, every fire effect on |

## Against the criteria

* **Where it's hot — partly.** Flames stay lit for the stream's whole burning residence (was a
  ~1 h window, so 3 k crown-fire cells drew as a thin band); class colours (surface fire deep red,
  class 2 orange, crown fire yellow-white); the head (fastest spread, from the arrival field)
  draws brighter and whiter. Clear up close — surface-fire stands keep green crowns
  (`ab/crown_close_503h_ab.jpg`), class-3 stands blacken — but from a few km the class colours
  tone-map to much the same orange. Your call whether that is enough or wants a stronger
  IR-map style coding at altitude.
* **Where it's heading — yes, subtle.** The head reads hotter than flanks / backing fire within a
  class (1.5-2x the spread rate). Spotting shows as spark showers from the head to where brands
  land, and a flare on each new spot fire (`stills/spot_side_459h.jpg`; 309 brands, 22 ignite).
* **What's lost — yes.** The scar (unchanged from HCP3) plus crowns by class.
* **Plume never contradicts the wind — yes, by construction.** Smoke takes the tick's wind from
  the stream; the film's vane is the stream's own wind, so it is a visual check, not an
  independent measurement. Known limit: the whole plume re-aims at once when the wind shifts (a
  real plume would bend over time).
* **Budget holds — yes.** At the eye-level head, 2560x1440, flames (898 cards) + torching +
  smoke (10 k puffs): p95 frame 6.3 ms (budget 16.7), 0 hitches, 3.6 GB VRAM (budget 4 GB).
  No firebrands were in flight at that moment (they add at most a few hundred sprites).

## What changed since HCP3

* Exposure: histogram auto exposure by default (dusk / backlit / under-canopy views were 2-3
  stops dark); stills adapt instantly and stay deterministic.
* Intensity classes, wind from the stream, flame persistence, heading (FireTex A = rate of
  spread), firebrands + spot flares, eye-level flame cards (M_Flame), torching as sparse tongues
  instead of painted crowns, and the ground "contour rings" (a frozen flicker pattern) removed.

## Open / carried

* Class legibility from altitude (above). Eye-level flames still a little sparse.
* Eye-level fire views are not pixel-deterministic run to run (ssim ~0.97-0.99); thresholds
  loosened on two captures until the source is found.
* Burned interior goes straight to black; real fires keep burning / smouldering / smoking inside
  (your note). Plume history (bending with past wind). Night pass not done.
* Next per your sequencing: the 8g fundamentals, not new fire features.

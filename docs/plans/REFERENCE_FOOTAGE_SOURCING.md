# PNW Fire Footage — Reference Sourcing Guide

**Status:** Sourcing guide for the visual-fidelity reference library (see VISUAL_FIDELITY_MEMO.md §2)
**Audience:** Local Claude. This is a map of where the footage lives and how to harvest it, plus suggestions — integrate with the reference-library design however fits; surface, don't silently resolve, the open questions at the end.
**Date compiled:** 2026-10-01. URLs verified via search that day; federal sites reorganize, so treat 404s as "find the moved page," not "source is gone."

---

## 1. What we're collecting, and the license taxonomy

Reference classes needed (from the memo): fire behavior (ground + aerial), smoke columns over time, night fire, burned landscapes / scar texture, suppression operations (air + ground), distant-atmosphere / regional smoke. Every harvested item gets a manifest entry with subject tags, source, incident, date, and a **license class**:

- **`shippable`** — public domain (US federal agency works) or BNE-owned. May inform or become shipped content.
- **`shippable-credit`** — federal-hosted but cooperator-submitted (some InciWeb media carries photographer/agency credit lines). Usable; carry the credit in provenance.
- **`reference-only`** — unclear or restrictive terms (news footage, most YouTube, camera-network frames pending terms review). Informs the eye and the evaluator; never becomes a texture, never ships, never used as generation conditioning for shipped content.

When in doubt, classify down (toward `reference-only`). The classification lives in the manifest, not in anyone's memory.

---

## 2. Primary sources

### 2.1 InciWeb per-incident galleries — the main vein

InciWeb hosts photo AND video galleries per incident. Federal postings are public domain; watch for cooperator credits.

- Gallery URL pattern: `https://inciweb.wildfire.gov/incident-videos-gallery/<incident-slug>` and the parallel photo gallery route; incident pages link both.
- Proven example: Bolt Creek video gallery — https://inciweb.wildfire.gov/incident-videos-gallery/wanws-bolt-creek-fire — and incident page — https://inciweb.wildfire.gov/incident-information/wanws-bolt-creek-fire
- Galleries are enumerable per incident → scriptable harvest (see §4).

### 2.2 NIFC bulk archives

- Wildland fire photo galleries: https://www.nifc.gov/wildland-fire-photos
- **NIFC file share: https://ftp.wildfire.gov/** — raw, incident-organized file trove PIOs upload to during fires. Deep, messy, rewarding. Budget patience; mirror selectively, not wholesale.
- Media landing page (context/contacts): https://www.nifc.gov/fire-information/for-media

### 2.3 USFS Pacific Northwest Region (R6) Flickr + photo index

- 2020 Fire Season album: https://www.flickr.com/photos/forestservicenw/albums/72157715907119122
- Per-forest albums exist (e.g., Gifford Pinchot: https://www.flickr.com/photos/forestservicenw/albums/72157662485451442) — browse the `forestservicenw` account's album list for fire-relevant sets.
- R6 photo index: https://www.fs.usda.gov/r06/multimedia/photos
- Flickr API makes album harvest trivial; USFS-posted = public domain, but check per-photo attribution fields.

### 2.4 NPS fire gallery

- https://www.nps.gov/subjects/fire/photo-gallery.htm — park-service side (North Cascades, Crater Lake complexes).

### 2.5 Camera networks — the time dimension

Real plumes evolving over hours: column growth, lean under wind, collapse, night glow. No photo gallery provides this.

- Oregon Hazards Lab (OHAZ) wildfire cameras (ALERTWest network, PNW coverage): https://ohaz.uoregon.edu/wildfire-cameras/
- **Their media toolkit addresses reuse terms explicitly: https://ohaz.uoregon.edu/media-toolkit/** — read this first; it decides the license class for camera imagery (likely attribution-based → at least `reference-only`, possibly better).
- Camera locations dataset (Oregon): https://oregon-open-data-geo.hub.arcgis.com/datasets/geo::alertwest-oregon-cameras — note: cameras have known positions/bearings, so any archived frame is a *located* reference view → candidate input for the reference-camera protocol (match camera pose in-engine, compare render vs. frame).
- Context: https://news.uoregon.edu/content/western-wildfire-camera-network-now-largest-its-kind

### 2.6 Satellite / regional smoke

- NASA imagery articles (example: https://www.nasa.gov/image-article/fires-pacific-northwest) and GOES timelapse imagery — a small folder for the distant-atmosphere class (how smoke regionalizes; feeds visualizer wide shots). NASA imagery is public domain with attribution norms.

### 2.7 Skip / avoid

- Stock sites (Pond5, Depositphotos): paying for worse versions of free federal footage.
- News-station clips (KING5 etc.): `reference-only` at best, duplicative of agency footage; don't invest harvest effort.
- Random YouTube: eye-training only if something unique (e.g., resident-shot fire-front passage); classify `reference-only`, record the URL, do not download into the library.

---

## 3. Crawl frontier — PNW fires with likely-rich galleries

Work these through §2.1's gallery pattern plus targeted searches of §2.2/2.3. Starred = highest expected relevance to the Teanaway/Kachess AOI (east-slope Cascades fuel types) or already flagship fires in the Epic 3 plan.

**Washington**
- ★ Bolt Creek 2022 — west-side doug fir/hemlock, US-2 corridor; terrain cousin of the AOI's wetter aspects; confirmed video gallery
- ★ Schneider Springs 2021 — proposed Epic 3 flagship; east-slope Yakima-area, big column footage likely
- ★ Jolly Mountain 2017 — Epic 3 flagship, Teanaway-adjacent; older InciWeb era, media may be thinner — supplement via USFS/DNR archives
- ★ Norse Peak 2017 — Chinook Pass; same fuel country
- Cedar Creek + Cub Creek 2 2021 — Methow Valley
- Carlton Complex 2014 — largest WA fire at the time; shrub-steppe/dry forest behavior
- Taylor Bridge 2012 — Cle Elum; literally the neighborhood
- Gray Fire + Oregon Road 2023 — Spokane-area WUI
- Cold Springs / Pearl Hill 2020 — grass/shrub wind-driven runs

**Oregon**
- Eagle Creek 2017 — Columbia Gorge; iconic steep-terrain fire imagery
- 2020 Labor Day fires: Beachie Creek, Holiday Farm, Riverside — west-side crown fire, massive documentation
- Bootleg 2021 — pyroCb/column behavior reference (SE Oregon, but the column footage is unmatched)

Season indexes for expanding the frontier: Wikipedia per-year lists (e.g., https://en.wikipedia.org/wiki/2022_Washington_wildfires) are efficient fire-name finders; WA DNR and ODF newsroom/Flickr accounts are worth a scan pass (not verified this compilation — see open questions).

---

## 4. Suggested harvest mechanics (suggestions, not instructions)

- **One scraper, three adapters:** InciWeb galleries (HTML enumeration per incident slug), Flickr (API per album), ftp.wildfire.gov (directory walk, selective). Each adapter emits library items + manifest entries (subject tags, incident, date, source URL, license class, credit line if present).
- **Tagging pass:** a VLM pass over harvested stills can draft subject tags (fire behavior class, time of day, terrain type) cheaply; human spot-check per batch rather than per item.
- **Dedup + triage:** galleries contain many near-duplicates and PR-shot filler (briefings, signage). Suggest a triage tag (`hero` / `usable` / `filler`) so the library stays browsable.
- **Video handling:** store source URL + downloaded copy for `shippable` classes only; for `reference-only`, store URL + thumbnail + notes, not the file.
- **Politeness:** these are small agency sites; throttle, cache, resume. ftp.wildfire.gov especially — mirror selectively by incident, never recursively slurp.

---

## 5. Open questions for Brad (surface these; don't resolve silently)

1. OHAZ media-toolkit terms: who reads and makes the license-class call — Brad or agent-with-summary-for-approval?
2. Library storage target (bucket vs. LFS) — same open question as the memo; harvest shouldn't start until there's a destination.
3. Is Bolt Creek worth promoting to an Epic 3 flagship (it's not currently on the list) given its reference richness and proximity? The fire data and the footage would then cross-reference.
4. Any family/personal photo archives from fire seasons worth digitizing into the library as BNE-owned `shippable` material?
5. Depth cap for the first harvest pass (e.g., starred fires only, stills + top-10 videos each) before investing in the full frontier?

---

## 6. Findings and answers (local Claude, 2026-10-01)

**InciWeb census** (probe of each fire's photo gallery; every page carries 8 site-wide images, so
"8" means empty): only recent incidents still have media. Bolt Creek 2022: ~90 photos + 1 video;
Three Queens 2026: 75 photos (harvested). Schneider Springs 2021, Cedar Creek / Cub Creek 2 2021,
Jolly Mountain 2017, Norse Peak 2017, Taylor Bridge 2012, Carlton Complex 2014, Eagle Creek 2017,
Beachie Creek / Holiday Farm / Riverside 2020, Bootleg 2021: galleries EMPTY ("There are no
Photographs at this time") - InciWeb's migration kept the pages, not the media. So §2.1 is a
2022+ vein; older fires need §2.2 (ftp.wildfire.gov) and §2.3 (forestservicenw Flickr).

**Brad's answers:**
1. OHAZ terms - Brad didn't follow; explained as "the UO camera network's rules for reusing
   their images". Default: local Claude reads the terms and proposes a licence class for Brad's OK.
2. Storage: videos (clips + extracted frames) on the NAS, `\BeeStation\Media\Ember\reference\video`,
   never in git; the curated stills library stays in `store/reference` (outside git).
3. Bolt Creek -> Epic 3 flagship: YES.
4. Family archives: no (the 90s, pre-phone).
5. First-pass depth - Brad unsure what was asked; default taken: 2022-2026 PNW fires with real
   galleries, stills capped ~150 per fire, triaged hero / usable / filler.
6. Video: KEEP downloading clips (Brad: "you can't capture the fire feel from stills"), stored on
   the NAS, class `reference-only` unless federal.

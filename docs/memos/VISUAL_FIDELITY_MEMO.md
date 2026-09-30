# Visual Fidelity Memo — Image Gen, Blender MCP, and Real-World Reference

**Status:** Discussion / suggestions — NOT an execution plan
**Audience:** Whoever integrates these ideas into EPIC_5_PLAN.md (likely an agent). Nothing here is a directive; treat it as rationale and options to weigh against the plan's existing structure. Where this memo conflicts with the plan, the plan's principles (assets-from-code, human checkpoints, probes-before-vibes) should win unless there's a stated reason.
**Origin:** Conversation 2026-09-30. Prompted by two observations: (1) a coworker's success using an orchestrator + image-gen + Blender MCP flow for a 2D three.js game; (2) the realization that Ember currently has *no real-world reference material* anchoring its visual targets.

---

## 1. The gap this memo addresses

Epic 5's iteration loop is strong on *mechanics* (build, capture, evaluate, bundle) but weak on *targets*. As planned, agents iterate toward whatever the model-eyes evaluator and Brad's HCP feedback imply — but the evaluator has nothing grounded to compare against, and Brad's feedback only arrives at checkpoint boundaries. Two consequences:

- The D9 model-eyes reviewer judges renders against its general training priors ("what fire looks like"), not against how *this* landscape and *this* phenomenon actually look. Synthetic judging synthetic.
- Convergence between HCPs is slow: agents can't know what "Teanaway at dusk" should look like until a human says "not that."

Three ideas below attack this: **real reference** (grounds everything), **image generation** (turns reference + taste into iterable targets and certain texture classes), and **Blender MCP** (accelerates the mesh-generator development the plan already commits to). They interlock — reference grounds image gen; image gen produces targets; targets feed the evaluator — and they're worth integrating as a coherent addition rather than three bolt-ons.

A framing caution from the coworker comparison: in a 2D three.js game, generated images *are* the shipped assets, so the flow is the whole pipeline. In Ember, fidelity lives mostly in meshes, materials, lighting, and density inside UE. These ideas augment Workstream B; they don't replace the seed-pack/material-contract architecture.

---

## 2. Real-world reference material (the highest-leverage idea)

### 2.1 Why it matters more than tooling

Every fidelity judgment in Epic 5 — human or model — currently floats free of ground truth. A curated reference library converts "does this look right?" into "does this match *that*?", which helps all three judges: agents (side-by-side comparison), the VLM evaluator (VLMs are much stronger at "how do these two images differ" than at open-ended aesthetic judgment), and Brad (faster, more specific HCP notes).

### 2.2 Suggested shape: a tagged library with a license boundary

A repo-adjacent collection (LFS or bucket — it will outgrow git) where every item carries: subject tags (fuel class, fire behavior type, time of day, weather/atmosphere), location + date where known, source, and — critically — a **license class**:

- `shippable` — public domain or BNE-owned. Federal imagery qualifies (US government works): NIFC/USFS media galleries, InciWeb incident photo archives, BLM/USFS Flickr streams. These collections are rich in exactly the material nobody sells: retardant lines on brush, mosaic burn patterns, night operations, column behavior. Brad's own photos/drone footage also land here.
- `reference-only` — news footage, YouTube fire videos, ALERTWildfire-style camera frames, anything with unclear terms. Informs the eye and the evaluator; never becomes a texture or training input for anything shipped.

The boundary matters because the temptation to lift a great texture from a reference photo is constant, and license class in the manifest makes the rule mechanical instead of remembered.

Worth a look for dynamic fire behavior specifically: the public PTZ wildfire-camera networks (ALERTWildfire and successors) and their archives — real smoke columns leaning in real wind over hours, haze gradients, night glow. That's the truth standard HCP4's plume-coherence question currently lacks.

### 2.3 The reference-camera protocol (suggested as the centerpiece)

Because the M0 AOI is real terrain Brad knows intimately, reference can be *location-matched*: shoot from a recorded GPS position, bearing, and lens FOV; reproduce the identical camera in-engine; render the same framing. Photo left, render right, same ridgeline.

This is suggested as the centerpiece because it upgrades the fuzziest gate in the plan — HCP2's "does Teanaway look like Teanaway" — into a repeatable side-by-side, and it produces the comparison format VLMs handle best ("render's canopy is too uniform; ridge silhouette too smooth; shadows too blue"). If adopted, it implies: a small field-shoot kit/checklist (panoramas from named viewpoints, ground-level shots per FBFM40 class present in the AOI, burn-scar material from a nearby fire scar), a scenario-file representation for reference cameras so captures reproduce them automatically, and reference pairs embedded in HCP bundles.

The ground-level-per-fuel-class contact sheets do double duty: they're also the palette-tuning reference Epic 2's species palettes never had.

### 2.4 NAIP as a machine-checkable color probe

NAIP aerial imagery (~0.6–1m, public domain, full CONUS) over the AOI provides ground-truth *color* per landcover class. Two suggested uses, in increasing ambition:

1. **Probe:** sample NAIP per landcover class → target hue/value distributions → a scene-facts assertion that distant/top-down terrain renders within tolerance. This converts one aesthetic failure mode (terrain drifting toward generic-game-green) into a hard evaluate check — very much in the plan's probes-before-vibes spirit.
2. **Data layer:** NAIP as an optional imagery layer through the Epic 1 adapter seam, for distant-terrain grounding or material blending. Bigger lift; worth a ticket, not a commitment.

### 2.5 Expert annotation

Reference footage shows what fire looks like; it takes someone who has worked a fireline to say what you're *looking at*. A small annotated-clip set (backing fire vs. crown run, column behavior, what's about to happen and why) — plausibly built in one session with Brad's family — would serve HCP4's legibility bar now and Epic 6's authenticity later. Cheap, unique, and unpurchasable.

---

## 3. GPT image generation (via Codex CLI, orchestrated from Claude)

### 3.1 Where it genuinely helps

- **Pre-vis / look bible.** The strongest use. Generate candidate frames for key looks (biome × fire state × time of day), Brad curates, keepers become style targets checked into the repo. The D9 evaluator then compares renders against *curated* targets — taste as an artifact agents can iterate against between HCPs. Also useful inside HCP memos as "target vs. current" pairs.
- **Custom texture classes nobody sells:** scorch/char decals, retardant stains, cured-grass blends, burned-duff variants, mosaic-burn transitions; plus UI icons and HUD elements. Seamless/tileable output is achievable; normal/roughness derivation from albedo is scriptable downstream.
- **Grounded generation.** The synthesis with §2: condition image generation on real reference (image-to-image / style reference) rather than cold prompts. Scorch decals conditioned on actual PNW burn-scar photos will read true in a way prompt-only generation won't. Suggest treating ungrounded generation as a smell once the reference library exists.

### 3.2 Where it will disappoint (suggest steering around)

- **Flame/smoke flipbooks:** no temporal coherence frame-to-frame. Flipbooks stay procedural-then-EmberGen per the existing plan.
- **Tree impostors/billboards:** must match the 3D mesh across LOD transitions, so they bake *from* the mesh by construction. Generated billboards will pop.
- **Raw terrain surfaces:** scanned libraries (Megascans-class) still beat generated realism per dollar for rock/soil/duff ground truth. Generated wins for the custom/stylized/wildfire-specific layer on top.

### 3.3 Disciplines that make it durable

- **Prompt-as-source:** the prompt (and any conditioning-image reference) is the texture's source code; check it in next to the output. Regeneration and iteration both need it; an un-prompted PNG in the repo is a dead end.
- **Curation gate:** generated content that ships passes a human contact-sheet review (fits the existing HCP-bundle rhythm). Generated imagery has a long tail of subtle wrongness probes won't catch.
- **Licensing homework:** verify current OpenAI/Codex commercial-output terms before shipping generated textures in a paid product. Same diligence as Fab assets; record in the same provenance/ADR pattern.
- **Mechanics:** invoking Codex from the Claude-driven loop is unexotic — it's a command-line tool with a prompt file in and PNGs out, same category as ffmpeg. Orchestrator choice (e.g., Opus-class at medium effort for the grind) is a cost/quality dial, not an architecture decision.

---

## 4. Blender MCP (for models)

### 4.1 The phase distinction that keeps the plan's contract intact

The plan commits to deterministic, regenerable assets: every `.uasset` reproduces from checked-in scripts. Blender MCP — an agent interactively driving a live Blender session — sits in tension with that *if* its outputs are treated as assets. Suggested resolution: **MCP for discovery, headless for production.**

- **Discovery:** an agent exploring "what whorl density and branch droop make a doug fir silhouette read at 200m" interactively, with screenshots in the loop, is dramatically faster than blind-editing a generator script and re-running it. This is the right tool for developing and tuning the tree/shrub generators, and for one-off investigations (why does this mesh's Nanite conversion look wrong).
- **Production:** the deliverable of any MCP exploration is the *crystallized generator script* (`blender --background --python gen_conifer.py`, seeded, deterministic) — not the mesh the session produced. If a flow ends with "here's the .blend I made," it ended one step early.

### 4.2 Why procedural trees remain the right base (unchanged, restated for the integrator)

Conifers are the most parametrically tractable tree family, and a wildfire project needs every species in scorched/torched/consumed states — a *parameter axis* in a generator, an unpurchasable product in a marketplace. The escalation path (SpeedTree session or curated Fab pack, re-parented onto the material contract) remains the escape hatch if HCP2 rejects procedural silhouettes.

---

## 5. How the three interlock (the actual suggestion)

```
 real reference (§2)          image gen (§3)               Blender MCP (§4)
 ──────────────────           ─────────────────            ─────────────────
 ground truth library   ──▶   conditioned look bible   ┐   generator development
 reference-camera pairs ──▶   custom texture classes   │   (discovery mode)
 NAIP color probes      ─┐    (curated, prompt-as-src) │        │
                         │            │                │        ▼
                         ▼            ▼                │   crystallized scripts
                   evaluate step: probes + grounded targets    (production mode)
                         │            │                │        │
                         └────────────┴────────┬───────┴────────┘
                                               ▼
                              HCP bundles: render-vs-reference
                              side-by-sides, curated contact sheets
```

If integrated, the natural seams in the existing plan are: Workstream B gains the look-bible and texture-station concerns; Workstream A's evaluate step gains reference-pair comparison and the NAIP probe; the scenario format gains reference cameras; the HCP bundle definition gains render-vs-reference pairs; and a new small workstream (or B-extension) owns the reference library and its manifest. A field-shoot task for Brad is human work and should be scheduled like the other sanctioned human sessions, not agent-assigned.

## 6. Open questions for the integrator (don't resolve silently — surface them)

1. Where does the reference library live (LFS vs. bucket), and does its manifest join the existing provenance schema or get its own?
2. Does the NAIP probe gate evaluate verdicts from day one, or run advisory until tolerances are tuned on real captures?
3. Reference-camera reproduction: how close is close enough (FOV/position tolerance), and is atmospheric mismatch (haze, season) handled by shooting rules or by comparison methodology?
4. Is image-conditioned generation available through the chosen Codex path, or does the look bible start prompt-only until it is?
5. Budget/cadence for the human sessions this implies (field shoot; annotation session; possible SpeedTree/EmberGen weekends) — the plan capped human asset sessions at ≤1 per phase; does that cap flex or do these queue?

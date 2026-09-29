"""Render scenarios (`viz/scenarios/*.toml`, `render_scenario_version = 1`).

A render scenario is the unit the harness runs: which world (and optionally which
replay) to load, where the camera goes, what to capture, and what must be true
afterwards. Adding a test view is a data change — see docs/viz/harness.md.

Camera bookmarks are *orbit-style* so they survive terrain edits: a look-at target on
the ground (grid fraction or cell), a distance, yaw and pitch. The UE side resolves the
target's height from the loaded terrain.
"""

from __future__ import annotations

import os
import tomllib
from pathlib import Path
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator

SCENARIO_VERSION = 1


class _Strict(BaseModel):
    model_config = ConfigDict(extra="forbid")


class Bookmark(_Strict):
    name: str
    # Exactly one of target_frac (0..1 of the grid, x east, y south — pack index order)
    # or target_cell ([x, y] cell index).
    target_frac: tuple[float, float] | None = None
    target_cell: tuple[int, int] | None = None
    distance_m: float = Field(gt=0)
    yaw_deg: float = 0.0        # compass bearing the camera *looks toward* (0 = north)
    pitch_deg: float = -30.0    # negative looks down
    fov_deg: float = 60.0
    sun: str = "noon"           # a key into the UE sun presets (dawn | noon | dusk | ...)

    @model_validator(mode="after")
    def _one_target(self) -> Bookmark:
        if (self.target_frac is None) == (self.target_cell is None):
            raise ValueError(f"bookmark {self.name!r}: give exactly one of "
                             "target_frac / target_cell")
        return self


class Capture(_Strict):
    name: str
    bookmark: str
    t_s: int | None = None      # sim time to seek before capturing (replay scenarios only)
    warmup_frames: int = 30     # frames rendered before the shot (streaming / TAA settle)
    # Image-diff thresholds against the golden (ember/dev/imagediff.py). Tight on purpose:
    # HCP0 measured renders as pixel-identical run to run, and a real material change (clay
    # roughness 0.5 -> 0.9) scored ssim 0.982 / region 0.927 - which 0.97 / 0.90 let through.
    # Loosen per capture for legitimately noisy views (e.g. Niagara), with a comment.
    ssim_min: float = 0.995
    region_ssim_min: float = 0.98
    golden: bool = True         # False: captured and shown, never diffed (e.g. WIP views)


class Orbit(_Strict):
    """A camera orbit around a bookmark's target, captured every frame -> MP4 (review bundles).
    With `to_bookmark` it is a flyover instead: every pose field is interpolated from
    `bookmark` to `to_bookmark` (yaw the short way round) and `degrees` is ignored."""
    name: str
    bookmark: str               # start pose; yaw advances by `degrees` over `frames`
    to_bookmark: str | None = None
    degrees: float = 360.0
    t_from_s: float | None = None   # replay scenarios: sim time sweeps t_from_s -> t_to_s
    t_to_s: float | None = None     #   across the frames (timelapse)
    frames: int = Field(default=240, gt=1, le=3600)
    fps: int = 30
    warmup_frames: int = 30


class FactAssert(_Strict):
    fact: str                   # dotted path into the scene-facts JSON, e.g. "tiles.loaded"
    op: Literal["==", "!=", ">=", "<=", ">", "<"]
    value: float | int | str | bool
    capture: str | None = None  # None: must hold at every capture point


class FireProbe(_Strict):
    """A world point whose fire phase is reported as the fact `fire.probes.<name>` (-1 outside
    the grid, 0 unburnable, 1 unburned, 2 burning, 3 burned) - checks the scar lands where the
    Epic 4 stream says it does."""

    name: str
    x: float                    # metres, the replay grid's CRS
    y: float


class ScenarioMeta(_Strict):
    name: str
    description: str = ""
    # "terrain:<region>" -> <terrain store>/<region> (a Terrain tile-store region with
    # manifest.json; EPIC_5_PLAN D10), or a path relative to the scenario file.
    world: str
    replay: str | None = None   # optional .replay.json (fire state), relative
    resolution: tuple[int, int] = (1920, 1080)
    budget: str = "interactive" # key into viz/budgets.toml
    perf_frames: int = 0        # >0: frames measured after the last capture -> facts/perf.json
    exposure_bias: float = -2.0  # manual exposure EV100 bias (captures must not auto-expose)
    fixed_lod: int | None = None  # load one LOD everywhere (fixtures); None = stream (C3)
    lod_refine_factor: float = 1.5  # streaming: refine while distance < factor * tile span
    look: str = "viz/looks/terrain_default.toml"  # repo-relative look file, or "clay"
    vegetation: bool = False    # C4: instance the Terrain-conformant scatter near the camera
    veg_radius_m: float = 1500.0
    veg_near_radius_m: float = 500.0      # near tier: full trees, wind, live shadows (D11 s2)
    veg_mid_min_height_m: float = 12.0    # mid tier skips understory shorter than this
    # Wind sway (M_Veg world-position offset). Strength = crown-top sway in cm for a 10 m tree
    # (grows with height^2); from = compass direction the wind blows FROM. Stills use a frozen
    # wind clock (t = 0) so goldens stay pixel-deterministic; orbits/flyovers run it at frame/fps.
    wind_strength: float = 6.0
    wind_from_deg: float = 270.0
    smoke: bool = True           # replay scenarios: smoke v0 plumes (bend with wind_from_deg)
    smoke_wind_ms: float = 8.0   # plume drift speed (m/s)
    veg_lineup: bool = False    # B3 silhouette sheet: one tree per species at the first capture's
    #                             target (row across the view) + a 1.8 m post, no scattered trees
    exec_cmds: list[str] = Field(default_factory=list)  # console commands after world load
    perf_exec_cmds: list[str] = Field(default_factory=list)  # as the perf window opens
    water: bool = True           # use store/render/<region>/water (ember-dev water) if present
    perf_bookmark: str | None = None  # camera pose for the perf window (default: last pose)


class RenderScenario(_Strict):
    render_scenario_version: int
    scenario: ScenarioMeta
    bookmarks: list[Bookmark]
    captures: list[Capture]
    orbits: list[Orbit] = Field(default_factory=list)
    fire_probes: list[FireProbe] = Field(default_factory=list)
    asserts: list[FactAssert] = Field(default_factory=list, alias="assert")

    model_config = ConfigDict(extra="forbid", populate_by_name=True)

    @model_validator(mode="after")
    def _check(self) -> RenderScenario:
        if self.render_scenario_version != SCENARIO_VERSION:
            raise ValueError(f"render_scenario_version {self.render_scenario_version} "
                             f"unsupported (want {SCENARIO_VERSION})")
        names = [b.name for b in self.bookmarks]
        if len(set(names)) != len(names):
            raise ValueError("duplicate bookmark names")
        caps = [c.name for c in self.captures]
        if len(set(caps)) != len(caps):
            raise ValueError("duplicate capture names")
        for c in self.captures:
            if c.bookmark not in names:
                raise ValueError(f"capture {c.name!r} references unknown bookmark {c.bookmark!r}")
            if c.t_s is not None and self.scenario.replay is None:
                raise ValueError(f"capture {c.name!r} sets t_s but the scenario has no replay")
        if self.fire_probes and self.scenario.replay is None:
            raise ValueError("fire_probes need a replay")
        probes = [p.name for p in self.fire_probes]
        if len(set(probes)) != len(probes):
            raise ValueError("duplicate fire probe names")
        if self.scenario.perf_bookmark is not None and self.scenario.perf_bookmark not in names:
            raise ValueError(f"perf_bookmark {self.scenario.perf_bookmark!r} is not a bookmark")
        for o in self.orbits:
            for b in (o.bookmark, o.to_bookmark):
                if b is not None and b not in names:
                    raise ValueError(f"orbit {o.name!r} references unknown bookmark {b!r}")
        for a in self.asserts:
            if a.capture is not None and a.capture not in caps:
                raise ValueError(f"assert on {a.fact!r} references unknown capture {a.capture!r}")
        return self


class LoadedScenario(BaseModel):
    """A parsed scenario plus its resolved paths."""

    path: Path
    spec: RenderScenario
    world_path: Path
    replay_path: Path | None

    @property
    def name(self) -> str:
        return self.spec.scenario.name


def scenarios_dir(repo: Path) -> Path:
    return repo / "viz" / "scenarios"


def find_scenario(repo: Path, name_or_path: str) -> Path:
    p = Path(name_or_path)
    if p.suffix == ".toml" and p.exists():
        return p
    cand = scenarios_dir(repo) / f"{name_or_path}.toml"
    if cand.exists():
        return cand
    known = sorted(q.stem for q in scenarios_dir(repo).glob("*.toml"))
    raise FileNotFoundError(f"no render scenario {name_or_path!r}; "
                            f"known: {', '.join(known) or '-'}")


def terrain_store_root() -> Path:
    """Terrain's store: $EMBER_TERRAIN_STORE, else the sibling checkout ../Terrain/store."""
    env = os.environ.get("EMBER_TERRAIN_STORE")
    if env:
        return Path(env)
    return Path(__file__).resolve().parents[3] / "Terrain" / "store"


def resolve_world(ref: str, base: Path) -> Path:
    if ref.startswith("terrain:"):
        return (terrain_store_root() / ref.removeprefix("terrain:")).resolve()
    return (base / ref).resolve()


def load_scenario(path: Path) -> LoadedScenario:
    with open(path, "rb") as f:
        raw = tomllib.load(f)
    spec = RenderScenario.model_validate(raw)
    base = path.parent
    world = resolve_world(spec.scenario.world, base)
    replay = (base / spec.scenario.replay).resolve() if spec.scenario.replay else None
    return LoadedScenario(path=path.resolve(), spec=spec, world_path=world, replay_path=replay)

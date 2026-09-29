"""ember-dev harness (EPIC_5_PLAN A1): scenarios, image diff, facts/budgets, evaluate, bless,
build-log parsing. Engine-free: runs are synthesised on disk."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

from ember.dev import facts as F
from ember.dev.evaluate import bless, evaluate_run
from ember.dev.imagediff import GOLDEN_MAX_WIDTH, compare
from ember.dev.scenario import RenderScenario, load_scenario
from ember.dev.ue import parse_build_log

SCENARIO = """
render_scenario_version = 1
[scenario]
name = "S_test"
world = "world_dir"
resolution = [320, 180]
budget = "interactive"
perf_frames = 10

[[bookmarks]]
name = "a"
target_frac = [0.5, 0.5]
distance_m = 1000

[[captures]]
name = "a"
bookmark = "a"

[[captures]]
name = "b"
bookmark = "a"

[[assert]]
fact = "tiles.loaded"
op = "=="
value = 9
"""


def _img(path: Path, seed: int = 0, w: int = 320, h: int = 180, patch: bool = False) -> None:
    rng = np.random.default_rng(seed)
    # Smooth-ish structure so SSIM behaves like it does on renders.
    base = np.linspace(0, 200, w)[None, :] + np.linspace(0, 50, h)[:, None]
    noise = rng.normal(0, 3, (h, w))
    a = np.clip(base + noise, 0, 255)
    if patch:  # a "missing tile": one block goes flat black
        a[40:100, 60:140] = 0
    rgb = np.repeat(a[..., None], 3, axis=2).astype(np.uint8)
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgb).save(path)


def _facts(path: Path, capture: str, loaded: int = 9, **perf) -> None:
    d = {"format": "ember-scene-facts", "version": 1, "scenario": "S_test", "capture": capture,
         "tiles": {"loaded": loaded}, "render": {"vram_mb": 900.0},
         "perf": {"frame_ms_p95": 5.0, **perf}}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(d), encoding="utf-8")


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    (tmp_path / "viz" / "scenarios").mkdir(parents=True)
    (tmp_path / "viz" / "scenarios" / "S_test.toml").write_text(SCENARIO, encoding="utf-8")
    (tmp_path / "viz" / "budgets.toml").write_text(
        '[interactive]\nperf__frame_ms_p95_max = 16.7\nrender__vram_mb_max = 4096\n',
        encoding="utf-8")
    return tmp_path


def _run(repo: Path, name: str = "r1", *, patch_b: bool = False, loaded: int = 9,
         p95: float = 5.0, exit_code: int = 0) -> Path:
    run = repo / "runs" / name
    _img(run / "captures" / "a.png", 0)
    _img(run / "captures" / "b.png", 1, patch=patch_b)
    _facts(run / "facts" / "a.json", "a", loaded)
    _facts(run / "facts" / "b.json", "b", loaded)
    _facts(run / "facts" / "perf.json", "perf", loaded, frame_ms_p95=p95)
    (run / "run.json").write_text(json.dumps({"exit_code": exit_code, "error": ""}),
                                  encoding="utf-8")
    return run


def test_scenario_validation(repo: Path):
    sc = load_scenario(repo / "viz" / "scenarios" / "S_test.toml")
    assert sc.name == "S_test"
    assert sc.world_path.name == "world_dir"
    assert sc.spec.scenario.exposure_bias == -2.0
    bad = {"render_scenario_version": 1,
           "scenario": {"name": "x", "world": "w"},
           "bookmarks": [{"name": "a", "distance_m": 10}],   # no target
           "captures": []}
    with pytest.raises(ValueError):
        RenderScenario.model_validate(bad)
    bad = {"render_scenario_version": 1, "scenario": {"name": "x", "world": "w"},
           "bookmarks": [{"name": "a", "target_frac": [0.5, 0.5], "distance_m": 10}],
           "captures": [{"name": "c", "bookmark": "nope"}]}
    with pytest.raises(ValueError, match="unknown bookmark"):
        RenderScenario.model_validate(bad)
    bad["captures"] = [{"name": "c", "bookmark": "a", "t_s": 60}]
    with pytest.raises(ValueError, match="no replay"):
        RenderScenario.model_validate(bad)


def test_terrain_world_ref_resolves_against_store(monkeypatch, tmp_path: Path):
    from ember.dev.scenario import resolve_world

    monkeypatch.setenv("EMBER_TERRAIN_STORE", str(tmp_path / "store"))
    want = (tmp_path / "store" / "teanaway_dev").resolve()
    assert resolve_world("terrain:teanaway_dev", Path(".")) == want


def test_imagediff_identical_noise_and_local_break(tmp_path: Path):
    _img(tmp_path / "g.png", 0)
    _img(tmp_path / "same.png", 0)
    _img(tmp_path / "noisy.png", 7)       # same structure, different noise
    _img(tmp_path / "broken.png", 0, patch=True)
    same = compare(tmp_path / "same.png", tmp_path / "g.png", ssim_min=0.97, region_ssim_min=0.9)
    assert same.ok and same.ssim == pytest.approx(1.0)
    broken = compare(tmp_path / "broken.png", tmp_path / "g.png", ssim_min=0.97,
                     region_ssim_min=0.9, heatmap_out=tmp_path / "heat.png")
    assert not broken.ok
    assert "region" in broken.reason           # the local break is what trips it
    assert broken.region_ssim_min < broken.ssim  # region minimum is more sensitive than global
    assert (tmp_path / "heat.png").exists()
    noisy = compare(tmp_path / "noisy.png", tmp_path / "g.png", ssim_min=0.5, region_ssim_min=0.5)
    assert noisy.ssim < 1.0


def test_imagediff_downscaled_golden(tmp_path: Path):
    from ember.dev.imagediff import golden_image

    _img(tmp_path / "big.png", 0, w=2560, h=1440)
    g = golden_image(tmp_path / "big.png")
    assert g.width == GOLDEN_MAX_WIDTH
    g.save(tmp_path / "g.png")
    assert compare(tmp_path / "big.png", tmp_path / "g.png", ssim_min=0.999,
                   region_ssim_min=0.999).ok
    _img(tmp_path / "wrong_aspect.png", 0, w=640, h=640)
    assert not compare(tmp_path / "wrong_aspect.png", tmp_path / "g.png",
                       ssim_min=0.5, region_ssim_min=0.5).ok


def test_facts_asserts_and_budgets():
    facts = {"tiles": {"loaded": 9}, "perf": {"frame_ms_p95": 20.0}, "render": {"vram_mb": 900}}
    assert F.eval_assert(facts, "a", "tiles.loaded", "==", 9).ok
    assert not F.eval_assert(facts, "a", "tiles.loaded", ">", 9).ok
    missing = F.eval_assert(facts, "a", "tiles.nope", "==", 1)
    assert not missing.ok and "missing" in missing.expected
    checks = F.eval_budgets({"perf__frame_ms_p95_max": 16.7, "render__vram_mb_max": 4096,
                             "description": "x"}, facts, "perf")
    assert [c.ok for c in checks] == [False, True]


def test_evaluate_new_then_bless_then_match(repo: Path):
    sc = load_scenario(repo / "viz" / "scenarios" / "S_test.toml")
    run = _run(repo)
    v = evaluate_run(repo, sc, run)
    assert v["pass"]                       # new goldens are not failures
    assert v["summary"]["new"] == 2
    assert (run / "contact_sheet.png").exists()
    assert sorted(bless(repo, sc, run)) == ["a", "b"]
    v = evaluate_run(repo, sc, _run(repo, "r2"))
    assert v["pass"] and v["summary"]["match"] == 2


def test_evaluate_catches_image_fact_budget_and_run_failures(repo: Path):
    sc = load_scenario(repo / "viz" / "scenarios" / "S_test.toml")
    bless(repo, sc, _run(repo))
    v = evaluate_run(repo, sc, _run(repo, "img", patch_b=True))
    assert not v["pass"] and v["summary"]["mismatch"] == 1
    v = evaluate_run(repo, sc, _run(repo, "fact", loaded=8))
    assert not v["pass"]
    assert any(c["name"] == "tiles.loaded" and not c["ok"] for c in v["checks"])
    v = evaluate_run(repo, sc, _run(repo, "budget", p95=30.0))
    assert not v["pass"]
    assert any(c["kind"] == "budget" and not c["ok"] for c in v["checks"])
    v = evaluate_run(repo, sc, _run(repo, "crash", exit_code=2))
    assert not v["pass"]
    run = _run(repo, "gone")
    (run / "captures" / "b.png").unlink()
    v = evaluate_run(repo, sc, run)
    assert not v["pass"] and v["summary"]["missing"] == 1


def test_parse_build_log():
    log = r"""
Building EmberEditor...
C:\Projects\Ember\unreal\Ember\Source\Ember\Private\EmberEnvironment.cpp(12,1): fatal error C1083: Cannot open include file: 'SkyAtmosphere.h': No such file or directory
C:\Projects\Ember\unreal\Ember\Source\Ember\Private\A.cpp(3): warning C4996: 'x': deprecated
C:\Projects\Ember\unreal\Ember\Source\Ember\Private\A.cpp(3): warning C4996: 'x': deprecated
EmberHarness.cpp.obj : error LNK2019: unresolved external symbol "foo"
Result: Failed (OtherCompilationError)
"""
    errors, warnings, other, result, reason = parse_build_log(log)
    assert len(errors) == 1 and errors[0].code == "C1083" and errors[0].line == 12
    assert len(warnings) == 1                      # duplicates collapsed
    assert any("LNK2019" in o for o in other)
    assert (result, reason) == ("Failed", "OtherCompilationError")


def test_asset_lock_detects_stale_generator_and_unclaimed_asset(tmp_path: Path):
    import hashlib

    from ember.dev.assets import LOCK, check_lock

    gen = tmp_path / "assets" / "generators"
    gen.mkdir(parents=True)
    (gen / "g.py").write_bytes(b"print('gen')\r\n")          # CRLF working copy
    (gen / "manifest.toml").write_text('[[generator]]\nscript = "g.py"\n'
                                       'outputs = ["/Game/Ember/Generated/M_A"]\n')
    out = tmp_path / "unreal" / "Ember" / "Content" / "Ember" / "Generated"
    out.mkdir(parents=True)
    (out / "M_A.uasset").write_bytes(b"\x00")
    lf_sha = hashlib.sha256(b"print('gen')\n").hexdigest()   # CI checks out LF
    lock = {"format": "ember-generated-lock", "version": 1, "engine": "5.8.3",
            "generators": {"g.py": {"script_sha256": lf_sha, "outputs": {
                "/Game/Ember/Generated/M_A": "unreal/Ember/Content/Ember/Generated/M_A.uasset"}}}}
    (tmp_path / LOCK).write_text(json.dumps(lock))
    assert check_lock(tmp_path) == []
    (gen / "g.py").write_bytes(b"print('edited')\n")
    assert any("changed since last regen" in p for p in check_lock(tmp_path))
    (gen / "g.py").write_bytes(b"print('gen')\n")
    (out / "M_Hand.uasset").write_bytes(b"\x00")               # hand-made asset sneaks in
    assert any("no generator claims" in p for p in check_lock(tmp_path))
    (tmp_path / LOCK).unlink()
    assert any("missing" in p for p in check_lock(tmp_path))


def test_ue_module_compiles_every_worldcore_source():
    """WorldCoreUnity.cpp must include every worldcore/src/*.cpp (else: link errors in UE only)."""
    repo = Path(__file__).resolve().parents[1]
    unity = (repo / "unreal/Ember/Source/EmberWorld/Private/WorldCoreUnity.cpp").read_text()
    missing = [p.name for p in (repo / "worldcore/src").glob("*.cpp")
               if f'#include "src/{p.name}"' not in unity]
    assert not missing, f"add to WorldCoreUnity.cpp: {missing}"


def test_orbit_validation():
    base = {"render_scenario_version": 1, "scenario": {"name": "x", "world": "w"},
            "bookmarks": [{"name": "a", "target_frac": [0.5, 0.5], "distance_m": 10}],
            "captures": []}
    ok = RenderScenario.model_validate({**base, "orbits": [{"name": "o", "bookmark": "a"}]})
    assert ok.orbits[0].frames == 240 and ok.orbits[0].degrees == 360
    with pytest.raises(ValueError, match="unknown bookmark"):
        RenderScenario.model_validate({**base, "orbits": [{"name": "o", "bookmark": "zz"}]})
    with pytest.raises(ValueError):
        RenderScenario.model_validate({**base, "orbits": [{"name": "o", "bookmark": "a",
                                                           "frames": 1}]})
    fly = RenderScenario.model_validate(
        {**base, "bookmarks": base["bookmarks"] + [{"name": "b", "target_frac": [0.6, 0.5],
                                                     "distance_m": 10}],
         "orbits": [{"name": "f", "bookmark": "a", "to_bookmark": "b"}]})
    assert fly.orbits[0].to_bookmark == "b"
    with pytest.raises(ValueError, match="unknown bookmark"):
        RenderScenario.model_validate({**base, "orbits": [{"name": "f", "bookmark": "a",
                                                           "to_bookmark": "zz"}]})


def test_forest_report_synthetic(tmp_path: Path):
    """Density vs the accept rule, species shares, crown cover and outside-mask counting."""
    rasterio = pytest.importorskip("rasterio")
    from rasterio.transform import from_origin

    from ember.dev import forest

    region = tmp_path / "store" / "reg"
    (region / "veg").mkdir(parents=True)
    (region / "fuels").mkdir()
    (region / "canopy").mkdir()
    tr = from_origin(0.0, 20.0, 10.0, 10.0)            # 2 x 2 cells of 10 m
    cc = np.array([[50.0, 50.0], [0.0, -9999.0]], dtype=np.float32)
    chm = np.full((2, 2), 30.0, dtype=np.float32)
    for name, arr in (("fuels/cc.tif", cc), ("canopy/chm.tif", chm)):
        with rasterio.open(region / name, "w", driver="GTiff", height=2, width=2, count=1,
                           dtype="float32", crs="EPSG:32610", transform=tr, nodata=-9999.0) as w:
            w.write(arr, 1)
    species = "".join(f'[[groups.species]]\nkey = "{k}"\nweight = {w}\nheight_min_m = 1.0\n'
                      f'height_max_m = 50.0\nradius_m = 1.0\n' for k, w in (("a", 3), ("b", 1)))
    (region / "pal.toml").write_text(
        'name = "t"\n[[groups]]\nname = "conifer_forest"\nevt_min = 0\nevt_max = 9\n' + species,
        encoding="utf-8")
    (region / "veg/scatter.input.json").write_text(json.dumps({   # Terrain writes backslashes
        "palette": "pal.toml", "candidates_per_cell": 4, "cell_size_m": 10.0,
        "rasters": {"cc": "fuels\\cc.tif", "height": "canopy\\chm.tif"}}), encoding="utf-8")
    dt = [("x", "f8"), ("y", "f8"), ("z", "f4"), ("species", "u2"), ("height", "f4"),
          ("yaw", "f4"), ("scale", "f4"), ("radius", "f4")]
    rows = [(2, 18, 900, 0, 30, 0, 1, 1), (5, 15, 900, 0, 30, 0, 1, 1),   # cell (0,0): 2 trees
            (12, 18, 900, 1, 30, 0, 1, 1), (15, 15, 900, 0, 30, 0, 1, 1),  # cell (0,1): 2 trees
            (15, 5, 0, 0, 30, 0, 1, 1)]                                    # outside the DEM mask
    np.save(region / "veg/instances.npy", np.array(rows, dtype=dt))
    rep, _ = forest.report(region)
    assert rep["instances"] == 5 and rep["outside_dem_mask"] == 1
    b = rep["density_by_cc"]
    assert len(b) == 1 and b[0]["trees_per_cell"] == 2.0 and b[0]["expected_trees_per_cell"] == 2.0
    assert [s["share_of_group"] for s in rep["species"]] == [0.8, 0.2]
    assert [s["expected_share"] for s in rep["species"]] == [0.75, 0.25]
    # two trees of crown radius 1 m per 100 m2 cell: 1 - exp(-2 pi / 100)
    assert b[0]["rendered_crown_cover_pct"] == pytest.approx(6.09, abs=0.02)


def test_water_levels_reservoir_ring_and_river():
    from ember.dev.water import NODATA, derive_levels

    h, w = 120, 200
    dem = np.full((h, w), 900.0, dtype=np.float32)
    fb = np.full((h, w), 165, dtype=np.int32)
    # Reservoir: full-pool extent classed water; interior is a DEM hole (no LiDAR returns);
    # an exposed "bathtub ring" of lakebed returns sits 1-8 m above the true water line (500).
    yy, xx = np.mgrid[0:h, 0:w]
    lake = (yy - 60) ** 2 + (xx - 50) ** 2 < 30 ** 2
    fb[lake] = 98
    core = (yy - 60) ** 2 + (xx - 50) ** 2 < 24 ** 2
    dem[lake & ~core] = 500.0 + ((((yy - 60) ** 2 + (xx - 50) ** 2) ** 0.5 - 24) * 1.3)[lake & ~core]
    dem[core] = NODATA
    # River: a 3-cell channel of water returns at a 1 % grade (Cle Elum is < 1 %), x 110..190.
    river = (yy >= 58) & (yy <= 60) & (xx >= 110) & (xx < 190)
    fb[river] = 98
    dem[river] = (760.0 - (xx - 110) * 0.1)[river]
    level, bodies = derive_levels(dem, NODATA, fb)
    assert len(bodies) == 2
    lk = level[60, 50]
    assert abs(lk - 500.0) < 2.5, lk            # water line, not the median of the ring
    assert np.isfinite(level[core]).all()       # the hole is covered
    up, down = level[59, 115], level[59, 185]
    assert up - down > 5                        # the river's surface follows its gradient
    assert abs(up - dem[59, 115]) < 1.5 and abs(down - dem[59, 185]) < 1.5


def test_treegen_species_build_deterministically_within_budget():
    """B3 v2 growth-form trees: every species builds, deterministically, inside a triangle
    budget, with foliage (alpha 1) and wood (alpha 0) both present, standing on the origin."""
    import sys
    repo = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(repo / "assets" / "generators"))
    import treegen

    for key in treegen.SPECIES:
        a = treegen.build(key, 0)
        b = treegen.build(key, 0)
        assert a.verts == b.verts and a.tris == b.tris, key
        assert 200 < len(a.tris) <= 120_000, (key, len(a.tris))
        alphas = {c[3] for c in a.cols}
        assert 1.0 in alphas and alphas <= {0.0, 1.0}, (key, alphas)   # foliage flagged
        if key != "bunchgrass":
            assert 0.0 in alphas, key                                  # ... and wood
        lo, hi = a.bounds()
        assert abs(lo[2]) < 1.0 and hi[2] > 0.5 * treegen.H, key
        assert max(i for t in a.tris for i in t) < len(a.verts)
        if treegen.VARIANTS > 1:
            assert treegen.build(key, 1).verts != a.verts, key  # variants differ


def test_manifest_lists_every_treegen_asset():
    import sys
    import tomllib
    repo = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(repo / "assets" / "generators"))
    import treegen

    m = tomllib.loads((repo / "assets/generators/manifest.toml").read_text(encoding="utf-8"))
    veg = next(g for g in m["generator"] if g["script"] == "veg_species.py")
    assert veg["outputs"] == [f"/Game/Ember/Generated/{n}" for n in treegen.asset_names()]
    assert veg["depends"] == ["treegen.py"]
    order = [g["script"] for g in m["generator"]]
    assert order.index("m_veg.py") < order.index("veg_species.py")  # instances need their parent


def test_treegen_lite_keeps_the_tree_and_drops_foliage():
    """Mid-tier lite meshes: same trunk and branches (so no pop at the tier boundary), fewer
    foliage triangles, the same crown extent within 10 %."""
    import sys
    repo = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(repo / "assets" / "generators"))
    import treegen

    for key in treegen.SPECIES:
        if not treegen.has_lite(key):
            continue
        full, lite = treegen.build(key, 1), treegen.build(key, 1, treegen.LITE_DETAIL)
        wood = lambda m: [v for v, c in zip(m.verts, m.cols, strict=True) if c[3] == 0.0]  # noqa: E731
        assert wood(full) == wood(lite), key                    # identical trunk + branches
        assert len(lite.tris) < 0.7 * len(full.tris), key
        (flo, fhi), (llo, lhi) = full.bounds(), lite.bounds()
        for ax in (0, 1, 2):
            assert abs((lhi[ax] - llo[ax]) - (fhi[ax] - flo[ax])) <= 0.1 * (fhi[ax] - flo[ax]), key


def test_synth_fire_obeys_the_state_stream_rules(tmp_path: Path):
    """The synthetic replay is a valid stream by Epic 4's own rules: phases monotone, arrival
    written once, non-burnable fuels never burn, the burned area only grows."""
    rasterio = pytest.importorskip("rasterio")
    from rasterio.transform import from_origin

    from ember.dev import firesynth
    from ember.sim.stream import iter_frames, read_replay

    region = tmp_path / "reg"
    (region / "fuels").mkdir(parents=True)
    fb = np.full((90, 120), 165, dtype=np.uint8)          # TU5 everywhere ...
    fb[40:50, :] = 99                                     # ... a strip of bare rock
    with rasterio.open(region / "fuels/fbfm40.cog.tif", "w", driver="GTiff", height=90, width=120,
                       count=1, dtype="uint8", crs="EPSG:32610",
                       transform=from_origin(600000.0, 5200000.0, 10.0, 10.0)) as w:
        w.write(fb, 1)
    path = firesynth.synth(region, tmp_path / "out", ignition_frac=(0.5, 0.25), hours=12,
                           head_ms=0.05)
    replay = read_replay(path)
    assert replay["world"]["grid"]["nx"] == 40 and replay["world"]["grid"]["ny"] == 30
    prev_phase, prev_burned, rock = None, -1, None
    for _, f in iter_frames(path.parent / replay["stream"]):
        if rock is None:
            rock = f.phase == 0
            assert rock[13:17].all()                     # the rock rows (30 m cells) are unburnable
        assert not (f.phase[rock] > 0).any()
        if prev_phase is not None:
            assert (f.phase >= prev_phase).all()          # monotone
        burned = int((f.phase >= 2).sum())
        assert burned >= prev_burned
        prev_phase, prev_burned = f.phase, burned
    assert prev_burned > 0


def test_fire_probes_validation():
    base = {"render_scenario_version": 1,
            "scenario": {"name": "x", "world": "w", "replay": "r.replay.json"},
            "bookmarks": [{"name": "a", "target_frac": [0.5, 0.5], "distance_m": 10}],
            "captures": []}
    probe = {"name": "p", "x": 1.0, "y": 2.0}
    ok = RenderScenario.model_validate({**base, "fire_probes": [probe]})
    assert ok.fire_probes[0].x == 1.0 and ok.scenario.smoke
    with pytest.raises(ValueError, match="duplicate fire probe"):
        RenderScenario.model_validate({**base, "fire_probes": [probe, probe]})
    no_replay = {**base, "scenario": {"name": "x", "world": "w"}}
    with pytest.raises(ValueError, match="need a replay"):
        RenderScenario.model_validate({**no_replay, "fire_probes": [probe]})


def test_split_crop_centres_the_grid(tmp_path: Path):
    from ember.dev.scenario import LoadedScenario
    from ember.dev.split import grid_crop

    grid = {"origin_x": 1000.0, "origin_y": 5000.0, "nx": 80, "ny": 60, "cell_size_m": 30.0}
    replay = tmp_path / "f.replay.json"
    replay.write_text(json.dumps({"world": {"grid": grid}}), encoding="utf-8")
    (tmp_path / "run" / "facts").mkdir(parents=True)
    extent = [1000.0, 5000.0 - 1800.0, 1000.0 + 2400.0, 5000.0]   # the grid, exactly
    (tmp_path / "run" / "facts" / "a.json").write_text(
        json.dumps({"world": {"data_extent_m": extent}}), encoding="utf-8")

    def scenario(pitch: float, distance: float) -> LoadedScenario:
        spec = RenderScenario.model_validate({
            "render_scenario_version": 1,
            "scenario": {"name": "x", "world": "w", "replay": "f.replay.json",
                         "resolution": [1600, 900]},
            "bookmarks": [{"name": "map", "target_frac": [0.5, 0.5], "distance_m": distance,
                           "pitch_deg": pitch, "fov_deg": 50}],
            "captures": []})
        return LoadedScenario(path=tmp_path / "x.toml", spec=spec, world_path=tmp_path,
                              replay_path=replay)

    c = grid_crop(scenario(-89, 3600.0), tmp_path / "run", "map")
    assert abs(c.x + c.w / 2 - 800) <= 1 and abs(c.y + c.h / 2 - 450) <= 1
    assert abs(c.w / c.h - 2400 / 1800) < 0.01
    assert abs(c.w - 1600 * 2400 / (2 * 3600 * np.tan(np.radians(25)))) <= 1
    with pytest.raises(ValueError, match="does not fit"):
        grid_crop(scenario(-89, 1000.0), tmp_path / "run", "map")
    with pytest.raises(ValueError, match="straight down"):
        grid_crop(scenario(-40, 3600.0), tmp_path / "run", "map")

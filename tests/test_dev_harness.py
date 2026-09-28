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
    assert any("script changed" in p for p in check_lock(tmp_path))
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

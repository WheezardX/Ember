"""Assets-from-code (EPIC_5_PLAN §3, B1): run generator scripts headless and keep a lock.

The lock (assets/generated.lock.json) records, per generator, the script's sha256 and the
files it produced. `check_lock` is engine-free so CI can enforce "no generator edited without
regenerating, no hand-made asset in the generated folder". Byte-identical .uasset output is
NOT asserted: UE packages embed GUIDs/timestamps, so cleanliness is defined on inputs.
"""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import time
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from ember.dev.ue import Engine, repo_root

GEN_DIR = "assets/generators"
LOCK = "assets/generated.lock.json"
CONTENT_PREFIX = "/Game/"


@dataclass
class Generator:
    script: Path
    outputs: list[str]
    depends: list[Path] = field(default_factory=list)  # modules the script imports


def load_manifest(repo: Path) -> list[Generator]:
    with open(repo / GEN_DIR / "manifest.toml", "rb") as f:
        m = tomllib.load(f)
    return [Generator(repo / GEN_DIR / g["script"], list(g["outputs"]),
                      [repo / GEN_DIR / d for d in g.get("depends", [])]) for g in m["generator"]]


def _sha(p: Path) -> str:
    # Line-ending-normalised: the runner's working copy may be CRLF, CI's checkout is LF.
    return hashlib.sha256(p.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def gen_sha(g: Generator) -> str:
    """The generator's input hash: its script, plus every declared dependency."""
    if not g.depends:
        return _sha(g.script)
    parts = [_sha(g.script)] + [f"{d.name}:{_sha(d)}" for d in sorted(g.depends)]
    return hashlib.sha256("|".join(parts).encode()).hexdigest()


def asset_file(eng: Engine, asset: str) -> Path:
    """/Game/Ember/Generated/M_X -> <project>/Content/Ember/Generated/M_X.uasset"""
    rel = asset.removeprefix(CONTENT_PREFIX)
    return eng.project.parent / "Content" / f"{rel}.uasset"


def run_generator(eng: Engine, gen: Generator, log_dir: Path) -> tuple[bool, str, float]:
    log_dir.mkdir(parents=True, exist_ok=True)
    log = log_dir / f"{gen.script.stem}.log"
    cmd = [str(eng.editor_cmd_exe), str(eng.project), "-run=pythonscript",
           f"-script={gen.script.as_posix()}", "-unattended", "-nosplash", "-nosound",
           "-NoLiveCoding", f"-abslog={log}"]
    # Generators that need numpy (texture synthesis) call back into this host Python.
    env = dict(os.environ)
    env["EMBER_HOST_PYTHON"] = sys.executable
    t = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace",
                       env=env)
    secs = time.perf_counter() - t
    text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else p.stdout
    missing = [a for a in gen.outputs if f"EMBER_GENERATED {a}" not in text]
    py_err = "Traceback (most recent call last)" in text or "LogPython: Error" in text
    ok = p.returncode == 0 and not missing and not py_err
    why = "" if ok else (f"exit {p.returncode}; missing {missing}; "
                         f"python error {py_err}; log {log}")
    return ok, why, secs


def regen(eng: Engine, repo: Path | None = None, force: bool = False) -> dict:
    """Run the generators whose inputs changed since the lock (script + declared dependencies),
    or whose outputs are missing; `force` runs them all. Tree meshes take ~20 min to rebuild, so
    unchanged generators are skipped."""
    repo = repo or repo_root()
    gens = load_manifest(repo)
    lock_path = repo / LOCK
    old = {}
    if lock_path.exists():
        old = json.loads(lock_path.read_text(encoding="utf-8"))["generators"]
    results = []
    all_ok = True
    for g in gens:
        files = [asset_file(eng, a) for a in g.outputs]
        entry = old.get(g.script.name)
        current = (not force and entry is not None and entry["script_sha256"] == gen_sha(g)
                   and sorted(entry["outputs"]) == sorted(g.outputs)
                   and all(f.exists() for f in files))
        if current:
            results.append({"script": g.script.name, "ok": True, "error": "", "seconds": 0.0,
                            "skipped": True, "script_sha256": entry["script_sha256"],
                            "outputs": entry["outputs"]})
            continue
        ok, why, secs = run_generator(eng, g, repo / "runs" / "dev" / "assets")
        present = all(f.exists() for f in files)
        ok = ok and present
        all_ok &= ok
        results.append({"script": g.script.name, "ok": ok, "error": why or ("" if present else
                        "output file missing"), "seconds": round(secs, 1), "skipped": False,
                        "script_sha256": gen_sha(g),
                        "outputs": {a: str(asset_file(eng, a).relative_to(repo)).replace("\\", "/")
                                    for a in g.outputs}})
    if all_ok:
        lock = {"format": "ember-generated-lock", "version": 1,
                "engine": eng.version,
                "generators": {r["script"]: {"script_sha256": r["script_sha256"],
                                             "outputs": r["outputs"]} for r in results}}
        lock_path.write_text(json.dumps(lock, indent=2) + "\n", encoding="utf-8")
    return {"ok": all_ok, "generators": results}


def check_lock(repo: Path | None = None, content_root: Path | None = None) -> list[str]:
    """Engine-free staleness check. Returns problems (empty = clean)."""
    repo = repo or repo_root()
    problems = []
    lock_path = repo / LOCK
    if not lock_path.exists():
        return [f"{LOCK} missing: run ember-dev regen-assets"]
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    gens = {g.script.name: g for g in load_manifest(repo)}
    claimed = set()
    for name, g in gens.items():
        entry = lock["generators"].get(name)
        if entry is None:
            problems.append(f"{name}: not in lock (never generated)")
            continue
        if entry["script_sha256"] != gen_sha(g):
            problems.append(f"{name}: script or a dependency changed since last regen")
        for rel in entry["outputs"].values():
            claimed.add(rel)
            if not (repo / rel).exists():
                problems.append(f"{name}: output {rel} missing")
    for name in lock["generators"]:
        if name not in gens:
            problems.append(f"{name}: in lock but not in manifest")
    gen_root = content_root or (repo / "unreal" / "Ember" / "Content" / "Ember" / "Generated")
    if gen_root.exists():
        for f in gen_root.rglob("*.uasset"):
            rel = str(f.relative_to(repo)).replace("\\", "/")
            if rel not in claimed:
                problems.append(f"{rel}: generated folder contains an asset no generator claims")
    return problems

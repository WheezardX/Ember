"""The engine side of `ember-dev`: pinned-engine config, UBT builds with parsed diagnostics,
and harness runs (`-game -RenderOffscreen` with a run plan). EPIC_5_PLAN §2 / A1 / A4."""

from __future__ import annotations

import json
import os
import re
import subprocess
import time
import tomllib
from dataclasses import asdict, dataclass, field
from datetime import datetime
from pathlib import Path

from ember.dev.scenario import LoadedScenario


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


@dataclass
class Engine:
    version: str
    changelist: int
    root: Path
    project: Path
    editor_target: str
    game_target: str

    @property
    def build_bat(self) -> Path:
        return self.root / "Engine" / "Build" / "BatchFiles" / "Build.bat"

    @property
    def editor_exe(self) -> Path:
        return self.root / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe"

    @property
    def editor_cmd_exe(self) -> Path:
        return self.root / "Engine" / "Binaries" / "Win64" / "UnrealEditor-Cmd.exe"

    def installed_version(self) -> tuple[str, int] | None:
        p = self.root / "Engine" / "Build" / "Build.version"
        if not p.exists():
            return None
        d = json.loads(p.read_text(encoding="utf-8"))
        return f"{d['MajorVersion']}.{d['MinorVersion']}.{d['PatchVersion']}", int(d["Changelist"])


def load_engine(repo: Path | None = None) -> Engine:
    repo = repo or repo_root()
    cfg_path = repo / "unreal" / "engine.toml"
    with open(cfg_path, "rb") as f:
        cfg = tomllib.load(f)
    root = Path(os.environ.get("EMBER_UE_ROOT", cfg["engine_root"]))
    return Engine(cfg["engine_version"], int(cfg["engine_changelist"]), root,
                  (cfg_path.parent / cfg["project"]).resolve(), cfg["editor_target"],
                  cfg["game_target"])


# ---------------------------------------------------------------------------------- build

_DIAG = re.compile(
    r"^(?P<file>[A-Za-z]:[^()]+?)\((?P<line>\d+)(?:,(?P<col>\d+))?\)\s*:\s*"
    r"(?P<kind>fatal error|error|warning)\s+(?P<code>[A-Z]+\d+)\s*:\s*(?P<msg>.*)$")
_RESULT = re.compile(r"^Result:\s*(?P<result>\w+)(?:\s*\((?P<why>[^)]*)\))?")


@dataclass
class Diagnostic:
    kind: str
    code: str
    file: str
    line: int
    col: int | None
    message: str


@dataclass
class BuildResult:
    ok: bool
    target: str
    config: str
    exit_code: int
    seconds: float
    result: str
    reason: str
    errors: list[Diagnostic] = field(default_factory=list)
    warnings: list[Diagnostic] = field(default_factory=list)
    other_errors: list[str] = field(default_factory=list)   # UBT/rules errors without a location
    log: str = ""

    def to_dict(self) -> dict:
        return asdict(self)


def parse_build_log(text: str) -> tuple[list[Diagnostic], list[Diagnostic], list[str], str, str]:
    errors, warnings, other = [], [], []
    seen = set()
    result, reason = "Unknown", ""
    for raw in text.splitlines():
        line = raw.strip()
        m = _DIAG.match(line)
        if m:
            key = (m["file"], m["line"], m["code"], m["msg"])
            if key in seen:
                continue
            seen.add(key)
            d = Diagnostic(m["kind"], m["code"], m["file"].strip(), int(m["line"]),
                           int(m["col"]) if m["col"] else None, m["msg"].strip())
            (warnings if d.kind == "warning" else errors).append(d)
            continue
        r = _RESULT.match(line)
        if r:
            result, reason = r["result"], r["why"] or ""
            continue
        # Location-less failures: UBT rules errors, linker errors (LNK), module-instantiation.
        if line not in other and (re.search(r"\berror\b", line, re.I) or
                                  line.lower().startswith("unable to instantiate")):
            if not re.search(r"\b0 error", line, re.I):
                other.append(line)
    return errors, warnings, other, result, reason


def build(eng: Engine, target: str | None = None, config: str = "Development",
          log_dir: Path | None = None) -> BuildResult:
    target = target or eng.editor_target
    log_dir = log_dir or (repo_root() / "runs" / "dev" / "build")
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / f"{target}-{config}.log"
    cmd = [str(eng.build_bat), target, "Win64", config, f"-Project={eng.project}",
           "-WaitMutex", "-NoHotReload", "-FromMsBuild"]
    t = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    secs = time.perf_counter() - t
    text = (p.stdout or "") + (p.stderr or "")
    log_path.write_text(text, encoding="utf-8")
    errors, warnings, other, result, reason = parse_build_log(text)
    res = BuildResult(p.returncode == 0, target, config, p.returncode, round(secs, 1), result,
                      reason, errors, warnings, other if p.returncode != 0 else [], str(log_path))
    (log_dir / "last.json").write_text(json.dumps(res.to_dict(), indent=2), encoding="utf-8")
    return res


# ------------------------------------------------------------------------------------ run

def _git_sha(repo: Path) -> str:
    try:
        sha = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=repo, capture_output=True,
                             text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "status", "--porcelain"], cwd=repo, capture_output=True,
                               text=True).stdout.strip()
        return sha + ("+dirty" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def _look_ref(look: str) -> str:
    """"clay" passes through; a look file resolves to an absolute path (repo-relative input)."""
    if look == "clay":
        return look
    return (repo_root() / look).resolve().as_posix()


def _water_dir(sc: LoadedScenario) -> str | None:
    """store/render/<region>/water if the scenario wants water and the layer exists."""
    if not sc.spec.scenario.water:
        return None
    d = repo_root() / "store" / "render" / sc.world_path.name / "water"
    return d.as_posix() if (d / "index.json").exists() else None


def write_run_plan(sc: LoadedScenario, run_dir: Path, exposure_bias: float = 0.0) -> Path:
    s = sc.spec
    plan = {
        "format": "ember-run-plan", "version": 1,
        "scenario": sc.name,
        "world": str(sc.world_path).replace("\\", "/"),
        "replay": str(sc.replay_path).replace("\\", "/") if sc.replay_path else None,
        "out_dir": str(run_dir).replace("\\", "/"),
        "resolution": list(s.scenario.resolution),
        "perf_frames": s.scenario.perf_frames,
        "exposure_bias": exposure_bias,
        "fixed_lod": s.scenario.fixed_lod,
        "lod_refine_factor": s.scenario.lod_refine_factor,
        "look": _look_ref(s.scenario.look),
        "vegetation": s.scenario.vegetation,
        "veg_radius_m": s.scenario.veg_radius_m,
        "wind_strength": s.scenario.wind_strength,
        "wind_from_deg": s.scenario.wind_from_deg,
        "veg_lineup": s.scenario.veg_lineup,
        "exec_cmds": list(s.scenario.exec_cmds),
        "perf_exec_cmds": list(s.scenario.perf_exec_cmds),
        "water_dir": _water_dir(sc),
        "perf_bookmark": s.scenario.perf_bookmark,
        "bookmarks": [b.model_dump() for b in s.bookmarks],
        "captures": [c.model_dump() for c in s.captures],
        "orbits": [o.model_dump() for o in s.orbits],
    }
    p = run_dir / "plan.json"
    p.write_text(json.dumps(plan, indent=2), encoding="utf-8")
    return p


def run_scenario(eng: Engine, sc: LoadedScenario, *, runs_root: Path | None = None,
                 timeout_s: int = 600, exposure_bias: float = 0.0,
                 extra_args: list[str] | None = None) -> Path:
    """Launch the harness for one scenario; returns the run directory (always written)."""
    repo = repo_root()
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = (runs_root or repo / "runs" / "viz") / sc.name / stamp
    (run_dir / "log").mkdir(parents=True, exist_ok=True)
    plan = write_run_plan(sc, run_dir, exposure_bias)
    w, h = sc.spec.scenario.resolution
    log = run_dir / "log" / "Ember.log"
    cmd = [str(eng.editor_exe), str(eng.project), "-game", "-RenderOffscreen", "-unattended",
           "-nosplash", "-nosound", "-NoLoadingScreen", "-windowed", f"-ResX={w}", f"-ResY={h}",
           f"-EmberRun={plan.as_posix()}", f"-abslog={log}", "-log", "-NoVerifyGC",
           "-ExecCmds=DisableAllScreenMessages"] + (extra_args or [])
    t = time.perf_counter()
    timed_out = False
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=timeout_s)
        code = p.returncode
    except subprocess.TimeoutExpired:
        timed_out, code = True, -1
    secs = time.perf_counter() - t
    status = {}
    sp = run_dir / "run_status.json"
    if sp.exists():
        status = json.loads(sp.read_text(encoding="utf-8"))
    # The harness's own status is authoritative; the process code is kept for diagnosis.
    exit_code = status.get("exit_code", code if code not in (0, None) else 4)
    error = status.get("error", "")
    if timed_out:
        exit_code, error = 5, f"timed out after {timeout_s}s"
    elif not status:
        error = error or ("harness wrote no run_status.json (crash or plan error; "
                          "see log/Ember.log)")
    orbits = encode_orbits(sc, run_dir) if exit_code == 0 else []
    inst = eng.installed_version()
    meta = {
        "format": "ember-viz-run", "version": 1, "scenario": sc.name,
        "scenario_file": str(sc.path), "world": str(sc.world_path),
        "started": stamp, "seconds": round(secs, 1), "process_exit_code": code,
        "exit_code": exit_code, "error": error, "git": _git_sha(repo),
        "engine": {"pinned": eng.version, "installed": inst[0] if inst else None},
        "command": cmd,
        "orbits": orbits,
    }
    (run_dir / "run.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    return run_dir


def encode_orbits(sc: LoadedScenario, run_dir: Path, keep_frames: bool = False) -> list[dict]:
    """frames/<orbit>/f%05d.png -> orbits/<orbit>.mp4 (H.264, yuv420p, crf 18) via ffmpeg."""
    import shutil

    out = []
    ff = shutil.which("ffmpeg")
    for o in sc.spec.orbits:
        frames = run_dir / "frames" / o.name
        mp4 = run_dir / "orbits" / f"{o.name}.mp4"
        entry = {"name": o.name, "frames": len(list(frames.glob("f*.png"))), "mp4": None,
                 "error": ""}
        if not ff:
            entry["error"] = "ffmpeg not on PATH"
        elif entry["frames"] == 0:
            entry["error"] = "no frames captured"
        else:
            mp4.parent.mkdir(parents=True, exist_ok=True)
            p = subprocess.run([ff, "-y", "-loglevel", "error", "-framerate", str(o.fps),
                                "-i", str(frames / "f%05d.png"), "-c:v", "libx264",
                                "-pix_fmt", "yuv420p", "-crf", "18", "-movflags", "+faststart",
                                str(mp4)], capture_output=True, text=True)
            if p.returncode == 0 and mp4.exists():
                entry["mp4"] = str(mp4.relative_to(run_dir)).replace("\\", "/")
                if not keep_frames:
                    shutil.rmtree(frames, ignore_errors=True)
            else:
                entry["error"] = (p.stderr or "ffmpeg failed").strip()[:400]
        out.append(entry)
    return out


def latest_run(sc_name: str, runs_root: Path | None = None) -> Path | None:
    base = (runs_root or repo_root() / "runs" / "viz") / sc_name
    if not base.exists():
        return None
    runs = sorted(p for p in base.iterdir() if p.is_dir())
    return runs[-1] if runs else None

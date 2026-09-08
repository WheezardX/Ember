"""CP4 driver: run every scenario in this directory with embersim, then build the paired
side-by-side MP4s and response-curve plots with `ember sim compare|curves` (G2).

    python sim/scenarios/cp4/run_matrix.py [--embersim sim/build/Release/embersim.exe] [--out checkpoints/CP4]

Groups (a naive viewer should be able to call the winner before each plays):
  slope   : flat vs 30 % ramp, calm            -> upslope side runs ahead
  wind    : 0 / 2 / 5 / 8 m/s from the west    -> ellipse stretches downwind, head accelerates
  fuel    : GR2 / SH5 / TU5 / TL3, 2 m/s       -> grass runs, shrub follows, timber smolders
  green   : cured vs green grass               -> cured runs, green crawls
  moist   : RH 12 % vs RH 60 %                 -> dry runs, humid crawls
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
GROUPS = {
    "slope": ["slope-flat", "slope-ramp30"],
    "wind": ["wind-0", "wind-2", "wind-5", "wind-8"],
    "fuel": ["fuel-gr2", "fuel-sh5", "fuel-tu5", "fuel-tl3"],
    "green": ["green-cured", "green-green"],
    "moist": ["moist-dry", "moist-humid"],
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--embersim", default=str(ROOT / "sim/build/Release/embersim.exe"))
    ap.add_argument("--out", default=str(ROOT / "checkpoints/CP4"))
    ap.add_argument("--every", type=int, default=5)
    ap.add_argument("--skip-run", action="store_true")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    py = sys.executable

    replays: dict[str, Path] = {}
    for group, names in GROUPS.items():
        for n in names:
            scen = HERE / f"{n}.scenario.toml"
            replay = ROOT / "runs/cp4" / f"cp4-{n}.replay.json"
            if not args.skip_run:
                print(f"== run {n}")
                subprocess.run([args.embersim, "run", str(scen), "--quiet"], check=True)
            replays[n] = replay
        runs = [f"{n}={replays[n]}" for n in names]
        gdir = out / group
        gdir.mkdir(exist_ok=True)
        print(f"== compare {group}")
        subprocess.run([py, "-m", "ember.cli", "sim", "compare", "--out", str(gdir),
                        "--every", str(args.every), "--mp4",
                        *sum((["--run", r] for r in runs), [])], check=True, cwd=ROOT)
        print(f"== curves {group}")
        subprocess.run([py, "-m", "ember.cli", "sim", "curves", "--out", str(gdir / f"{group}-curves.png"),
                        "--table", str(gdir / f"{group}-table.md"),
                        *sum((["--run", r] for r in runs), [])], check=True, cwd=ROOT)
    # summary of final hashes + ticks/s for the memo
    summary = {}
    for n, r in replays.items():
        j = json.loads(r.read_text(encoding="utf-8"))
        res = j["result"]
        summary[n] = {"final_hash": res["final_state_hash"], "ticks": res["ticks"],
                      "ticks_per_s": res.get("timing", {}).get("ticks_per_s")}
    (out / "runs.json").write_text(json.dumps(summary, indent=1), encoding="utf-8")
    print("done ->", out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""The night sky's star list (EPIC_5_PLAN: "Night sky: stars, the real sky for place and date").

Source: the Yale Bright Star Catalogue, 5th revised edition (Hoffleit & Warren 1991; CDS V/50,
public): every star to about V 6.5, i.e. what the eye sees on a dark night. The raw catalogue lands
in store/stars/ (metered in the household ledger, ~0.5 MB); the client reads a compact list,
unreal/Ember/Content/Ember/Data/stars.csv:
    ra_deg, dec_deg (J2000), vmag, b_v
brightest first, stars to V 6.0 (the client places them for the scene's latitude, date and sun).
"""
from __future__ import annotations

import gzip
import urllib.request
from pathlib import Path

URL = "https://cdsarc.cds.unistra.fr/ftp/V/50/catalog.gz"
MAG_LIMIT = 6.0


def _parse(line: str) -> tuple[float, float, float, float] | None:
    # byte columns (1-based) from the V/50 ReadMe:
    # J2000 RA 76-83, Dec 84-90, Vmag 103-107, B-V 110-114
    try:
        rah, ram, ras = int(line[75:77]), int(line[77:79]), float(line[79:83])
        sign = -1.0 if line[83] == "-" else 1.0
        ded, dem, des = int(line[84:86]), int(line[86:88]), int(line[88:90])
        vmag = float(line[102:107])
    except (ValueError, IndexError):
        return None  # a few entries (novae, extragalactic) have no position / magnitude
    try:
        bv = float(line[109:114])
    except ValueError:
        bv = 0.6
    ra = 15.0 * (rah + ram / 60.0 + ras / 3600.0)
    dec = sign * (ded + dem / 60.0 + des / 3600.0)
    return ra, dec, vmag, bv


def build(store: Path, out: Path) -> dict:
    raw = store / "stars" / "bsc5_catalog.gz"
    if not raw.exists():
        from terrain.work.netmeter import NetMeter

        raw.parent.mkdir(parents=True, exist_ok=True)
        import ssl

        import certifi

        # (CDS's chain is not in the Windows Python store: certifi's bundle verifies it)
        ctx = ssl.create_default_context(cafile=certifi.where())
        with NetMeter(r"C:\Projects\Terrain\store", "stars", "bsc5"):
            req = urllib.request.Request(URL, headers={"User-Agent": "ember-stars/0"})
            data = urllib.request.urlopen(req, timeout=120, context=ctx).read()
        raw.write_bytes(data)
    lines = gzip.decompress(raw.read_bytes()).decode("latin-1").splitlines()
    stars = [s for s in (_parse(ln) for ln in lines) if s and s[2] <= MAG_LIMIT]
    stars.sort(key=lambda s: s[2])
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# Yale Bright Star Catalogue 5 (CDS V/50), V <= 6.0: "
                "ra_deg, dec_deg (J2000), vmag, b_v\n")
        for ra, dec, v, bv in stars:
            f.write(f"{ra:.4f},{dec:.4f},{v:.2f},{bv:.2f}\n")
    return {"stars": len(stars), "raw_kb": round(raw.stat().st_size / 1024), "out": str(out)}

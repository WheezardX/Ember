"""Reference library (EPIC_5_PLAN 8i R3; docs/plans/REFERENCE_HARVEST_PLAN.md).

Images live in the Terrain store, outside git: <store>/reference/<source>/<id>.jpg, long edge
<= MAX_EDGE (originals are not kept - disk), EXIF preserved (date, lens, bearing, GPS). The
manifest is in git: viz/reference/manifest.jsonl, one JSON object per image:

    id, source, origin (URL / path), credit, licence ("shippable" | "reference-only"),
    taken (ISO, EXIF), lat / lon / bearing_deg / focal35_mm (EXIF, when present),
    w / h (stored), phash, tags [..], notes, added (ISO)

Licence classes: shippable = BNE-owned or a US federal work; reference-only = everything else
(look at it, never feed it into a shipped asset). Unknown = reference-only.
"""

from __future__ import annotations

import io
import json
from datetime import datetime
from pathlib import Path

from PIL import Image, ImageOps

MANIFEST = Path("viz") / "reference" / "manifest.jsonl"
MAX_EDGE = 2048
LICENCES = ("shippable", "reference-only")


def library_root() -> Path:
    from ember.dev.scenario import terrain_store_root

    return terrain_store_root() / "reference"


def load(repo: Path) -> list[dict]:
    p = repo / MANIFEST
    if not p.exists():
        return []
    return [json.loads(line) for line in p.read_text(encoding="utf-8").splitlines() if line.strip()]


def save(repo: Path, rows: list[dict]) -> None:
    p = repo / MANIFEST
    p.parent.mkdir(parents=True, exist_ok=True)
    rows = sorted(rows, key=lambda r: (r["source"], r.get("taken") or "", r["id"]))
    p.write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rows), encoding="utf-8")


def phash(im: Image.Image) -> str:
    """64-bit difference hash (dedupe; near-identical shots share most bits)."""
    g = im.convert("L").resize((9, 8), Image.LANCZOS)
    px = list(g.getdata())
    bits = sum(1 << i for i in range(64) if px[(i // 8) * 9 + i % 8] > px[(i // 8) * 9 + i % 8 + 1])
    return f"{bits:016x}"


def _exif_facts(im: Image.Image) -> dict:
    ex = im.getexif()
    sub = ex.get_ifd(0x8769)
    gps = ex.get_ifd(0x8825)
    out: dict = {}
    t = sub.get(0x9003) or ex.get(0x0132)
    if t:
        try:
            out["taken"] = datetime.strptime(str(t).strip("\x00"), "%Y:%m:%d %H:%M:%S").isoformat()
        except ValueError:
            pass
    if sub.get(0xA405):
        out["focal35_mm"] = float(sub[0xA405])
    if gps.get(17) is not None:
        out["bearing_deg"] = float(gps[17])
        out["bearing_ref"] = str(gps.get(16) or "")           # M = magnetic, T = true

    def dms(v, ref):
        d = float(v[0]) + float(v[1]) / 60 + float(v[2]) / 3600
        return -d if ref in ("S", "W") else d

    if gps.get(2) and gps.get(4):
        out["lat"] = round(dms(gps[2], gps.get(1)), 7)
        out["lon"] = round(dms(gps[4], gps.get(3)), 7)
    return out


def ingest(repo: Path, data: bytes, *, source: str, origin: str, credit: str, licence: str,
           tags: list[str], notes: str = "", taken: str | None = None) -> dict | None:
    """Store one image (downsized, EXIF kept) and add its manifest row. None if already there."""
    if licence not in LICENCES:
        raise ValueError(f"licence must be one of {LICENCES}")
    rows = load(repo)
    if any(r["origin"] == origin for r in rows):
        return None
    im = Image.open(io.BytesIO(data))
    exif = im.info.get("exif") or b""
    facts = _exif_facts(im)
    im = ImageOps.exif_transpose(im).convert("RGB") if exif else im.convert("RGB")
    if max(im.size) > MAX_EDGE:
        im.thumbnail((MAX_EDGE, MAX_EDGE), Image.LANCZOS)
    h = phash(im)
    rid = f"{source}-{(facts.get('taken') or taken or 'undated')[:10]}-{h[:8]}"
    dst = library_root() / source / f"{rid}.jpg"
    dst.parent.mkdir(parents=True, exist_ok=True)
    if exif:
        # orientation is baked in by exif_transpose: reset the tag so viewers do not rotate twice
        e = Image.Exif()
        e.load(exif)
        e[0x0112] = 1
        im.save(dst, "JPEG", quality=88, exif=e.tobytes())
    else:
        im.save(dst, "JPEG", quality=88)
    row = {"id": rid, "source": source, "origin": origin, "credit": credit, "licence": licence,
           "taken": facts.pop("taken", None) or taken, **facts, "w": im.width, "h": im.height,
           "phash": h, "tags": sorted(set(tags)), "notes": notes,
           "added": datetime.now().astimezone().isoformat(timespec="seconds"),
           "file": f"{source}/{rid}.jpg"}
    rows.append(row)
    save(repo, rows)
    return row


def add_files(repo: Path, paths: list[Path], **kw) -> list[dict]:
    out = []
    for p in paths:
        files = sorted(x for x in p.rglob("*") if x.suffix.lower() in (".jpg", ".jpeg", ".png", ".webp")) \
            if p.is_dir() else [p]
        for f in files:
            r = ingest(repo, f.read_bytes(), origin=str(f.resolve()), **kw)
            if r:
                out.append(r)
    return out


def sheet_html(repo: Path, tag: str | None = None, name: str = "sheet") -> Path:
    """Contact sheet (local file:// HTML) for Brad's keep / reject / tag pass.
    `tag` may list several tags, comma-separated: an image matches if it has any of them."""
    import html

    want = {t.strip() for t in tag.split(",")} if tag else set()
    rows = [r for r in load(repo) if not want or want & set(r["tags"])]
    root = library_root().as_uri()
    cells = []
    for r in rows:
        meta = " ".join(x for x in [(r.get("taken") or "")[:10], r["licence"],
                                    f"{r['bearing_deg']:.0f}°" if "bearing_deg" in r else "",
                                    "GPS" if "lat" in r else ""] if x)
        cells.append(f'<figure><a href="{root}/{r["file"]}"><img loading="lazy" src="{root}/{r["file"]}"></a>'
                     f'<figcaption><b>{html.escape(r["id"])}</b><br>{html.escape(meta)}<br>'
                     f'{html.escape(", ".join(r["tags"]))}<br><i>{html.escape(r.get("notes", ""))}</i>'
                     f'</figcaption></figure>')
    page = ("<!doctype html><meta charset=utf-8><title>Ember reference library</title><style>"
            "body{font:13px system-ui;background:#16181b;color:#e6e2da;margin:0;padding:16px}"
            ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:12px}"
            "figure{margin:0}img{width:100%;border-radius:3px;display:block}"
            "figcaption{color:#aaa;margin-top:4px;line-height:1.35}b{color:#e6e2da}</style>"
            f"<h1>Reference library</h1><p>{len(rows)} images{' tagged ' + html.escape(tag) if tag else ''}."
            " Keep / reject / retag: tell the agent the ids.</p><div class=grid>" + "".join(cells) + "</div>")
    out = library_root() / f"{name}.html"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(page, encoding="utf-8")
    return out

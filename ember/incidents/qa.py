"""F2 — per-incident QA report (self-contained HTML + inline SVG; no matplotlib).

Answers the D1-review question "do we believe this progression?" from one page: the
arrival-time raster as a color-banded heatmap (isochrones = its contours), fire growth
over time, the observation inventory (perimeters / hotspots / IR), the weather + world
pins, and any recorded gaps. Reads only what's already in the incident store — no
network, no re-fetch. Rendered as a standalone HTML file that opens in any browser.
"""

from __future__ import annotations

import html
import json
from datetime import datetime
from pathlib import Path

from terrain.util.logging import get_logger

from ember.incidents.arrival import ALGORITHM
from ember.incidents.model import BundleManifest, IncidentRecord, IncidentStore

log = get_logger(__name__)

# Perceptual-ish ramp early(bright)->late(dark red); arrival hours are binned onto it.
_RAMP = ["#fde725", "#addc30", "#5ec962", "#28ae80", "#21918c",
         "#2c728e", "#3b528b", "#472d7b", "#440154"]


def _fmt_dt(v) -> str:
    if isinstance(v, str):
        return v[:16]
    if isinstance(v, datetime):
        return v.isoformat()[:16]
    return "—"


def _arrival_svg(store: IncidentStore) -> str:
    """Color-banded heatmap of the arrival raster (burned cells only)."""
    try:
        import numpy as np
        import rasterio
    except ImportError:
        return "<p class='muted'>(rasterio unavailable — arrival heatmap skipped)</p>"

    path = store.derived(f"arrival_time.{ALGORITHM}.cog.tif")
    if not path.exists():
        return "<p class='muted'>(no arrival raster in store)</p>"

    with rasterio.open(path) as ds:
        nodata = ds.nodata
        scale = max(1.0, max(ds.width, ds.height) / 140.0)
        w, h = max(1, int(ds.width / scale)), max(1, int(ds.height / scale))
        arr = ds.read(1, out_shape=(h, w)).astype("float64")

    burned = arr != nodata if nodata is not None else np.isfinite(arr)
    if not burned.any():
        return "<p class='muted'>(arrival raster has no burned cells)</p>"
    vals = arr[burned]
    lo, hi = float(vals.min()), float(vals.max())
    span = (hi - lo) or 1.0

    rects = []
    for j in range(h):
        for i in range(w):
            if not burned[j, i]:
                continue
            frac = (arr[j, i] - lo) / span
            color = _RAMP[min(len(_RAMP) - 1, int(frac * len(_RAMP)))]
            rects.append(f'<rect x="{i}" y="{j}" width="1" height="1" fill="{color}"/>')
    legend = "".join(
        f'<span class="sw" style="background:{c}"></span>' for c in _RAMP)
    return (
        f'<svg viewBox="0 0 {w} {h}" class="heat" preserveAspectRatio="xMidYMid meet" '
        f'shape-rendering="crispEdges">{"".join(rects)}</svg>'
        f'<div class="legend"><span>t0 (+{lo:.0f} h)</span>{legend}'
        f'<span>+{hi:.0f} h</span></div>'
    )


def _size_svg(record: IncidentRecord) -> str:
    """Fire growth (acres over time) as a simple SVG area/line."""
    pts = [(p.at, p.acres) for p in record.size_series if p.at and p.acres is not None]
    if len(pts) < 2:
        return "<p class='muted'>(size series too short to plot)</p>"
    pts.sort(key=lambda x: x[0])
    t0 = pts[0][0]
    xs = [(t - t0).total_seconds() / 3600.0 for t, _ in pts]
    ys = [a for _, a in pts]
    xmax, ymax = (xs[-1] or 1.0), (max(ys) or 1.0)
    W, H = 320, 90
    pl = " ".join(f"{x / xmax * W:.1f},{H - a / ymax * H:.1f}"
                  for x, a in zip(xs, ys, strict=True))
    return (
        f'<svg viewBox="0 0 {W} {H}" class="spark" preserveAspectRatio="none">'
        f'<polyline points="{pl}" fill="none" stroke="#b5231f" stroke-width="2"/></svg>'
        f'<div class="cap">{ys[-1]:,.0f} ac over {xs[-1] / 24:.0f} d</div>'
    )


def _obs_rows(bundle: BundleManifest) -> str:
    from collections import Counter

    by_kind: Counter = Counter(o.kind for o in bundle.observations)
    src = sorted({o.source for o in bundle.observations})
    rows = "".join(f"<tr><td>{html.escape(k)}</td><td>{n}</td></tr>"
                   for k, n in sorted(by_kind.items()))
    return rows + f"<tr><td>sources</td><td>{html.escape(', '.join(src))}</td></tr>"


def build_qa_report(store: IncidentStore) -> Path:
    """Write ``qa.html`` into the incident store from its bundle; return the path."""
    bundle = BundleManifest.model_validate_json(store.bundle_json.read_text(encoding="utf-8"))
    record = IncidentRecord.model_validate_json(store.incident_json.read_text(encoding="utf-8"))
    arr = bundle.provenance.get("arrival", {})
    gaps = []
    if bundle.weather:
        wp = store.dir / bundle.weather
        if wp.exists():
            gaps = json.loads(wp.read_text(encoding="utf-8")).get("gaps", [])

    def esc(x):
        return html.escape(str(x))

    world = "—"
    if bundle.world_region:
        world = f"{esc(bundle.world_region)} · pin {esc((bundle.world_manifest_hash or '')[:12])}"

    gaps_html = ("<ul class='gaps'>" + "".join(f"<li>{esc(g)}</li>" for g in gaps[:12]) + "</ul>"
                 if gaps else "<p class='muted'>none recorded</p>")

    page = f"""<!doctype html><html lang="en"><meta charset="utf-8">
<title>QA · {esc(record.name)} {record.year}</title>
<style>
  body{{font:14px/1.5 system-ui,sans-serif;margin:0;background:#fafafa;color:#1a1a1a}}
  .wrap{{max-width:900px;margin:0 auto;padding:24px}}
  h1{{font-size:20px;margin:0 0 4px}} h2{{font-size:14px;text-transform:uppercase;
    letter-spacing:.05em;color:#666;margin:24px 0 8px}}
  .grid{{display:grid;grid-template-columns:1fr 1fr;gap:24px}}
  .card{{background:#fff;border:1px solid #e5e5e5;border-radius:8px;padding:16px}}
  table{{border-collapse:collapse;width:100%}} td{{padding:3px 8px;border-bottom:1px solid #f0f0f0}}
  td:first-child{{color:#666}} .muted{{color:#999;font-style:italic}}
  .heat{{width:100%;max-height:340px;background:#f3f3f3;border-radius:4px}}
  .legend{{display:flex;align-items:center;gap:2px;font-size:12px;color:#666;margin-top:6px}}
  .sw{{width:20px;height:12px;display:inline-block}} .legend span:first-child{{margin-right:6px}}
  .legend span:last-child{{margin-left:6px}}
  .spark{{width:100%;height:90px}} .cap{{font-size:12px;color:#666}}
  .gaps{{margin:0;padding-left:18px;color:#a15}}
  code{{background:#f0f0f0;padding:1px 4px;border-radius:3px}}
</style>
<div class="wrap">
  <h1>{esc(record.name)} — {record.year}</h1>
  <div class="muted">{esc(bundle.incident_id)} · generated {datetime.now().isoformat()[:16]}</div>

  <h2>Arrival time (isochrones = color bands)</h2>
  <div class="card">{_arrival_svg(store)}</div>

  <div class="grid">
    <div><h2>Incident</h2><div class="card"><table>
      <tr><td>discovered</td><td>{_fmt_dt(record.discovered_at)}</td></tr>
      <tr><td>contained</td><td>{_fmt_dt(record.contained_at)}</td></tr>
      <tr><td>final acres</td><td>{record.final_acres or '—'}</td></tr>
      <tr><td>burned km²</td><td>{arr.get('burned_km2', '—')}</td></tr>
      <tr><td>duration</td><td>{arr.get('duration_h', 0) / 24:.0f} d</td></tr>
      <tr><td>observed frac</td><td>{arr.get('observed_frac', '—')}</td></tr>
      <tr><td>world</td><td>{world}</td></tr>
    </table></div></div>

    <div><h2>Growth</h2><div class="card">{_size_svg(record)}</div>
      <h2>Observations</h2><div class="card"><table>{_obs_rows(bundle)}</table></div></div>
  </div>

  <h2>Weather / gaps</h2>
  <div class="card">
    <p>weather: <code>{esc(bundle.weather or 'none')}</code></p>
    <p>gaps:</p>{gaps_html}
  </div>
</div></html>"""
    out = store.dir / "qa.html"
    out.write_text(page, encoding="utf-8")
    log.info("qa report -> %s", out)
    return out

"""Reference library (ember.dev.reference): ingest downsizes, keeps EXIF facts, dedupes by origin."""

import io

from PIL import Image

from ember.dev import reference


def _jpeg(w=3000, h=2000, bearing=147.0) -> bytes:
    im = Image.new("RGB", (w, h), (40, 90, 50))
    for x in range(0, w, 100):
        im.paste((200, 180, 120), (x, 0, x + 40, h))
    ex = Image.Exif()
    sub = ex.get_ifd(0x8769)
    sub[0x9003] = "2025:04:13 12:48:37"   # DateTimeOriginal
    sub[0xA405] = 24                      # FocalLengthIn35mmFilm
    gps = ex.get_ifd(0x8825)
    gps[16] = "M"
    gps[17] = bearing
    buf = io.BytesIO()
    im.save(buf, "JPEG", exif=ex.tobytes())
    return buf.getvalue()


def test_ingest_downsizes_keeps_exif_and_dedupes(tmp_path, monkeypatch):
    monkeypatch.setenv("EMBER_TERRAIN_STORE", str(tmp_path / "store"))
    repo = tmp_path / "repo"
    kw = dict(source="test", origin="x:1", credit="me", licence="shippable", tags=["stand", "stand"])
    r = reference.ingest(repo, _jpeg(), **kw)
    assert r["w"] == reference.MAX_EDGE and r["h"] < reference.MAX_EDGE
    assert r["taken"] == "2025-04-13T12:48:37"
    assert r["focal35_mm"] == 24 and r["bearing_deg"] == 147.0 and "lat" not in r
    assert r["tags"] == ["stand"]
    stored = Image.open(tmp_path / "store" / "reference" / r["file"])
    assert stored.getexif().get_ifd(0x8769).get(0xA405) == 24   # EXIF survives the downsize
    assert reference.ingest(repo, _jpeg(), **kw) is None          # same origin: skipped
    assert len(reference.load(repo)) == 1


def test_unknown_licence_rejected(tmp_path, monkeypatch):
    monkeypatch.setenv("EMBER_TERRAIN_STORE", str(tmp_path / "store"))
    try:
        reference.ingest(tmp_path, _jpeg(), source="t", origin="o", credit="", licence="cc-by", tags=[])
    except ValueError:
        return
    raise AssertionError("licence outside the two classes must be rejected")

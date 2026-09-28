"""Golden-image comparison for render captures.

Metric: SSIM on luminance (Gaussian window, sigma 1.5 — Wang et al. 2004 constants),
reported globally and per region on a coarse grid. The region minimum is what catches
local breakage (a missing tile, a seam, a black Niagara quad) that a global mean would
average away. Implemented with scipy so the harness adds no dependencies.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter

_C1 = (0.01 * 255) ** 2
_C2 = (0.03 * 255) ** 2


# Goldens are stored downscaled (repo size); captures are diffed at this size.
GOLDEN_MAX_WIDTH = 1280


def golden_image(capture: Path) -> Image.Image:
    """The form a capture takes as a golden: RGB, BOX-downscaled to <= GOLDEN_MAX_WIDTH wide."""
    im = Image.open(capture).convert("RGB")
    if im.width > GOLDEN_MAX_WIDTH:
        h = round(im.height * GOLDEN_MAX_WIDTH / im.width)
        im = im.resize((GOLDEN_MAX_WIDTH, h), Image.Resampling.BOX)
    return im


def _luma(im: Image.Image) -> np.ndarray:
    rgb = np.asarray(im.convert("RGB"), dtype=np.float64)
    # Elementwise, not `rgb @ w`: matmul needs the BLAS DLL, which only loads when the conda
    # env is activated (Library\bin on PATH); calling the env's python.exe bare crashes with
    # 0xc06d007f. Nothing here needs BLAS, so stay robust to an unactivated env.
    return 0.299 * rgb[..., 0] + 0.587 * rgb[..., 1] + 0.114 * rgb[..., 2]


def load_luma(path: Path) -> np.ndarray:
    """RGB(A) image -> float64 luma (Rec. 601), 0..255."""
    return _luma(Image.open(path))


def ssim_map(a: np.ndarray, b: np.ndarray, sigma: float = 1.5) -> np.ndarray:
    mu_a = gaussian_filter(a, sigma)
    mu_b = gaussian_filter(b, sigma)
    aa = gaussian_filter(a * a, sigma) - mu_a * mu_a
    bb = gaussian_filter(b * b, sigma) - mu_b * mu_b
    ab = gaussian_filter(a * b, sigma) - mu_a * mu_b
    num = (2 * mu_a * mu_b + _C1) * (2 * ab + _C2)
    den = (mu_a * mu_a + mu_b * mu_b + _C1) * (aa + bb + _C2)
    return num / den


@dataclass
class DiffResult:
    ok: bool
    reason: str
    ssim: float
    region_ssim_min: float
    worst_region: tuple[int, int]      # (col, row) in the region grid
    mean_abs_luma: float
    grid: tuple[int, int]

    def to_dict(self) -> dict:
        return asdict(self)


def compare(capture: Path, golden: Path, *, ssim_min: float, region_ssim_min: float,
            grid: tuple[int, int] = (4, 4), heatmap_out: Path | None = None) -> DiffResult:
    b_im = Image.open(golden)
    a_im = golden_image(capture)
    if a_im.size != b_im.size:
        # Same aspect, different storage size (e.g. an older golden): resize the capture.
        ca, ga = a_im.width / a_im.height, b_im.width / b_im.height
        if abs(ca - ga) < 0.01:
            a_im = Image.open(capture).convert("RGB").resize(b_im.size, Image.Resampling.BOX)
    a = _luma(a_im)
    b = _luma(b_im)
    if a.shape != b.shape:
        return DiffResult(False, f"size mismatch {a.shape[::-1]} vs golden {b.shape[::-1]}",
                          0.0, 0.0, (0, 0), float("nan"), grid)
    m = ssim_map(a, b)
    h, w = m.shape
    gx, gy = grid
    worst, worst_at = 1.0, (0, 0)
    for r in range(gy):
        for c in range(gx):
            cell = m[r * h // gy:(r + 1) * h // gy, c * w // gx:(c + 1) * w // gx]
            v = float(cell.mean())
            if v < worst:
                worst, worst_at = v, (c, r)
    s = float(m.mean())
    mad = float(np.abs(a - b).mean())
    if heatmap_out is not None:
        write_heatmap(m, heatmap_out)
    reasons = []
    if s < ssim_min:
        reasons.append(f"ssim {s:.4f} < {ssim_min}")
    if worst < region_ssim_min:
        reasons.append(f"region {worst_at} ssim {worst:.4f} < {region_ssim_min}")
    return DiffResult(not reasons, "; ".join(reasons) or "match", s, worst, worst_at, mad, grid)


def write_heatmap(m: np.ndarray, out: Path) -> None:
    """Dissimilarity heatmap: black = identical, red->yellow = worse."""
    d = np.clip(1.0 - m, 0.0, 1.0)
    d = np.clip(d * 4.0, 0.0, 1.0)  # 0.25 dissimilarity saturates — small breaks stay visible
    rgb = np.stack([np.clip(d * 2, 0, 1), np.clip(d * 2 - 1, 0, 1), np.zeros_like(d)], axis=-1)
    out.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray((rgb * 255).astype(np.uint8)).save(out)

"""Auto-skip `geo` and `network` tests when their prerequisites are absent, so a
fresh checkout runs green before the toolchain is provisioned."""

import importlib.util

import pytest


def _geo_available() -> bool:
    return all(
        importlib.util.find_spec(m) is not None
        for m in ("pdal", "osgeo", "rasterio")
    )


def pytest_collection_modifyitems(config, items):
    if _geo_available():
        return
    skip_geo = pytest.mark.skip(reason="geospatial stack (pdal/gdal/rasterio) not installed")
    for item in items:
        if "geo" in item.keywords:
            item.add_marker(skip_geo)

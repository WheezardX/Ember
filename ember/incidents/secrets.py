"""API-key handling for incident feeds (FIRMS, Synoptic, ...).

Keys come from the environment (or an untracked ``.secrets.toml`` at the repo root),
NEVER from settings and NEVER written into provenance or bundles — a scenario bundle
is shareable public data. Adapters call ``require_secret`` at fetch time; callers that
want graceful degradation use ``get_secret`` and skip the source when it returns None.
"""

from __future__ import annotations

import os
from pathlib import Path


def _from_secrets_file(name: str) -> str | None:
    """Optional untracked ``.secrets.toml`` (``FIRMS_MAP_KEY = "..."``) beside the repo."""
    for base in (Path.cwd(), Path(__file__).resolve().parents[2]):
        fp = base / ".secrets.toml"
        if fp.exists():
            import tomllib

            data = tomllib.loads(fp.read_text(encoding="utf-8"))
            if name in data:
                return str(data[name])
    return None


def get_secret(name: str) -> str | None:
    """Return a secret from env or ``.secrets.toml``, or None if unset."""
    return os.environ.get(name) or _from_secrets_file(name)


def require_secret(name: str, *, hint: str = "") -> str:
    """Return a secret or raise a clear, actionable error naming the env var."""
    val = get_secret(name)
    if not val:
        msg = f"missing required secret {name!r} (set the {name} env var"
        msg += f" or add it to .secrets.toml). {hint}".rstrip()
        raise RuntimeError(msg + ")")
    return val

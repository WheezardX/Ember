# API credentials

Ember's incident/weather feeds need two free API keys. They live in
**`.secrets.toml`** at the repo root (git-ignored) or in environment variables — never
in settings, provenance, or scenario bundles. `ember/incidents/secrets.py` loads them.

```toml
# .secrets.toml  (repo root, git-ignored — copy .secrets.toml.example)
FIRMS_MAP_KEY  = "your_firms_key"
SYNOPTIC_TOKEN = "your_synoptic_token"
```

> Values **must be quoted** (TOML strings). An unquoted key is a parse error.

Missing a key isn't fatal: the feed that needs it is skipped (best-effort), the rest of
the bundle still builds.

---

## FIRMS_MAP_KEY — NASA FIRMS active-fire hotspots (B3)

- **Where generated:** <https://firms.modaps.eosdis.nasa.gov/api/map_key/>
  (the main FIRMS site buries it under *Web Services / API → Area → Get MAP_KEY*; the
  link above is the direct page).
- **How:** enter an email, submit — the key is issued **immediately** (no account, no
  approval). Re-requesting with the same email returns the existing key.
- **Refetch / rotate:** revisit the same URL with the same email to see the key again.
- **Account/email used:** _(fill in which email you registered — the key is tied to it)_
- **Limits:** 5000 transactions / 10-minute interval per key; a multi-day request counts
  as several transactions. `firms.py` date-chunks windows (≤3-day chunks) to stay well
  under this and avoid the endpoint's per-request day cap.
- **Used by:** `ember/incidents/firms.py`.

## SYNOPTIC_TOKEN — Synoptic Data / MesoWest RAWS observations (C3)

- **Where generated:** Synoptic **Customer Console** — <https://customer.synopticdata.com/>
  (sign up at <https://customer.synopticdata.com/signup/>).
- **How:** Synoptic issues an **API key** (private) and, from it, one or more **tokens**
  (public). The **token** is what goes in `SYNOPTIC_TOKEN` and in every request
  (`?token=…`); the API key manages tokens and is not used directly here.
  1. Create/copy your **API key** in the Console.
  2. Under that key, **generate a token** ("Manage tokens" / "Create token").
  3. Leave the token **unrestricted** (no domain/referrer limits) — calls are
     server-side, so a domain restriction would block them.
- **Trial:** the free signup starts a ~30-day full-API trial; afterward, public networks
  including **RAWS** stay available on the open-access tier (RAWS is federal
  public-domain data).
- **Refetch / rotate:** re-open the Customer Console → your API key → tokens. You can
  regenerate a token there; update `.secrets.toml` if you rotate it.
- **Account/email used:** _(fill in the account/email — needed to get back into the Console)_
- **RAWS network id:** `2` (Synoptic/MesoWest `MNET_ID`), the default in `synoptic.py`.
- **Used by:** `ember/weather/synoptic.py`.

---

## Other feeds — no key required

- **WFIGS** incidents/perimeters (B1/B2): public NIFC ArcGIS FeatureServers.
- **NIROPS** IR products (B4): public directory index at `ftp.wildfire.gov`.
- **HRRR** weather grid (C2): public AWS bucket `noaa-hrrr-bdp-pds` via herbie.

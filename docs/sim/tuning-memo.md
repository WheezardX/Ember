# ember-ca tuning memo (CP4, 2026-09-07)

Records every coefficient change to `sim/packs/ca_params.v1.toml` and why, per plan D3's
"tuning memo (what was changed and why, vs published-curve starting points)". The measuring
instrument is the CP4 matrix (`sim/scenarios/cp4/`, `checkpoints/CP4/`): 200×200 synthetic
worlds, 2 h at `dt = 60`, `ember sim curves` head rates.

## v1.0.0 → v1.1.0

**Starting point (v1.0.0):** base rates transcribed as "no-wind, flat, dry" Rothermel-order
magnitudes (GR2 22 mm/s ≈ 1.3 m/min) with the spec's moisture factor applied *on top*. Because
`f_moist = ((mx − m)/mx)²` is 0.44 for grass at the reference afternoon (RH 20 % → 5 %
moisture; mx 15 %), the effective calm rate came out at ~0.5 m/min, and the wind gain
(0.7 per m/s of *mid-flame* wind, itself 0.43 of the 10 m wind) added only ×1.6 at 2 m/s. Result:
grass at 2 m/s ran **30 m/h**; timber litter did not move; CP3's 24 h fire was 10 ha.

**Legibility targets** (cured, RH 20 %, 28 °C, flat; head rate): grass ~300 m/h calm and
1–2 km/h at 5–8 m/s; shrub about half of grass; timber understory a third; litter a crawl;
green or humid → visibly stalled. These are order-of-magnitude plausible against the Fireline
Handbook's grass/timber ranges and, more importantly, make the four comparisons predictable.

| key | v1.0.0 | v1.1.0 | why |
|---|---|---|---|
| `class.GR.base_rate_mms` | 25 | 190 | calm cured grass → ~300 m/h after the 0.44 moisture factor |
| `fuel.101/102/104/107` | 12/22/45/60 | 100/170/260/350 | Scott & Burgan ordering kept, scale ×7 |
| `class.GS.base_rate_mms`, `fuel.121/122` | 18; 12/20 | 100; 70/100 | ~200 m/h calm |
| `class.SH.base_rate_mms`, `fuel.141/142/145/147` | 14; 6/10/22/20 | 65; 30/50/100/90 | ~150 m/h calm |
| `class.TU.base_rate_mms`, `fuel.161/165` | 8; 6/12 | 35; 25/50 | ~80 m/h calm |
| `class.TL.base_rate_mms`, `fuel.181/183/188/189` | 4; 2/3/6/7 | 12; 6/10/18/22 | ~30 m/h calm (smolder, but moves) |
| `class.SB.base_rate_mms`, `fuel.201/204` | 6; 4/10 | 26; 18/40 | ~60 m/h calm |
| `k_wind_q8` GR/GS/SH/TU/TL/SB | 180/150/140/120/100/120 | 512/400/380/300/256/300 | grass ×5 at 5 m/s 10 m wind (mid-flame 2.15 m/s), timber ×2 |
| `global.lb_per_ms_q8` | 128 | 384 | wavelet LB ≈ 4.2 at 5 m/s grass; the **burned-set** envelope of 16-direction wavelets comes out at ~55–75 % of the wavelet LB (measured 2.4), which is the Anderson-order grass value |
| `global.slope_equiv_cms_per_q8unit` | 700 | 400 | 30 % slope ≈ +1.2 m/s equivalent → grass ×3.4, timber ×2.2 (was ×5) |

**Also fixed while tuning (model, not coefficients):** the knight-move rule (spec §1) — before
it, direct spread hopped a one-cell fuel break through `(2,1)` neighbours once rates were high
enough to reach the break within the test window.

**Result (CP4 tables, 2 h, head rate m/h):** wind 0/2/5/8 m/s → 203 / 454 / 908 / 908 (the last
two hit the `cell/dt` head cap, spec §11.8); fuel GR2/SH5/TU5/TL3 at 2 m/s → 454 / 363 / 151 /
15; slope flat vs 30 % → 203 / 605; cured vs green → 605 / 151; RH 12 % vs 60 % → 605 / 91.

**Not changed, noted:** residence times, moisture-of-extinction values, crowning thresholds,
spotting rates — no CP4 case exercised them beyond "on/off". Crowning + spotting get their
own addendum scenarios (D4) before CP5/CP6 judge them.

**Caveat:** none of this is validation. The coefficients make the cartoon predictable; CP5
checks it is the right order of magnitude against one real fire and says so with a
disclaimer.

"""Generator: /Game/Ember/Generated/M_Terrain — the terrain master (EPIC_5_PLAN B2).

Ground colour is composed per tile on the CPU from store layers (worldcore/look.cpp, rules in
viz/looks/*.toml) and bound as a texture; this material only samples it and adds lighting-scale
detail so close-ups are not flat 10 m texels. Runs headless via `ember-dev regen-assets`.

Parameters (the runtime surface; docs/viz/assets.md):
    Albedo          texture2D  per-tile albedo (apron included)     default WhiteSquareTexture
    AlbedoScale     scalar     UV0 -> texture scale (skip apron)    default 1
    AlbedoOffset    scalar     UV0 -> texture offset                default 0
    DetailStrength  scalar     +/- brightness from world noise      default 0.10
    DetailScale     scalar     noise frequency (1/cm)               default 0.004  (~2.5 m features)
    Roughness       scalar                                          default 0.92
    FireTex         texture2D  fire state over the replay grid (EmberFireActor, linear):
                               R burning (85 x intensity class 1-3), G burned incl. burning
                               (64 + 63 x class), B sqrt(hours since arrival / 200), A rate of
                               spread (log 15..300 m/h -> 0..1, the head)  default black
    FireRect        vector     fire grid in UE cm: (x0, y0, width, height)
    FireOn          scalar     0 = no fire bound                        default 0
    FireTime        scalar     the player's clock (s) for flame flicker default 0
    FireGain        scalar     flame emissive strength                  default 1
    GroundMix       texture2D  per-tile ground weights (linear): R litter, G grass, B rock,
                               A shrub; same UV as Albedo                default T_LinearBlack
    GroundOn        scalar     0 = no ground detail (look without [ground])  default 0
    GroundFadeNear  scalar     full detail inside this distance (cm)    default 15000
    GroundFadeFar   scalar     no detail past this distance (cm)        default 40000
    GroundNormal    scalar     detail normal strength                   default 1.4
    GroundHeight    scalar     height-blend contrast between sets       default 0.6
    GroundCliffNz   scalar     |normal.z| below which detail projects on a vertical plane
                               (YZ or XZ, whichever the normal faces) default 0.707 (45 deg)
    Tex_<Set>_C/_N  texture    detail sets (t_ground.py): Litter, Grass, Rock, Shrub
    Rep_<Set>       scalar     repeat in cm: litter 250, grass 300, rock 400, shrub 300

Ground plane v1 (EPIC_5_PLAN 8f GP3): near the camera the macro colour is multiplied by the detail
sets' colour variation (mean 1), height-blended by GroundMix; the normal becomes world-space
(vertex normal + detail slope) and roughness/AO come from the sets. Past GroundFadeFar the result
is exactly the macro look (the map look from the air).
"""

import unreal

PATH = "/Game/Ember/Generated"
NAME = "M_Terrain"
FULL = f"{PATH}/{NAME}"

eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary

if eal.does_asset_exist(FULL):
    eal.delete_asset(FULL)
mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
    NAME, PATH, unreal.Material, unreal.MaterialFactoryNew())


def expr(cls, x, y):
    return mel.create_material_expression(mat, cls, x, y)


def link(src, out, dst, inp):
    # MaterialEditingLibrary returns False on a bad pin name instead of raising; make it loud.
    if not mel.connect_material_expressions(src, out, dst, inp):
        raise RuntimeError(f"connect failed: {src.get_name()}.{out!r} -> {dst.get_name()}.{inp!r}")


def to_property(src, out, prop):
    if not mel.connect_material_property(src, out, prop):
        raise RuntimeError(f"connect failed: {src.get_name()}.{out!r} -> {prop}")


def scalar(name, value, x, y):
    e = expr(unreal.MaterialExpressionScalarParameter, x, y)
    e.set_editor_property("parameter_name", name)
    e.set_editor_property("default_value", value)
    return e


# UV = TexCoord0 * AlbedoScale + AlbedoOffset
uv = expr(unreal.MaterialExpressionTextureCoordinate, -1400, -200)
s = scalar("AlbedoScale", 1.0, -1400, -80)
o = scalar("AlbedoOffset", 0.0, -1400, 20)
mul = expr(unreal.MaterialExpressionMultiply, -1200, -150)
link(uv, "", mul, "A")
link(s, "", mul, "B")
add = expr(unreal.MaterialExpressionAdd, -1050, -120)
link(mul, "", add, "A")
link(o, "", add, "B")

tex = expr(unreal.MaterialExpressionTextureSampleParameter2D, -850, -200)
tex.set_editor_property("parameter_name", "Albedo")
white = unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture")
tex.set_editor_property("texture", white)
tex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
link(add, "", tex, "UVs")

# detail = 1 + noise(world position * DetailScale) * DetailStrength, noise in [-1, 1]
wp = expr(unreal.MaterialExpressionWorldPosition, -1400, 250)
dscale = scalar("DetailScale", 0.004, -1400, 360)
wpm = expr(unreal.MaterialExpressionMultiply, -1200, 280)
link(wp, "", wpm, "A")
link(dscale, "", wpm, "B")
noise = expr(unreal.MaterialExpressionNoise, -1050, 280)
noise.set_editor_property("scale", 1.0)
noise.set_editor_property("levels", 4)
noise.set_editor_property("output_min", -1.0)
noise.set_editor_property("output_max", 1.0)
link(wpm, "", noise, "World Position")  # pin names: mel.get_material_expression_input_names()
strength = scalar("DetailStrength", 0.10, -1050, 420)
nm = expr(unreal.MaterialExpressionMultiply, -850, 300)
link(noise, "", nm, "A")
link(strength, "", nm, "B")
one = expr(unreal.MaterialExpressionConstant, -850, 420)
one.set_editor_property("r", 1.0)
detail = expr(unreal.MaterialExpressionAdd, -650, 320)
link(one, "", detail, "A")
link(nm, "", detail, "B")

base = expr(unreal.MaterialExpressionMultiply, -450, -100)
link(tex, "RGB", base, "A")
link(detail, "", base, "B")

_made = set()


def linear_black():
    """/Game/Ember/Generated/T_LinearBlack: a 4x4 black texture with sRGB off - the default for
    the linear-colour FireTex samplers (the engine's Black is sRGB, and a Linear Color sampler
    with an sRGB default fails the material compile). Imported from a PNG written here."""
    import os
    import struct
    import tempfile
    import zlib

    full = f"{PATH}/T_LinearBlack"
    if full in _made:
        return unreal.load_asset(full)
    if eal.does_asset_exist(full):
        eal.delete_asset(full)
    w = h = 4
    raw = b"".join(b"\x00" + b"\x00\x00\x00\xff" * w for _ in range(h))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    tmp = os.path.join(tempfile.mkdtemp(), "T_LinearBlack.png")
    with open(tmp, "wb") as f:
        f.write(png)
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", tmp)
    task.set_editor_property("destination_path", PATH)
    task.set_editor_property("destination_name", "T_LinearBlack")
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", False)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    tex = unreal.load_asset(full)
    if tex is None:
        raise RuntimeError("T_LinearBlack import failed")
    tex.set_editor_property("srgb", False)
    tex.set_editor_property("compression_settings",
                            unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP)
    eal.save_asset(full, only_if_is_dirty=False)
    unreal.log(f"EMBER_GENERATED {full}")
    _made.add(full)
    return tex


def custom(name, x, y, inputs, code, out=unreal.CustomMaterialOutputType.CMOT_FLOAT3):
    c = expr(unreal.MaterialExpressionCustom, x, y)
    c.set_editor_property("output_type", out)
    c.set_editor_property("description", name)
    ins = []
    for n in inputs:
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        ins.append(ci)
    c.set_editor_property("inputs", ins)
    c.set_editor_property("code", code)
    return c


GROUND_CODE = r"""
float fade = saturate((FadeFar - length(WP - Cam)) / max(FadeFar - FadeNear, 1.0)) * On;
float3 vn = normalize(VN);
NormalWS = vn;
Rough = R0;
// Sky occlusion under canopy (material AO scales only sky / indirect light, not the sun): the
// trees are not in the distance-field scene, so Lumen barely shades the forest floor from the
// sky - it was lit flat blue-grey, which read as a court. Litter weight tracks canopy cover
// (canopy_to_litter), at every distance.
float canopy = saturate(Mix.x) * On;
Occ = 1.0 - CanopyOcc * canopy;
// Far-field canopy impression (8g item 6: past the tree radius the forest was flat paint).
// Beyond FarStart (the scenario's tree radius; 0 = no trees drawn at all) forested ground gets
// crown-scale texture: value noise at ~9 m and ~3.5 m -> lit crowns and dark gaps, and bumps so
// crowns catch the sun. Canopy weight = the ground mix's litter weight (canopy_to_litter).
float dist = length(WP - Cam);
float farW = canopy * saturate((dist - FarStart) / 60000.0) * step(0.5, CanopyOn);
if (farW > 0.001) {
    float2 cp = WP.xy / 100.0;
    float2 cg = 0; float ch = 0;
    // stand clumps (30 m), crowns (9 m), crown detail (3.5 m)
    float2 cs[3] = { float2(30.0, 0.4), float2(9.0, 0.45), float2(3.5, 0.15) };
    // anti-aliasing: an octave fades to its mean from ~6 pixels per wavelength down to 2 (it
    // aliased into a herringbone on far slopes). Pixel footprint ~ distance x 0.00055 (1440p).
    float pxm = dist / 100.0 * 0.00055;
    [unroll] for (int k = 0; k < 3; ++k) {
        float w0 = cs[k].y;
        float vis = saturate(1.5 - 3.0 * pxm / cs[k].x);
        cs[k].y *= vis;
        ch += (1.0 - vis) * w0 * 0.5;
        float2 qq = cp / cs[k].x + 31.7 * k;
        float2 qi = floor(qq), qf = frac(qq);
        float2 qw = qf * qf * (3.0 - 2.0 * qf);
        float2 dw = 6.0 * qf * (1.0 - qf);
        float2 ci = qi - 1024.0 * floor(qi / 1024.0);
        float2 cj = ci + 1.0; cj -= 1024.0 * floor(cj / 1024.0);
        float4 hv = frac(sin(float4(dot(ci, float2(12.9898, 78.233)), dot(float2(cj.x, ci.y), float2(12.9898, 78.233)),
                                    dot(float2(ci.x, cj.y), float2(12.9898, 78.233)), dot(cj, float2(12.9898, 78.233)))) * 43758.5453);
        ch += cs[k].y * (hv.x + (hv.y - hv.x) * qw.x + (hv.z - hv.x) * qw.y + (hv.x - hv.y - hv.z + hv.w) * qw.x * qw.y);
        cg += cs[k].y / cs[k].x * float2(dw.x * ((hv.y - hv.x) + (hv.x - hv.y - hv.z + hv.w) * qw.y),
                                         dw.y * ((hv.z - hv.x) + (hv.x - hv.y - hv.z + hv.w) * qw.x));
    }
    float crowns = saturate((ch - 0.3) * 2.2);            // 0 gap .. 1 crown top
    float3 canopyCol = Base * lerp(0.35, 1.2, crowns);    // shadowed gaps between lit crowns
    Base = lerp(Base, canopyCol, farW);
    vn = normalize(vn + float3(-cg * 2.5, 0.0) * farW);  // crowns are ~10 m domes: steep bumps
    NormalWS = vn;
}
if (fade <= 0.0) { return Base; }
// Projection (Brad, from GW2): top-down until the surface is steeper than 45 deg, then a hard
// switch to the vertical plane the normal faces most (YZ or XZ). No blend: the jump lands on
// the steep break, where a change in the stone pattern reads as a rock edge, not a seam.
float3 an = abs(vn);
float2 p = WP.xy;
float3 tu = float3(1, 0, 0), tv = float3(0, 1, 0);
if (an.z < CliffZ) {
    if (an.x > an.y) { p = WP.yz; tu = float3(0, 1, 0); tv = float3(0, 0, 1); }
    else             { p = WP.xz; tu = float3(1, 0, 0); tv = float3(0, 0, 1); }
}
float t = saturate(Nlo * 0.5 + 0.5);
// Parallax + self-shadow on the litter (ground volume, 8g: "the bump doesn't read"). Near the
// camera on top-down-projected forest floor, march the view ray into the litter height field
// (C alpha: 0 = deepest duff, 1 = top of sticks / needle mats, PomDepth cm deep) and sample there,
// so needles and sticks occlude what lies behind them; then march toward the sun - what is
// shadowed by higher litter darkens. Litter only (the forest floor is where the eye is).
// (depth pass 2026-10-01, Brad: "still reads like a texture": out to 35 m, was 20)
float pomW = saturate((3500.0 - dist) / 1200.0) * saturate(Mix.x * 1.5) * step(CliffZ, an.z) * step(0.01, PomDepth);
float shade = 1.0;
if (pomW > 0.01) {
    float2 p0 = p;
    float2 g0 = p0 / RL;
    float2 gx = ddx(g0), gy = ddy(g0);
    float3 V = normalize(Cam - WP);
    // relative to the SURFACE (slopes): the view's tangential part over its normal part; fade
    // out at grazing angles, where any parallax smears into streaks
    float vdn = dot(V, vn);
    pomW *= saturate((vdn - 0.2) / 0.25);
    float2 step2 = -(V.xy - vn.xy * vdn) / max(vdn, 0.35) * PomDepth;   // cm of p shift over the full depth
    const int N = 16;
    float d = 0.0, h = 1.0, dPrev = 0.0, gapPrev = 0.0;
    float2 pp = p0;
    [loop] for (int i = 0; i <= N; ++i) {
        float2 qq = float2(pp.x * 0.8 - pp.y * 0.6, pp.x * 0.6 + pp.y * 0.8) * 0.37 + 1731.0;
        h = lerp(Texture2DSampleGrad(LC, LCSampler, pp / RL, gx, gy).a,
                 Texture2DSampleGrad(LC, LCSampler, qq / RL, gx * 0.37, gy * 0.37).a, t);
        float gap = (1.0 - h) - d;              // > 0: the ray is still above the surface
        if (gap <= 0.0) {
            // linear refinement between the last two steps (no stair-step bands)
            float a = (i > 0) ? gapPrev / max(gapPrev - gap, 1e-4) : 0.0;
            d = lerp(dPrev, d, a);
            pp = p0 + step2 * d;
            h = 1.0 - d;
            break;
        }
        dPrev = d;
        gapPrev = gap;
        d += 1.0 / N;
        pp = p0 + step2 * d;
    }
    p = lerp(p0, pp, pomW);
    // toward the sun: rising ray from the hit point; a higher litter sample in between = shadow
    float3 Ls = normalize(SunDir);
    if (Ls.z > 0.05) {
        float occ = 0.0;
        float2 sun2 = Ls.xy / max(Ls.z, 0.15) * PomDepth;
        [unroll] for (int k = 1; k <= 4; ++k) {
            float f = k / 4.0 * (1.0 - h);
            float2 ps = pp + sun2 * f;
            float2 qs = float2(ps.x * 0.8 - ps.y * 0.6, ps.x * 0.6 + ps.y * 0.8) * 0.37 + 1731.0;
            float hs = lerp(Texture2DSampleGrad(LC, LCSampler, ps / RL, gx, gy).a,
                            Texture2DSampleGrad(LC, LCSampler, qs / RL, gx * 0.37, gy * 0.37).a, t);
            occ = max(occ, hs - (h + f));
        }
        shade = lerp(1.0, 1.0 - 0.75 * saturate(occ * 8.0), pomW);   // was 0.55 / x6: shadows in the litter barely showed
    }
}
float2 q = float2(p.x * 0.8 - p.y * 0.6, p.x * 0.6 + p.y * 0.8) * 0.37 + 1731.0;
float4 c0 = lerp(Texture2DSample(LC, LCSampler, p / RL), Texture2DSample(LC, LCSampler, q / RL), t);
float4 n0 = lerp(Texture2DSample(LN, LNSampler, p / RL), Texture2DSample(LN, LNSampler, q / RL), t);
float4 c1 = lerp(Texture2DSample(GC, GCSampler, p / RG), Texture2DSample(GC, GCSampler, q / RG), t);
float4 n1 = lerp(Texture2DSample(GN, GNSampler, p / RG), Texture2DSample(GN, GNSampler, q / RG), t);
float4 c2 = lerp(Texture2DSample(RC, RCSampler, p / RR), Texture2DSample(RC, RCSampler, q / RR), t);
float4 n2 = lerp(Texture2DSample(RN, RNSampler, p / RR), Texture2DSample(RN, RNSampler, q / RR), t);
float4 c3 = lerp(Texture2DSample(SC, SCSampler, p / RS), Texture2DSample(SC, SCSampler, q / RS), t);
float4 n3 = lerp(Texture2DSample(SN, SNSampler, p / RS), Texture2DSample(SN, SNSampler, q / RS), t);
float4 w = saturate(Mix);
float ws = saturate(1.0 - dot(w, float4(1, 1, 1, 1)));
// height blend: the taller set wins where two meet (crisp, natural transitions)
float4 hb = w + float4(c0.a, c1.a, c2.a, c3.a) * HC * step(0.001, w);
float hs = ws + 0.5 * HC * step(0.001, ws);
float top = max(max(max(hb.x, hb.y), max(hb.z, hb.w)), hs) - 0.25;
float4 bw = max(hb - top, 0.0);
float bs = max(hs - top, 0.0);
float sum = dot(bw, float4(1, 1, 1, 1)) + bs + 1e-5;
bw /= sum; bs /= sum;
// Ground v2 (8g, Brad: "a basketball court"). Each set's variation multiplies a colour that is
// part the map's macro colour and part the set's OWN colour (AbsW: litter reads as brown needle
// duff near the camera even where the map is canopy-tinted green).
float3 own = bw.x * lerp(Base, AC0, AbsW.x) * 2.0 * c0.rgb + bw.y * lerp(Base, AC1, AbsW.y) * 2.0 * c1.rgb
           + bw.z * lerp(Base, AC2, AbsW.z) * 2.0 * c2.rgb + bw.w * lerp(Base, AC3, AbsW.w) * 2.0 * c3.rgb
           + bs * Base;
float2 slope = bw.x * (n0.rg * 2 - 1) + bw.y * (n1.rg * 2 - 1) + bw.z * (n2.rg * 2 - 1) + bw.w * (n3.rg * 2 - 1);
float rough = bw.x * n0.b + bw.y * n1.b + bw.z * n2.b + bw.w * n3.b + bs * R0;
float ao = bw.x * n0.a + bw.y * n1.a + bw.z * n2.a + bw.w * n3.a + bs;
// Hummocks: the forest floor is never a plane - value-noise relief at ~1.2 m and ~0.35 m bends
// the normal (finite differences, world XY) and darkens the hollows a little.
float2 hp = WP.xy / 100.0;                       // metres
float2 hs2[2] = { float2(1.2, 1.0), float2(0.35, 0.45) };   // (wavelength m, weight)
float2 hg = 0;
float hh = 0;
[unroll] for (int k = 0; k < 2; ++k) {
    float2 qq = hp / hs2[k].x + k * 17.3;
    float2 qi = floor(qq), qf = frac(qq);
    float2 qw = qf * qf * (3.0 - 2.0 * qf);
    float2 dw = 6.0 * qf * (1.0 - qf);
    float2 ci = qi - 1024.0 * floor(qi / 1024.0);
    float2 cj = ci + 1.0; cj -= 1024.0 * floor(cj / 1024.0);
    float4 hv = frac(sin(float4(dot(ci, float2(12.9898, 78.233)), dot(float2(cj.x, ci.y), float2(12.9898, 78.233)),
                                dot(float2(ci.x, cj.y), float2(12.9898, 78.233)), dot(cj, float2(12.9898, 78.233)))) * 43758.5453);
    float va = hv.x, vb = hv.y, vc = hv.z, vd = hv.w;
    hh += hs2[k].y * (va + (vb - va) * qw.x + (vc - va) * qw.y + (va - vb - vc + vd) * qw.x * qw.y);
    // analytic gradient of the bilinear-smoothstep value noise, per metre of world
    hg += hs2[k].y / hs2[k].x * float2(dw.x * ((vb - va) + (va - vb - vc + vd) * qw.y),
                                       dw.y * ((vc - va) + (va - vb - vc + vd) * qw.x));
}
float3 hn = float3(-hg * HumS, 0.0);
ao *= lerp(1.0, 0.8 + 0.4 * saturate(hh / 1.45), HumS > 0 ? 1.0 : 0.0);
NormalWS = normalize(vn + ((tu * slope.x + tv * slope.y) * NS + hn) * fade);
Rough = lerp(R0, rough, fade);
// crevices: 60 % of the detail AO darkens the colour (was half; 80 % went near-black), all of it shades the sky light
Occ *= lerp(1.0, ao, fade);
return lerp(Base, own * lerp(1.0, ao, 0.6) * shade, fade);
"""

mixtex = expr(unreal.MaterialExpressionTextureSampleParameter2D, -850, -420)
mixtex.set_editor_property("parameter_name", "GroundMix")
mixtex.set_editor_property("texture", linear_black())
mixtex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
link(add, "", mixtex, "UVs")

ground = custom("EmberGround", -250, -300,
                ["Base", "Mix", "WP", "Cam", "VN", "Nlo", "On", "FadeNear", "FadeFar", "NS", "HC",
                 "R0", "LC", "LN", "GC", "GN", "RC", "RN", "SC", "SN", "RL", "RG", "RR", "RS",
                 "CliffZ", "AC0", "AC1", "AC2", "AC3", "AbsW", "HumS", "CanopyOcc", "FarStart", "CanopyOn",
                 "PomDepth", "SunDir"],
                GROUND_CODE)
outs = []
for oname, otype in (("NormalWS", unreal.CustomMaterialOutputType.CMOT_FLOAT3),
                     ("Rough", unreal.CustomMaterialOutputType.CMOT_FLOAT1),
                     ("Occ", unreal.CustomMaterialOutputType.CMOT_FLOAT1)):
    co = unreal.CustomOutput()
    co.set_editor_property("output_name", oname)
    co.set_editor_property("output_type", otype)
    outs.append(co)
ground.set_editor_property("additional_outputs", outs)
link(base, "", ground, "Base")
link(mixtex, "RGBA", ground, "Mix")
# low-frequency noise (~25 m) picks between the two detail scales, so neither tiling repeats
wp_lo = expr(unreal.MaterialExpressionMultiply, -1200, -560)
link(wp, "", wp_lo, "A")
lo_scale = expr(unreal.MaterialExpressionConstant, -1400, -560)
lo_scale.set_editor_property("r", 0.0004)
link(lo_scale, "", wp_lo, "B")
noise_lo = expr(unreal.MaterialExpressionNoise, -1050, -560)
noise_lo.set_editor_property("scale", 1.0)
noise_lo.set_editor_property("levels", 2)
noise_lo.set_editor_property("output_min", -1.0)
noise_lo.set_editor_property("output_max", 1.0)
link(wp_lo, "", noise_lo, "World Position")
link(noise_lo, "", ground, "Nlo")
link(wp, "", ground, "WP")
link(expr(unreal.MaterialExpressionCameraPositionWS, -650, -640), "", ground, "Cam")
link(expr(unreal.MaterialExpressionVertexNormalWS, -650, -700), "", ground, "VN")
link(scalar("GroundOn", 0.0, -650, -760), "", ground, "On")
link(scalar("GroundFadeNear", 15000.0, -650, -820), "", ground, "FadeNear")
link(scalar("GroundFadeFar", 40000.0, -650, -880), "", ground, "FadeFar")
link(scalar("GroundNormal", 1.4, -650, -940), "", ground, "NS")   # 1.0 read flat under soft canopy light
link(scalar("GroundHeight", 0.6, -650, -1000), "", ground, "HC")
link(scalar("Roughness", 0.92, -650, -1060), "", ground, "R0")
link(scalar("GroundCliffNz", 0.707, -650, -1120), "", ground, "CliffZ")  # |N.z| below: vertical projection
# Ground v2: each set's own colour (sRGB) and how much of it shows near the camera, and the
# hummock relief strength.
GROUND_OWN = {"Litter": ("#4A3322", 0.65), "Grass": ("#8C7F55", 0.35), "Rock": ("#9A9284", 0.4),
              "Shrub": ("#4E4630", 0.4)}


def _lin(hexcol):
    c = [int(hexcol[i:i + 2], 16) / 255.0 for i in (1, 3, 5)]
    return [v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in c]


for i, (set_name, (hexcol, _)) in enumerate(GROUND_OWN.items()):
    v = expr(unreal.MaterialExpressionVectorParameter, -900, -1400 - 90 * i)
    v.set_editor_property("parameter_name", f"GroundColor_{set_name}")
    r, g, b = _lin(hexcol)
    v.set_editor_property("default_value", unreal.LinearColor(r, g, b, 1.0))
    link(v, "RGB", ground, f"AC{i}")
absw = expr(unreal.MaterialExpressionVectorParameter, -900, -1800)
absw.set_editor_property("parameter_name", "GroundOwnColor")
absw.set_editor_property("default_value", unreal.LinearColor(*[w for _, w in GROUND_OWN.values()]))
link(absw, "RGBA", ground, "AbsW")
link(scalar("GroundHummocks", 0.12, -650, -1180), "", ground, "HumS")
link(scalar("GroundCanopyOcclusion", 0.8, -650, -1240), "", ground, "CanopyOcc")
to_property(ground, "Occ", unreal.MaterialProperty.MP_AMBIENT_OCCLUSION)
# far-field canopy impression: starts at the tree radius (cm, set per scenario by the harness)
link(scalar("CanopyFarStart", 0.0, -650, -1300), "", ground, "FarStart")
link(scalar("CanopyImpression", 1.0, -650, -1360), "", ground, "CanopyOn")
# litter parallax depth (cm; 0 = off) and the sun for its self-shadow
link(scalar("GroundPomDepth", 11.0, -650, -1420), "", ground, "PomDepth")   # cm; litter v3 height has real mid-scale relief
link(expr(unreal.MaterialExpressionSkyAtmosphereLightDirection, -650, -1480), "", ground, "SunDir")
for i, (set_name, pin, rep) in enumerate((("Litter", "L", 250.0), ("Grass", "G", 300.0),
                                          ("Rock", "R", 400.0), ("Shrub", "S", 300.0))):
    for kind in ("C", "N"):
        obj = expr(unreal.MaterialExpressionTextureObjectParameter, -1000, -1200 - 120 * (2 * i + (kind == "N")))
        obj.set_editor_property("parameter_name", f"Tex_{set_name}_{kind}")
        t = unreal.load_asset(f"/Game/Ember/Generated/Ground/T_Ground_{set_name}_{kind}")
        if t is None:
            raise RuntimeError(f"T_Ground_{set_name}_{kind} missing: t_ground.py must run first")
        obj.set_editor_property("texture", t)
        if kind == "N":
            obj.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_MASKS)
        else:
            # data, not colour: mean-0.5 multipliers (sRGB decode made 2 x C average ~0.43, so
            # the near ground rendered at under half its map brightness)
            obj.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
        link(obj, "", ground, f"{pin}{kind}")
    link(scalar(f"Rep_{set_name}", rep, -1200, -1200 - 240 * i), "", ground, f"R{pin}")


# ---- fire (HCP3): the replay's state texture, sampled by world position -------------------------
rect = expr(unreal.MaterialExpressionVectorParameter, -1400, 600)
rect.set_editor_property("parameter_name", "FireRect")
rect.set_editor_property("default_value", unreal.LinearColor(0.0, 0.0, 1.0, 1.0))
fire_on = scalar("FireOn", 0.0, -1400, 700)
fire_time = scalar("FireTime", 0.0, -1400, 790)
fire_gain = scalar("FireGain", 1.0, -1400, 880)
# UV over the fire grid, jittered by the detail noise (a few metres) so 30 m cells read organic.
fuv = custom("EmberFireUV", -1050, 600, ["WP", "R", "N"], (
    "float2 uv = (WP.xy - R.xy) / R.zw;\n"
    "uv += N * 1500.0 / R.zw;\n"          # ~15 m jitter: 30 m cells stop reading as squares
    "return uv;\n"), unreal.CustomMaterialOutputType.CMOT_FLOAT2)
link(wp, "", fuv, "WP")
link(rect, "RGBA", fuv, "R")
link(noise, "", fuv, "N")
ftex = expr(unreal.MaterialExpressionTextureSampleParameter2D, -850, 600)
ftex.set_editor_property("parameter_name", "FireTex")
ftex.set_editor_property("texture", linear_black())
ftex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
link(fuv, "", ftex, "UVs")
# Ground: charcoal black -> dark ash (noise), settling over the days after the burn. Warm, not
# grey: a neutral grey reads slate-blue under the skylight at altitude (HCP3 Jolly).
fcol = custom("EmberFireGround", -450, 450, ["Base", "F", "N", "On", "Macro"], (
    "float burned = smoothstep(0.35, 0.65, saturate(F.g * 2.0) + N * 0.18) * On;\n"
    "float age_h = F.b * F.b * 200.0;\n"
    "float ash = saturate(N * 0.9 + 0.35);\n"
    "float3 fresh = lerp(float3(0.010, 0.009, 0.008), float3(0.035, 0.032, 0.03), ash * ash);\n"
    "float3 old = lerp(float3(0.028, 0.022, 0.017), float3(0.075, 0.063, 0.052), ash * ash);\n"
    "float3 charc = lerp(fresh, old, saturate(age_h / 72.0));\n"
    # ground plane v1: the detail's light/dark pattern survives as charred texture
    "float3 lw = float3(0.2126, 0.7152, 0.0722);\n"
    "float r = dot(Base, lw) / max(dot(Macro, lw), 1e-4);\n"
    "charc *= lerp(1.0, clamp(r, 0.3, 2.0), 0.85);\n"
    "return lerp(Base, charc, burned);\n"))
link(ground, "", fcol, "Base")
link(base, "", fcol, "Macro")
link(ftex, "RGBA", fcol, "F")
link(noise, "", fcol, "N")
link(fire_on, "", fcol, "On")
to_property(fcol, "", unreal.MaterialProperty.MP_BASE_COLOR)
# Emissive: flickering flames on burning cells; a fading ember glow for ~12 h after the front.
femi = custom("EmberFireGlow", -450, 700,
              ["F", "N", "On", "T", "G", "WP", "Cam", "Ground", "Macro", "RepClass"], (
    "float inside = On;\n"
    "float age_h = F.b * F.b * 200.0;\n"
    "float cls = F.r * 3.0;\n"                                   # intensity class 1..3 (0 = out)
    "float burning = smoothstep(0.3, 0.7, min(cls, 1.0) * 0.725 + N * 0.2) * inside;\n"
    # HCP4: flame strength by class - a creeping surface fire is a low glow, the head is bright
    "float clsk = lerp(0.3, 1.0, saturate((cls - 1.0) * 0.5));\n"
    # hottest at the front (first ~hour after arrival), fading to smouldering over the burn. From
    # altitude the band widens (decay grows with camera distance) so the active edge stays a few
    # pixels wide, like the IR maps (design doc 5.2: legibility first); unchanged within 3 km.
    "float dkm = length(WP - Cam) / 100000.0;\n"
    "float front = exp(-age_h / (0.7 * max(1.0, dkm / 3.0)));\n"
    # HCP4 flame persistence: a stream with intensity classes says how long a cell burns (Jolly
    # CA: median 2 h, p90 7-9 h after arrival), so flames stay lit for the whole residence at a
    # floor by class; the fresh front still peaks above it (the head reads hotter, and redder
    # behind it). Without classes (playback, synth) it is the HCP3 front window.
    "float persist = RepClass * lerp(0.25, 0.6, saturate((cls - 1.0) * 0.5));\n"
    "float alive = lerp(persist, 1.0, front);\n"
    "float breakup = saturate(N * 1.0 + 0.75);\n"
    # Flicker phase from a 1.5 m value noise, one period per blob. (It was N * 23: several sine
    # periods across the smooth 2.5 m noise froze into concentric rings in every still - the
    # "char contour rings" of HCP3 / GP5.)
    "float2 fq = WP.xy / 150.0;\n"
    "float2 fi = floor(fq), ff = frac(fq);\n"
    "ff = ff * ff * (3.0 - 2.0 * ff);\n"
    # corners wrapped into [0, 1024) before the sin hash (large-world coordinates lose it)
    "float2 fc0 = fi - 1024.0 * floor(fi / 1024.0);\n"
    "float2 fc1 = fc0 + 1.0; fc1 -= 1024.0 * floor(fc1 / 1024.0);\n"
    "const float2 hk = float2(12.9898, 78.233);\n"
    "float4 fh = frac(sin(float4(dot(fc0, hk), dot(float2(fc1.x, fc0.y), hk),\n"
    "                            dot(float2(fc0.x, fc1.y), hk), dot(fc1, hk))) * 43758.5453);\n"
    "float fph = lerp(lerp(fh.x, fh.y, ff.x), lerp(fh.z, fh.w, ff.x), ff.y) * 6.2832;\n"
    "float flick = 0.7 + 0.3 * sin(T * 7.0 + fph) * sin(T * 3.1 + fph * 1.7);\n"
    "float3 flame = lerp(float3(3.0, 0.45, 0.04), float3(5.0, 1.8, 0.25), front) * flick;\n"
    # HCP4: the class reads as colour, not only brightness (IR-map legibility, design doc 5.2):
    # creeping surface fire a dim deep red, class 2 the orange above, crown fire yellow-white.
    # A stream without classes is drawn as 3 = the flame above (the HCP3 look).
    "float3 c1 = float3(2.0, 0.18, 0.02) * flick;\n"
    "float3 c3 = lerp(flame, float3(7.0, 4.2, 1.4) * flick, 0.35);\n"
    "flame = cls < 2.0 ? lerp(c1, flame, saturate(cls - 1.0)) : lerp(flame, c3, saturate(cls - 2.0) * RepClass);\n"
    # HCP4 H4-3 heading: F.a = local rate of spread (log, 0 <= 15 m/h .. 1 >= 300 m/h). The head
    # (fast) burns brighter and whiter, flanks and backing fire dimmer and redder, within a class.
    "float head = saturate(F.a) * RepClass;\n"
    "alive *= lerp(1.0, lerp(0.65, 1.5, head), RepClass);\n"
    "flame *= lerp(float3(1, 1, 1), lerp(float3(0.9, 0.75, 0.6), float3(1.15, 1.6, 2.2), head), RepClass);\n"
    "float patch = saturate(N * 2.0 - 0.2);\n"
    # ground plane v1: near the camera the glow breaks up into embers on the bright bits of the
    # ground detail (needles, twigs) instead of a 2.5 m wash; unchanged past ~300 m
    "float3 lw = float3(0.2126, 0.7152, 0.0722);\n"
    "float gr = dot(Ground, lw) / max(dot(Macro, lw), 1e-4);\n"
    "float nearc = saturate(1.0 - length(WP - Cam) / 30000.0);\n"
    "float fine = saturate((gr - 1.15) * 3.0);\n"
    "fine *= fine * saturate(N * 3.0 - 0.5);\n"            # sparse: highlights x noise
    "patch = lerp(patch, patch * fine, nearc);\n"
    # eye level: a smouldering floor is dim - the 2.5 m wash lit the whole scene red (Lumen GI)
    "patch *= lerp(1.0, 0.35, nearc);\n"
    "float smoulder = burning * (1.0 - front) * 0.07 * patch;\n"
    "float embers = saturate(F.g * 2.0) * (1.0 - saturate(F.r * 4.0)) * inside\n"
    "             * saturate(1.0 - age_h / 12.0) * patch * 0.08;\n"
    # H4-5: within the flame cards' range (AEmberFlameActor: full to 220 m, gone by 400 m) the
    # cards are the flames; the ground under them is a glowing bed, not a white-hot wash.
    "float cards = saturate((40000.0 - length(WP - Cam)) / 18000.0);\n"
    "return G * (burning * clsk * alive * breakup * flame * lerp(1.0, 0.12, cards)\n"
    "            + (smoulder + embers) * float3(1.2, 0.18, 0.02));\n"))
link(ftex, "RGBA", femi, "F")
link(noise, "", femi, "N")
link(fire_on, "", femi, "On")
link(fire_time, "", femi, "T")
link(fire_gain, "", femi, "G")
link(wp, "", femi, "WP")
link(expr(unreal.MaterialExpressionCameraPositionWS, -650, 900), "", femi, "Cam")
link(ground, "", femi, "Ground")
link(scalar("FireClasses", 0.0, -650, 980), "", femi, "RepClass")  # 1 = the stream reports intensity classes
link(base, "", femi, "Macro")
to_property(femi, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

to_property(ground, "Rough", unreal.MaterialProperty.MP_ROUGHNESS)
to_property(ground, "NormalWS", unreal.MaterialProperty.MP_NORMAL)
mat.set_editor_property("tangent_space_normal", False)

# Geomorph (8g item 4, tiles popped in "solid"): AEmberTerrainActor puts each vertex's height
# offset to the surface it replaces in UV2.x (cm) and sets MorphStart / MorphSeconds on the
# tile's instance; the offset eases out (smoothstep) over MorphSeconds. MorphSeconds 0 = none.
uv2 = expr(unreal.MaterialExpressionTextureCoordinate, -1400, 1200)
uv2.set_editor_property("coordinate_index", 2)
morph = custom("EmberGeomorph", -450, 1200, ["D", "T", "S", "L"], (
    "float u = L > 0.0 ? saturate((T - S) / L) : 1.0;\n"
    "u = u * u * (3.0 - 2.0 * u);\n"
    "return float3(0.0, 0.0, D.x * (1.0 - u));\n"))
link(uv2, "", morph, "D")
link(expr(unreal.MaterialExpressionTime, -1400, 1300), "", morph, "T")
link(scalar("MorphStart", 0.0, -1400, 1380), "", morph, "S")
link(scalar("MorphSeconds", 0.0, -1400, 1460), "", morph, "L")
to_property(morph, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

mel.recompile_material(mat)
eal.save_asset(FULL, only_if_is_dirty=False)
unreal.log(f"EMBER_GENERATED {FULL}")

"""Generator: /Game/Ember/Generated/M_Veg - the vegetation master material (EPIC_5_PLAN B3 v2).

    lerp(TrunkColor, Color x per-tree tint, vertex alpha) x vertex RGB; two-sided; Two-Sided
    Foliage shading; wind WPO (sway + per-tree lean + twig flutter, harness-owned clock); fire
    (FireTex / FireRect / FireOn / FireTime / FireGain sampled at each tree's pivot: char, flames).
    Ground cover (ground plane v1 GP5): Consume 0..1 collapses a burned plant toward its pivot
    (grass, ferns, shrubs are consumed, not left standing black); Smoulder 0..1 gives wood (vertex
    alpha 0: logs, stumps) a patchy ember glow for many hours after the front (Three Queens field
    photo: a hollow stump still burning inside). Both default 0 (trees unchanged).

Split from veg_species.py so a shader edit rebuilds one material (seconds) instead of every
tree mesh (~20 min). Species material instances (veg_species.py) parent this asset, so it is
rebuilt in place, never deleted. Runs headless via `ember-dev regen-assets`.
"""

import unreal

PATH = "/Game/Ember/Generated"
eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()

# Foliage grade defaults (M_Veg FoliageSaturation / FoliageBrightness), tuned against NAIP with
# S_naip_tq + `ember-dev naip-probe` (8i R2).
# Burned-tree outcome (8g burned-area mosaic), per tree from PerInstanceRandom and the class it
# burned at (FireTex G). oc: 0 green survivor, 1 scorched orange crown, 2 black crown,
# 3 bare black skeleton (needles gone), 4 consumed (broken off to a ~25 % snag).
#   crown fire (3): 12 % consumed, 63 % bare, 18 % black crown, 7 % orange
#   class 2:        22 % bare, 40 % orange, 38 % survive green
#   class 1:        12 % orange, 88 % survive green
OUTCOME_HLSL = (
    "float ocls = 1.0 + 2.0 * saturate((F.g - 0.5) * 2.0);\n"
    "float rr = frac(Rnd * 13.37 + 0.31);\n"
    "int oc = 0;\n"
    "if (ocls > 2.5)      { oc = rr < 0.12 ? 4 : (rr < 0.75 ? 3 : (rr < 0.93 ? 2 : 1)); }\n"
    "else if (ocls > 1.5) { oc = rr < 0.22 ? 3 : (rr < 0.62 ? 1 : 0); }\n"
    "else                { oc = rr < 0.12 ? 1 : 0; }\n")

FOLIAGE_SATURATION = 0.65
FOLIAGE_BRIGHTNESS = 1.5


def vnoise3(p, expr, out):
    """HLSL for a smooth 3-D value noise of `expr` (small coordinates) into float `out` (0..1).
    `p` prefixes the temporaries so several can live in one custom node."""
    corner = "frac(sin(dot({i} + float3({x}, {y}, {z}), float3(12.9898, 78.233, 37.719))) * 43758.5453)"
    c = [corner.format(i=f"{p}i", x=x, y=y, z=z) for z in (0, 1) for y in (0, 1) for x in (0, 1)]
    return (
        f"float3 {p} = {expr};\n"
        f"float3 {p}i = floor({p}), {p}f = frac({p});\n"
        f"{p}f = {p}f * {p}f * (3.0 - 2.0 * {p}f);\n"
        f"float {out} = lerp(lerp(lerp({c[0]}, {c[1]}, {p}f.x), lerp({c[2]}, {c[3]}, {p}f.x), {p}f.y),\n"
        f"                  lerp(lerp({c[4]}, {c[5]}, {p}f.x), lerp({c[6]}, {c[7]}, {p}f.x), {p}f.y), {p}f.z);\n")


# ------------------------------------------------------------------ master material
def build_material():
    full = f"{PATH}/M_Veg"
    # Rebuilt in place: the species material instances (veg_species.py) reference this asset, so
    # deleting and re-creating it would orphan them. Clear the graph and wire it again.
    if eal.does_asset_exist(full):
        mat = unreal.load_asset(full)
        mel.delete_all_material_expressions(mat)
    else:
        mat = tools.create_asset("M_Veg", PATH, unreal.Material, unreal.MaterialFactoryNew())

    def link(src, out, dst, inp):
        if not mel.connect_material_expressions(src, out, dst, inp):
            raise RuntimeError(f"connect failed: {out!r} -> {inp!r}")

    def custom(name, x, y, inputs, code):
        c = mel.create_material_expression(mat, unreal.MaterialExpressionCustom, x, y)
        c.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
        c.set_editor_property("description", name)
        ins = []
        for n in inputs:
            ci = unreal.CustomInput()
            ci.set_editor_property("input_name", n)
            ins.append(ci)
        c.set_editor_property("inputs", ins)
        c.set_editor_property("code", code)
        return c

    col = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -900, -200)
    col.set_editor_property("parameter_name", "Color")
    col.set_editor_property("default_value", unreal.LinearColor(0.03, 0.06, 0.025, 1.0))
    trunk_col = mel.create_material_expression(
        mat, unreal.MaterialExpressionVectorParameter, -900, -400)
    trunk_col.set_editor_property("parameter_name", "TrunkColor")
    trunk_col.set_editor_property("default_value", unreal.LinearColor(0.06, 0.035, 0.02, 1.0))
    vc = mel.create_material_expression(mat, unreal.MaterialExpressionVertexColor, -900, 100)
    rnd = mel.create_material_expression(mat, unreal.MaterialExpressionPerInstanceRandom, -900, 0)

    # Per-tree foliage tint: brightness and a little yellow-green shift, so no two neighbours
    # match (PerInstanceRandom: constant over an instance, deterministic per component seed).
    tint = custom("EmberTint", -650, -100, ["Rnd"], (
        "float v = 0.84 + 0.32 * Rnd;\n"
        "float y = frac(Rnd * 7.31) - 0.5;\n"
        "return float3(v * (1.0 + 0.10 * y), v, v * (1.0 - 0.12 * y));\n"))
    link(rnd, "", tint, "Rnd")
    # Foliage grade (8i R2, NAIP: timber rendered ~2x too saturated green and too dark from
    # above). Saturation and brightness on the tinted foliage colour; the transmission colour
    # below takes the same graded colour, so crowns do not glow game-green in backlight.
    grade = {}
    for i, (name, default) in enumerate((("FoliageSaturation", FOLIAGE_SATURATION),
                                          ("FoliageBrightness", FOLIAGE_BRIGHTNESS))):
        p = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter,
                                           -900, -700 - 90 * i)
        p.set_editor_property("parameter_name", name)
        p.set_editor_property("default_value", default)
        grade[name] = p
    fol = custom("EmberFoliageGrade", -450, -150, ["C", "T", "Sat", "Br"], (
        "float3 c = C.rgb * T;\n"
        "float l = dot(c, float3(0.2126, 0.7152, 0.0722));\n"
        "return lerp(float3(l, l, l), c, Sat) * Br;\n"))
    link(col, "", fol, "C")
    link(tint, "", fol, "T")
    link(grade["FoliageSaturation"], "", fol, "Sat")
    link(grade["FoliageBrightness"], "", fol, "Br")
    lerp = mel.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate,
                                          -300, -250)
    link(trunk_col, "", lerp, "A")
    link(fol, "", lerp, "B")
    link(vc, "A", lerp, "Alpha")          # vertex alpha: 0 = bark, 1 = foliage
    mul = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -150, -100)
    link(lerp, "", mul, "A")
    link(vc, "", mul, "B")                # vertex RGB: shading / inner-crown darkening
    # Needles and leaves transmit light: Two-Sided Foliage shading with a subsurface colour of
    # the foliage tint (x alpha, so bark transmits nothing). Without it crowns render near-black.
    sss_gain = mel.create_material_expression(mat, unreal.MaterialExpressionConstant, -300, 50)
    # the grade's brightness is for the lit colour only: backlit crowns went frosty with it
    sss_gain.set_editor_property("r", 0.6 / FOLIAGE_BRIGHTNESS)
    sss = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -150, 50)
    link(fol, "", sss, "A")
    link(sss_gain, "", sss, "B")
    sss_a = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -50, 50)
    link(sss, "", sss_a, "A")
    link(vc, "A", sss_a, "B")
    if not mel.connect_material_property(sss_a, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR):
        raise RuntimeError("subsurface colour")
    # Sky occlusion under canopy (ground v2): the ground-cover actor writes the canopy over each
    # instance into custom data 0 (trees write nothing: 0 = open sky). Material AO scales only the
    # sky / indirect light, so sunflecks stay bright while the blue sky no longer floods ferns and
    # rocks under the trees (as M_Terrain's GroundCanopyOcclusion).
    canopy = mel.create_material_expression(mat, unreal.MaterialExpressionPerInstanceCustomData, -300, 320)
    canopy.set_editor_property("data_index", 0)
    cocc = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -300, 400)
    cocc.set_editor_property("parameter_name", "CanopyOcclusion")
    cocc.set_editor_property("default_value", 0.8)
    tree_h = mel.create_material_expression(mat, unreal.MaterialExpressionPerInstanceCustomData, -300, 480)
    tree_h.set_editor_property("data_index", 1)       # trees: height (cm); cover: 0
    # A tree's top is in the light: occlusion fades to 40 % of full at the crown top.
    occ = custom("EmberCanopyOcc", -100, 350, ["C", "K", "H", "Local"], (
        "float hf = H > 1.0 ? saturate(Local.z / H) : 0.0;\n"
        "return 1.0 - K * saturate(C) * (1.0 - 0.6 * hf);\n"))
    occ.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    link(canopy, "", occ, "C")
    link(cocc, "", occ, "K")
    link(tree_h, "", occ, "H")                        # Local is linked once it exists (wind block)
    if not mel.connect_material_property(occ, "", unreal.MaterialProperty.MP_AMBIENT_OCCLUSION):
        raise RuntimeError("ambient occlusion")
    rough = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -300, 200)
    rough.set_editor_property("parameter_name", "Roughness")
    rough.set_editor_property("default_value", 0.85)
    if not mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS):
        raise RuntimeError("roughness")

    # Wind (world position offset). The runtime owns the clock (WindTime: frozen for stills,
    # frame/fps for orbits), so renders stay deterministic. Offsets are relative to the INSTANCE
    # pivot (LocalPosition(instance) -> world): bend grows with height^2; per-tree phase and a
    # static per-tree lean from PerInstanceRandom; a slow gust band travels downwind; foliage
    # (vertex alpha) adds a fast small flutter, the motion that makes wind readable.
    lpos = mel.create_material_expression(mat, unreal.MaterialExpressionLocalPosition, -1300, 450)
    lpos.set_editor_property("local_origin", unreal.LocalPositionOrigin.INSTANCE)
    lpos.set_editor_property("included_offsets", unreal.PositionIncludedOffsets.EXCLUDE_OFFSETS)
    local = mel.create_material_expression(mat, unreal.MaterialExpressionTransform, -1100, 450)
    local.set_editor_property("transform_source_type",
                              unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_INSTANCE)
    local.set_editor_property("transform_type", unreal.MaterialVectorCoordTransform.TRANSFORM_WORLD)
    link(lpos, "", local, "")
    link(local, "", occ, "Local")                     # canopy occlusion's height above the pivot
    wpos = mel.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -1100, 300)
    wpos.set_editor_property("world_position_shader_offset",
                             unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    opos = mel.create_material_expression(mat, unreal.MaterialExpressionSubtract, -900, 350)
    link(wpos, "", opos, "A")             # pivot = world position - (pivot -> vertex)
    link(local, "", opos, "B")
    params = {}
    for i, (name, default) in enumerate((("WindTime", 0.0), ("WindStrength", 6.0))):
        p = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter,
                                           -1100, 640 + 90 * i)
        p.set_editor_property("parameter_name", name)
        p.set_editor_property("default_value", default)
        params[name] = p
    wdir = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -1100, 820)
    wdir.set_editor_property("parameter_name", "WindDir")
    wdir.set_editor_property("default_value", unreal.LinearColor(1.0, 0.0, 0.0, 0.0))
    wind = custom("EmberWind", -600, 500, ["Local", "Pivot", "Rnd", "A", "T", "S", "D"], (
        "float h = max(Local.z, 0.0) / 1000.0;\n"                   # height above pivot, 10 m
        "float bend = S * h * h;\n"
        "float2 p = Pivot.xy * 0.01;\n"                             # metres
        "float ph = Rnd * 6.2831853;\n"
        "float along = dot(p, D.xy);\n"
        "float gust = 0.5 + 0.5 * sin(T * 0.35 - along * 0.012);\n"
        "float sway = 0.55 + 0.45 * sin(T * 1.6 + ph) * (0.6 + 0.4 * gust);\n"
        "float3 o = float3(D.xy * bend * sway * (0.7 + 0.6 * gust), -0.15 * bend * sway);\n"
        "float la = frac(Rnd * 13.7) * 6.2831853;\n"               # static lean, 0..12 cm/(10 m)^2
        "o.xy += float2(cos(la), sin(la)) * 12.0 * frac(Rnd * 3.3) * h * h;\n"
        # Twig flutter, cm. Brad (8g): close up the branches looked springy - whole sprays
        # bouncing in phase at ~1.3 Hz by up to 9 cm. Now smaller, slower and decorrelated over
        # ~20 cm (neighbouring twigs out of step): a shimmer, not a bounce. Sway is unchanged.
        "float fp = dot(Local, float3(0.053, 0.047, 0.041)) + ph;\n"
        "float fl = A * (S / 6.0) * (0.4 + 0.6 * gust) * (0.5 + 0.9 * h);\n"
        "o += fl * float3(sin(T * 3.9 + fp) * D.x, sin(T * 3.3 + fp * 1.3) * D.y + "
        "0.3 * sin(T * 4.6 + fp), 0.4 * sin(T * 4.9 + fp * 0.7));\n"
        "return o;\n"))
    link(local, "", wind, "Local")
    link(opos, "", wind, "Pivot")
    link(rnd, "", wind, "Rnd")
    link(vc, "A", wind, "A")
    link(params["WindTime"], "", wind, "T")
    link(params["WindStrength"], "", wind, "S")
    link(wdir, "", wind, "D")

    # Fire (HCP3): each tree samples the replay's fire state at its own pivot (EmberFireActor's
    # FireTex over FireRect, same encoding as M_Terrain). Burned: charred crown and bark.
    # Burning: flickering flames in the foliage.
    rect = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -1100, 1000)
    rect.set_editor_property("parameter_name", "FireRect")
    rect.set_editor_property("default_value", unreal.LinearColor(0.0, 0.0, 1.0, 1.0))
    fire_params = {}
    for i, (name, default) in enumerate((("FireOn", 0.0), ("FireTime", 0.0), ("FireGain", 1.0),
                                         ("Consume", 0.0), ("Smoulder", 0.0), ("FireOutcomes", 0.0))):
        q = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter,
                                           -1100, 1100 + 90 * i)
        q.set_editor_property("parameter_name", name)
        q.set_editor_property("default_value", default)
        fire_params[name] = q
    fuv = mel.create_material_expression(mat, unreal.MaterialExpressionCustom, -800, 1000)
    fuv.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT2)
    fuv.set_editor_property("description", "EmberFireUV")
    ins = []
    for n in ("Pivot", "R"):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        ins.append(ci)
    fuv.set_editor_property("inputs", ins)
    fuv.set_editor_property("code", "return (Pivot.xy - R.xy) / R.zw;\n")
    link(opos, "", fuv, "Pivot")
    link(rect, "RGBA", fuv, "R")
    ftex = mel.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D,
                                          -600, 1000)
    ftex.set_editor_property("parameter_name", "FireTex")
    black = unreal.load_asset(f"{PATH}/T_LinearBlack")  # m_terrain.py (runs first)
    if black is None:
        raise RuntimeError("T_LinearBlack missing: m_terrain.py must run before m_veg.py")
    ftex.set_editor_property("texture", black)
    ftex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    link(fuv, "", ftex, "UVs")
    burnt = custom("EmberFireChar", -100, 150, ["Base", "F", "A", "On", "Rnd", "Local", "Out"], (
        "float burned = saturate(F.g * 2.0) * On;\n"
        + OUTCOME_HLSL +
        "if (Out > 0.5 && burned > 0.01) {\n"
        # 8g burned-area mosaic (Brad: every tree had orange needles): one outcome per tree
        "    float3 black = float3(0.012, 0.011, 0.010);\n"
        "    float3 orange = float3(0.22, 0.075, 0.022) * (0.8 + 0.4 * frac(Rnd * 3.1));\n"
        "    float3 charFol2 = float3(0.016, 0.012, 0.009);\n"
        "    float3 crown = oc == 0 ? Base : (oc == 1 ? orange : charFol2);\n"
        "    float charH = oc >= 2 ? 1e7 : lerp(150.0, 1200.0, saturate((ocls - 1.0) * 0.5));\n"
        "    float barkChar = 1.0 - smoothstep(charH * 0.85, charH, Local.z);\n"
        "    float3 bark = lerp(Base, black, saturate(barkChar * 1.2));\n"
        "    return lerp(Base, lerp(bark, crown, A), saturate(burned * 1.2));\n"
        "}\n"
        # HCP4: the class the stand burned at (G = 64 + 63 x class): a surface fire leaves green
        # crowns and a char band on the trunk, class 2 scorches crowns brown, class 3 (crown
        # fire) blackens them. A stream without classes is drawn as 3 (the HCP3 look).
        "float cls = 1.0 + 2.0 * saturate((F.g - 0.5) * 2.0);\n"
        "float scorch = smoothstep(1.5, 2.2, cls);\n"
        "float3 scorched = float3(0.09, 0.045, 0.02) * (0.8 + 0.4 * frac(Rnd * 3.1));\n"
        "float3 charFol = lerp(float3(0.016, 0.012, 0.009), float3(0.09, 0.045, 0.02),\n"
        "                      frac(Rnd * 5.7) * 0.6);\n"   # some crowns scorched brown
        "float3 crown = lerp(lerp(Base, scorched, scorch), charFol, smoothstep(2.4, 2.9, cls));\n"
        "float charH = lerp(150.0, 3000.0, saturate((cls - 1.0) * 0.5)) * (0.8 + 0.4 * frac(Rnd * 7.3));\n"
        "charH = cls > 2.85 ? 1e7 : charH;\n"                     # crown fire: the whole tree
        "float barkChar = 1.0 - smoothstep(charH * 0.85, charH, Local.z);\n"
        "float3 bark = lerp(Base, float3(0.012, 0.011, 0.010), saturate(barkChar * 1.2));\n"
        "return lerp(Base, lerp(bark, crown, A), saturate(burned * 1.2));\n"))
    link(mul, "", burnt, "Base")
    link(ftex, "RGBA", burnt, "F")
    link(vc, "A", burnt, "A")
    link(fire_params["FireOn"], "", burnt, "On")
    link(rnd, "", burnt, "Rnd")
    link(local, "", burnt, "Local")
    link(fire_params["FireOutcomes"], "", burnt, "Out")
    if not mel.connect_material_property(burnt, "", unreal.MaterialProperty.MP_BASE_COLOR):
        raise RuntimeError("base colour")
    glow = custom("EmberFireCrown", -100, 300,
                  ["F", "A", "On", "T", "G", "Rnd", "Local", "Sm", "WP"], (
        "float age_h = F.b * F.b * 200.0;\n"
        "float cls = F.r * 3.0;\n"                                 # burning class 1..3
        # trees torch as the front arrives, and only in crown fire (class 3); class 2 flames
        # stay low on the trunk; the old strength (R 0.725) is kept for class 3
        "float burning = 0.725 * saturate(cls) * On * exp(-age_h / 1.2);\n"
        "float torch = smoothstep(2.5, 2.9, cls) + (1.0 - smoothstep(2.5, 2.9, cls)) * smoothstep(1.5, 1.9, cls)\n"
        "            * (1.0 - smoothstep(200.0, 400.0, Local.z));\n"
        "burning *= torch;\n"
        "float h = saturate(Local.z / 3000.0);\n"
        # H4-5: tongues of flame licking up through the crown (3-D value noise scrolling upward
        # on the fire clock), not the whole crown painted orange (the HCP3 "torching blowout").
        + vnoise3("tq", "Local * 0.012 + float3(Rnd * 31.0, Rnd * 17.0, -T * 2.4)", "tn") +
        "float tongue = saturate((tn - 0.58) * 5.0);\n"
        "float3 flame = lerp(float3(5.0, 1.0, 0.1), float3(8.0, 3.6, 0.7), h * tn);\n"
        # the flame shapes are M_Flame's cards now; the crown only glows through them
        "float3 e = 0.25 * burning * (0.15 + 0.85 * A) * tongue * tongue * flame;\n"
        # smouldering wood: starts ~20 min after the front, fades over ~a day, in patches (value
        # noise; the old product of sines repeated every ~3 m and banded trunks)
        "float sm = Sm * saturate(F.g * 2.0) * On * (1.0 - A) * saturate(age_h / 0.3) * exp(-age_h / 20.0);\n"
        + vnoise3("sq", "Local * 0.035 + Rnd * 17.0", "sn") +
        "float patch = saturate((sn - 0.66) * 6.0);\n"
        "float pulse = 0.75 + 0.25 * sin(T * 1.1 + Rnd * 30.0);\n"
        "e += sm * patch * pulse * float3(2.6, 0.38, 0.03);\n"
        "return G * e;\n"))
    link(ftex, "RGBA", glow, "F")
    link(vc, "A", glow, "A")
    link(fire_params["FireOn"], "", glow, "On")
    link(fire_params["FireTime"], "", glow, "T")
    link(fire_params["FireGain"], "", glow, "G")
    link(rnd, "", glow, "Rnd")
    link(local, "", glow, "Local")
    link(fire_params["Smoulder"], "", glow, "Sm")
    link(wpos, "", glow, "WP")
    if not mel.connect_material_property(glow, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        raise RuntimeError("emissive")
    ftex_v = mel.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D,
                                            -600, 1250)
    ftex_v.set_editor_property("parameter_name", "FireTex")
    ftex_v.set_editor_property("texture", black)
    ftex_v.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    ftex_v.set_editor_property("mip_value_mode", unreal.TextureMipValueMode.TMVM_MIP_LEVEL)
    ftex_v.set_editor_property("const_mip_value", 0)
    link(fuv, "", ftex_v, "UVs")
    tree_h2 = mel.create_material_expression(mat, unreal.MaterialExpressionPerInstanceCustomData, -600, 700)
    tree_h2.set_editor_property("data_index", 1)      # trees: height (cm)
    consume = custom("EmberConsume", -300, 600, ["O", "L", "F", "On", "C", "Out", "Rnd", "A", "H"], (
        "float burned = saturate(F.g * 2.0) * On;\n"
        "float k = saturate(burned * C) * 0.85;\n"   # burned cover: ~15 % stubble
        "float3 o = O * (1.0 - k) - L * k;\n"
        "if (Out > 0.5 && burned > 0.5) {\n"
        + OUTCOME_HLSL.replace("    ", "") +
        # bare skeleton: needles gone; consumed: needles and everything above ~25 % height gone (a
        # snag). Collapse is HORIZONTAL onto the trunk axis - triangles become zero-width lines,
        # invisible - so the offset stays within a crown radius (Nanite clamps WPO to the
        # material's max displacement; a collapse to the base would need the tree's height).
        "    float3 axis = float3(-L.x, -L.y, 0.0);\n"
        "    if (oc >= 3) { o = A > 0.5 ? axis : float3(0, 0, 0); }\n"   # no wind on a dead stem
        "    if (oc == 4 && H > 1.0 && L.z > 0.25 * H) { o = axis; }\n"
        "}\n"
        "return o;\n"))
    link(wind, "", consume, "O")
    link(local, "", consume, "L")
    link(ftex_v, "RGBA", consume, "F")
    link(fire_params["FireOn"], "", consume, "On")
    link(fire_params["Consume"], "", consume, "C")
    link(fire_params["FireOutcomes"], "", consume, "Out")
    link(rnd, "", consume, "Rnd")
    link(vc, "A", consume, "A")
    link(tree_h2, "", consume, "H")
    if not mel.connect_material_property(consume, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET):
        raise RuntimeError("world position offset")
    mat.set_editor_property("two_sided", True)                  # foliage is single-layer sprays
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    mat.set_editor_property("max_world_position_offset_displacement", 900.0)  # Nanite WPO bounds: sway + burned collapse onto the trunk (<= crown radius)
    mat.set_editor_property("used_with_instanced_static_meshes", True)
    mat.set_editor_property("used_with_nanite", True)
    mel.recompile_material(mat)
    eal.save_asset(full, only_if_is_dirty=False)
    unreal.log(f"EMBER_GENERATED {full}")
    return mat



build_material()

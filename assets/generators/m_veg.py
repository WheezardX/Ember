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
    fol = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -450, -150)
    link(col, "", fol, "A")
    link(tint, "", fol, "B")
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
    sss_gain.set_editor_property("r", 0.6)
    sss = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -150, 50)
    link(fol, "", sss, "A")
    link(sss_gain, "", sss, "B")
    sss_a = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -50, 50)
    link(sss, "", sss_a, "A")
    link(vc, "A", sss_a, "B")
    if not mel.connect_material_property(sss_a, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR):
        raise RuntimeError("subsurface colour")
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
        "float fp = dot(Local, float3(0.021, 0.017, 0.013)) + ph;\n"
        "float fl = A * (S / 6.0) * (0.4 + 0.6 * gust) * (1.5 + 2.5 * h);\n"  # twig flutter, cm
        "o += fl * float3(sin(T * 7.3 + fp) * D.x, sin(T * 6.1 + fp * 1.3) * D.y + "
        "0.4 * sin(T * 8.7 + fp), 0.6 * sin(T * 9.1 + fp * 0.7));\n"
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
                                         ("Consume", 0.0), ("Smoulder", 0.0))):
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
    burnt = custom("EmberFireChar", -100, 150, ["Base", "F", "A", "On", "Rnd"], (
        "float burned = F.g * F.a * On;\n"
        "float3 charFol = lerp(float3(0.016, 0.012, 0.009), float3(0.09, 0.045, 0.02),\n"
        "                      frac(Rnd * 5.7) * 0.6);\n"   # some crowns scorched brown
        "float3 charc = lerp(float3(0.012, 0.011, 0.010), charFol, A);\n"
        "return lerp(Base, charc, saturate(burned * 1.2));\n"))
    link(mul, "", burnt, "Base")
    link(ftex, "RGBA", burnt, "F")
    link(vc, "A", burnt, "A")
    link(fire_params["FireOn"], "", burnt, "On")
    link(rnd, "", burnt, "Rnd")
    if not mel.connect_material_property(burnt, "", unreal.MaterialProperty.MP_BASE_COLOR):
        raise RuntimeError("base colour")
    glow = custom("EmberFireCrown", -100, 300,
                  ["F", "A", "On", "T", "G", "Rnd", "Local", "Sm", "WP"], (
        "float age_h = F.b * F.b * 200.0;\n"
        "float burning = F.r * F.a * On * exp(-age_h / 1.2);\n"   # trees torch as the front arrives
        "float h = saturate(Local.z / 3000.0);\n"
        "float flick = 0.6 + 0.4 * sin(T * 8.0 + Rnd * 40.0 + Local.z * 0.01);\n"
        "float3 flame = lerp(float3(7.0, 1.2, 0.1), float3(10.0, 4.5, 0.8), h) * flick;\n"
        "float3 e = burning * (0.25 + 0.75 * A) * flame;\n"
        # smouldering wood: starts ~20 min after the front, fades over ~a day, in patches
        "float sm = Sm * F.g * F.a * On * (1.0 - A) * saturate(age_h / 0.3) * exp(-age_h / 20.0);\n"
        "float3 q = WP * 0.02 + Rnd * 17.0;\n"
        "float patch = saturate(sin(q.x) * sin(q.y * 1.3) * sin(q.z * 1.7) * 4.0 - 0.8);\n"
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
    consume = custom("EmberConsume", -300, 600, ["O", "L", "F", "On", "C"], (
        "float k = saturate(F.g * F.a * On * C) * 0.85;\n"   # burned: down to ~15 % stubble
        "return O * (1.0 - k) - L * k;\n"))
    link(wind, "", consume, "O")
    link(local, "", consume, "L")
    link(ftex_v, "RGBA", consume, "F")
    link(fire_params["FireOn"], "", consume, "On")
    link(fire_params["Consume"], "", consume, "C")
    if not mel.connect_material_property(consume, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET):
        raise RuntimeError("world position offset")
    mat.set_editor_property("two_sided", True)                  # foliage is single-layer sprays
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    mat.set_editor_property("max_world_position_offset_displacement", 700.0)  # Nanite WPO bounds
    mat.set_editor_property("used_with_instanced_static_meshes", True)
    mat.set_editor_property("used_with_nanite", True)
    mel.recompile_material(mat)
    eal.save_asset(full, only_if_is_dirty=False)
    unreal.log(f"EMBER_GENERATED {full}")
    return mat



build_material()

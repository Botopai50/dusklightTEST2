#!/usr/bin/env python3
"""Add the BOTW v2.3 lightweight cross-backend shadow path.

The full shadow-map mod replays the opaque scene into a second render pass every frame. That is
expensive and its offscreen depth snapshot is not available on every Dawn backend. This transform:

* adds a short directional screen-space raymarch to the existing BOTW finishing pass;
* keeps the game's original actor/blob shadows in the lightweight path;
* disables the full shadow map by default and marks it as an optional high-end mode;
* reduces the optional map to actor-focused casters, a smaller map and a tighter light volume;
* avoids an unnecessary color snapshot when the optional map is rendered.

It runs after apply_botw_shadow_preset.py and therefore targets the v2.2 transformed sources.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

SOURCE_MARKER = "DUSKLIGHT_LIGHTWEIGHT_SHADOW_SOURCE_V23"
SHADER_MARKER = "DUSKLIGHT_LIGHTWEIGHT_SHADOW_SHADER_V23"
FULL_MAP_MARKER = "DUSKLIGHT_FULL_MAP_OPTIMIZED_V23"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise RuntimeError(f"Could not find expected {label} block")
    return text.replace(old, new, 1)


def patch_full_shadow_source(root: Path) -> bool:
    path = root / "mods/shadow_mod/src/mod.cpp"
    text = path.read_text(encoding="utf-8")
    if FULL_MAP_MARKER in text:
        print(f"BOTW v2.3 optional full shadow map already optimized: {path}")
        return False

    text = replace_once(
        text,
        "// Dynamic shadows example mod.\n",
        "// Dynamic shadows example mod.\n"
        "// DUSKLIGHT_FULL_MAP_OPTIMIZED_V23\n"
        "// Optional high-end path. The default cross-backend shadow is implemented in the BOTW\n"
        "// visual pass and does not replay the scene.\n",
        "shadow source header",
    )

    old_constants = '''#if defined(__ANDROID__)
    constexpr int64_t botwMapSize = 1; // 2048
    constexpr int64_t botwCoverage = 2600;
#else
    constexpr int64_t botwMapSize = 2; // 4096
    constexpr int64_t botwCoverage = 3000;
#endif
'''
    new_constants = '''#if defined(__ANDROID__)
    constexpr int64_t botwMapSize = 0; // 1024, optional high-end path
    constexpr int64_t botwCoverage = 2200;
#else
    constexpr int64_t botwMapSize = 1; // 2048, optional high-end path
    constexpr int64_t botwCoverage = 2600;
#endif
'''
    text = replace_once(text, old_constants, new_constants, "full-map platform defaults")

    replacements = [
        (
            '    result = register_bool_option("effectEnabled", true, g_cvarEnabled, error);',
            '    result = register_bool_option("fullShadowMapEnabledV23", false, g_cvarEnabled, error);',
            "full-map enabled key",
        ),
        (
            '    result = register_int_option("mapSizeSharpV22", botwMapSize, g_cvarMapSize, error);',
            '    result = register_int_option("mapSizeFullV23", botwMapSize, g_cvarMapSize, error);',
            "map-size key",
        ),
        (
            '    result = register_bool_option("noFrustumClippingSharpV22", false, g_cvarNoFrustumClipping, error);',
            '    result = register_bool_option("noFrustumClippingFullV23", false, g_cvarNoFrustumClipping, error);',
            "frustum key",
        ),
        (
            '    result = register_int_option("strengthSharpV22", 42, g_cvarStrength, error);',
            '    result = register_int_option("strengthFullV23", 38, g_cvarStrength, error);',
            "strength key",
        ),
        (
            '    result = register_int_option("pcfSharpV22", 0, g_cvarPcf, error);',
            '    result = register_int_option("pcfFullV23", 0, g_cvarPcf, error);',
            "PCF key",
        ),
        (
            '    result = register_int_option("biasSharpV22", 12, g_cvarBias, error);',
            '    result = register_int_option("biasFullV23", 12, g_cvarBias, error);',
            "bias key",
        ),
        (
            '    result = register_int_option("boxRadiusSharpV22", botwCoverage, g_cvarBoxRadius, error);',
            '    result = register_int_option("boxRadiusFullV23", botwCoverage, g_cvarBoxRadius, error);',
            "coverage key",
        ),
        (
            '    result = register_int_option("edgeFadeWidthSharpV22", 6, g_cvarEdgeFadeWidth, error);',
            '    result = register_int_option("edgeFadeWidthFullV23", 6, g_cvarEdgeFadeWidth, error);',
            "edge fade key",
        ),
        (
            '    result = register_bool_option("contactShadowsSharpV22", false, g_cvarContactShadows, error);',
            '    result = register_bool_option("contactShadowsFullV23", false, g_cvarContactShadows, error);',
            "contact key",
        ),
    ]
    for old, new, label in replacements:
        text = replace_once(text, old, new, label)

    old_draw_lists = '''void draw_opaque_scene_lists() {
    dComIfGd_drawOpaListBG();
    dComIfGd_drawOpaListDarkBG();
    dComIfGd_drawOpaListMiddle();
    dComIfGd_drawOpaList();
    dComIfGd_drawOpaListDark();
    dComIfGd_drawOpaListPacket();
}
'''
    new_draw_lists = '''void draw_opaque_scene_lists() {
    // BOTW v2.3 optional full-map mode: replay actor/object lists only. Re-rendering both terrain
    // lists doubled the cost of large outdoor scenes and is unnecessary for character silhouettes.
    dComIfGd_drawOpaListMiddle();
    dComIfGd_drawOpaList();
    dComIfGd_drawOpaListDark();
    dComIfGd_drawOpaListPacket();
}
'''
    text = replace_once(text, old_draw_lists, new_draw_lists, "actor-focused caster lists")

    old_resolve = '''    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    resolveDesc.color = true;
    resolveDesc.depth = true;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) != MOD_OK ||
        resolved.color == nullptr || resolved.depth == nullptr)
    {
        return;
    }

    j3dSys.reinitGX();
    J3DShape::resetVcdVatCache();
    restore_game_camera();

    g_mapPass.lightColor = resolved.color;
    g_mapPass.shadowMap = resolved.depth;
'''
    new_resolve = '''    GfxResolveDesc resolveDesc = GFX_RESOLVE_DESC_INIT;
    // Production needs only depth. The old color snapshot added a large copy and extra memory
    // traffic. Camera-replay debug keeps color available when explicitly selected.
    resolveDesc.color = cameraReplayDebug;
    resolveDesc.depth = true;
    GfxResolvedTargets resolved = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolveDesc, &resolved) != MOD_OK ||
        resolved.depth == nullptr)
    {
        return;
    }

    j3dSys.reinitGX();
    J3DShape::resetVcdVatCache();
    restore_game_camera();

    g_mapPass.lightColor = resolved.color != nullptr ? resolved.color : resolved.depth;
    g_mapPass.shadowMap = resolved.depth;
'''
    text = replace_once(text, old_resolve, new_resolve, "depth-only full-map resolve")

    text = replace_once(
        text,
        '    svc_ui->pane_add_section(mod_ctx, left, "Shadow Map");\n'
        '    add_toggle(left, "Enabled", g_cvarEnabled, "Enables dynamic shadows.");',
        '    svc_ui->pane_add_section(mod_ctx, left, "Full Shadow Map (High-end)");\n'
        '    add_toggle(left, "Enabled", g_cvarEnabled,\n'
        '        "Optional actor-focused shadow map. The lightweight cross-backend shadow is in "\n'
        '        "BOTW Visual Suite controls. This mode is intended for Vulkan or strong GPUs.");',
        "full-map UI label",
    )

    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Applied BOTW v2.3 optional full-map optimizations: {path}")
    return True


def patch_visual_source(root: Path) -> bool:
    path = root / "mods/botw_visual_mod/src/mod.cpp"
    text = path.read_text(encoding="utf-8")
    if SOURCE_MARKER in text:
        print(f"BOTW v2.3 lightweight shadow source already applied: {path}")
        return False

    text = replace_once(
        text,
        '// Breath of the Wild-inspired visual suite v2.2.\n'
        '// DUSKLIGHT_TEXTURE_SAFE_SOURCE_V22\n',
        '// Breath of the Wild-inspired visual suite v2.3.\n'
        '// DUSKLIGHT_TEXTURE_SAFE_SOURCE_V22\n'
        '// DUSKLIGHT_LIGHTWEIGHT_SHADOW_SOURCE_V23\n',
        "visual v2.3 header",
    )

    text = replace_once(
        text,
        'ConfigVarHandle g_aoStrength = 0;\n',
        'ConfigVarHandle g_aoStrength = 0;\n'
        'ConfigVarHandle g_screenShadowStrength = 0;\n'
        'ConfigVarHandle g_screenShadowReach = 0;\n',
        "lightweight shadow config declarations",
    )

    text = replace_once(
        text,
        '    uniforms.effects[2] = 1.0f;\n'
        '    uniforms.effects[3] = 1.0f;\n',
        '    // Cross-backend directional shadow: integrated into this existing fullscreen pass, so\n'
        '    // it does not replay scene geometry or allocate a shadow map.\n'
        '    uniforms.effects[2] = percent(g_screenShadowStrength, 30);\n'
        '    uniforms.effects[3] = static_cast<float>(\n'
        '        std::clamp<int64_t>(get_int(g_screenShadowReach, 260), 40, 800));\n',
        "lightweight shadow uniforms",
    )

    text = replace_once(
        text,
        '    add_number(right, "Contact AO", g_aoStrength, 0, 100, 5, "%",\n'
        '        "Lightweight depth-based grounding.");\n',
        '    add_number(right, "Contact AO", g_aoStrength, 0, 100, 5, "%",\n'
        '        "Lightweight depth-based grounding.");\n'
        '    add_number(right, "Directional Shadow", g_screenShadowStrength, 0, 100, 5, "%",\n'
        '        "Cross-backend screen-space shadow. Uses 5 samples on mobile and 7 on medium quality; "\n'
        '        "does not redraw the scene.");\n'
        '    add_number(right, "Shadow Reach", g_screenShadowReach, 40, 800, 20, "",\n'
        '        "View-space distance marched toward the sun. Lower values are faster and more stable.");\n',
        "lightweight shadow controls",
    )

    text = replace_once(
        text,
        '        (result = register_int("contactAoV22", 10, g_aoStrength, error)) != MOD_OK ||\n',
        '        (result = register_int("contactAoV22", 10, g_aoStrength, error)) != MOD_OK ||\n'
        '        (result = register_int("directionalShadowV23", 30, g_screenShadowStrength, error)) != MOD_OK ||\n'
        '        (result = register_int("directionalShadowReachV23", 260, g_screenShadowReach, error)) != MOD_OK ||\n',
        "lightweight shadow config registration",
    )

    text = text.replace('tab.title = "BOTW Visual Suite v2";', 'tab.title = "BOTW Visual Suite v2.3";', 1)
    text = text.replace('draw.label = "BOTW visual suite v2";', 'draw.label = "BOTW visual suite v2.3";', 1)
    text = text.replace(
        'svc_log->info(mod_ctx, "Breath of the Wild visual suite v2 ready");',
        'svc_log->info(mod_ctx, "Breath of the Wild visual suite v2.3 ready");',
        1,
    )

    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Applied BOTW v2.3 lightweight shadow controls: {path}")
    return True


def patch_visual_shader(root: Path) -> bool:
    path = root / "mods/botw_visual_mod/res/botw_visual.wgsl"
    text = path.read_text(encoding="utf-8")
    if SHADER_MARKER in text:
        print(f"BOTW v2.3 lightweight shadow shader already applied: {path}")
        return False

    function_anchor = '''fn analytic_sky(uv: vec2f) -> vec3f {
'''
    lightweight_function = '''// DUSKLIGHT_LIGHTWEIGHT_SHADOW_SHADER_V23
// Directional screen-space shadow. It reads the already resolved scene depth and marches a short
// ray toward the sun. Unlike the optional shadow map it performs no scene replay and uses no
// offscreen render target, making it suitable for Vulkan, D3D12 and OpenGL ES/Dawn backends.
fn directional_screen_shadow(origin: vec3f, pixel: vec2f) -> f32 {
    if u.effects.z <= 0.001 || u.sun_dir_view.w <= 0.001 {
        return 0.0;
    }

    let view_distance = max(-origin.z, 0.0);
    let distance_fade = 1.0 - smoothstep(2600.0, 6200.0, view_distance);
    if distance_fade <= 0.001 {
        return 0.0;
    }

    let steps = select(5u, select(7u, 9u, u.flags.z >= 2u), u.flags.z >= 1u);
    let light_dir = safe_normalize(u.sun_dir_view.xyz, vec3f(0.0, 1.0, -1.0));
    let step_vec = light_dir * (max(u.effects.w, 40.0) / f32(steps));
    let jitter = 0.20 + hash12(pixel * 0.5) * 0.55;
    var ray = origin + step_vec * jitter;

    for (var i = 0u; i < 9u; i += 1u) {
        if i >= steps {
            break;
        }
        ray += step_vec;
        let clip = u.proj_from_view * vec4f(ray, 1.0);
        if clip.w <= 0.0001 {
            break;
        }

        let ndc = clip.xyz / clip.w;
        let ray_uv = vec2f(0.5 + ndc.x * 0.5, 0.5 - ndc.y * 0.5);
        if any(ray_uv <= vec2f(0.001)) || any(ray_uv >= vec2f(0.999)) {
            break;
        }

        let sample_depth = load_depth_uv(ray_uv);
        if geometry_mask_from_depth(sample_depth) <= 0.001 {
            continue;
        }
        let scene_position = unproject_view(ray_uv, sample_depth);
        let delta = scene_position.z - ray.z;
        let progress = f32(i + 1u) / f32(steps);
        let thickness = 10.0 + progress * 30.0 + view_distance * 0.0012;
        if delta > 1.5 && delta < thickness {
            return distance_fade * (1.0 - progress * 0.22);
        }
    }
    return 0.0;
}

'''
    text = replace_once(text, function_anchor, lightweight_function + function_anchor,
        "analytic sky anchor for lightweight shadow function")

    apply_anchor = '''    color = apply_material_response(color, masks, normal, view_dir, coord, depth);

'''
    apply_block = '''    color = apply_material_response(color, masks, normal, view_dir, coord, depth);

    // Add a restrained cool directional shadow while preserving the game's original actor shadows.
    // The strength is intentionally modest because this is a near-field enhancement, not a second
    // full shadow-map replacement.
    if u.effects.z > 0.001 && geometry > 0.001 {
        let screen_shadow = directional_screen_shadow(view_pos, in.position.xy);
        let shadow_amount = clamp(screen_shadow * u.effects.z * style, 0.0, 0.55);
        let cool_shadow_factor = vec3f(0.66, 0.71, 0.82);
        color *= mix(vec3f(1.0), cool_shadow_factor, shadow_amount);
    }

'''
    text = replace_once(text, apply_anchor, apply_block, "lightweight shadow application")

    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Applied BOTW v2.3 lightweight cross-backend shadow shader: {path}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()

    if not (root / "mods/botw_visual_mod/src/mod.cpp").is_file():
        raise RuntimeError(f"Dusklight source tree not found at {root}")

    changed = patch_full_shadow_source(root)
    changed = patch_visual_source(root) or changed
    changed = patch_visual_shader(root) or changed
    print("BOTW v2.3 lightweight shadow ready" + (" (source updated)" if changed else ""))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)

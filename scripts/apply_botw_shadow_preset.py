#!/usr/bin/env python3
"""Apply the texture-safe BOTW visual preset and the sharp projected-shadow preset.

The first v2 preset was too aggressive: it filtered/posterized the entire scene and the projected
shadow remained soft even at high map resolutions because a bilinear comparison was always used.
This deterministic configure-time transform keeps the original game textures untouched and gives
"Soft Shadows: Off" a true nearest-depth comparison with a tighter light frustum.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

SHADOW_SOURCE_MARKER = "DUSKLIGHT_SHARP_SHADOW_V21"
SHADOW_SHADER_MARKER = "DUSKLIGHT_SHARP_SHADOW_SAMPLE_V21"
SHADOW_COLOR_MARKER = "DUSKLIGHT_BOTW_COLORED_SHADOW"
VISUAL_SOURCE_MARKER = "DUSKLIGHT_TEXTURE_SAFE_SOURCE_V21"
VISUAL_SHADER_MARKER = "DUSKLIGHT_TEXTURE_SAFE_SHADER_V21"
VISUAL_PORTABILITY_MARKER = "DUSKLIGHT_BOTW_WGSL_PORTABILITY"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise RuntimeError(f"Could not find expected {label} block")
    return text.replace(old, new, 1)


def replace_all_required(text: str, replacements: list[tuple[str, str, str]]) -> str:
    for old, new, label in replacements:
        text = replace_once(text, old, new, label)
    return text


def patch_shadow_source(root: Path) -> bool:
    path = root / "mods/shadow_mod/src/mod.cpp"
    text = path.read_text(encoding="utf-8")
    if SHADOW_SOURCE_MARKER in text:
        print(f"Sharp projected-shadow source preset already applied: {path}")
        return False

    old = '''    result = register_bool_option("effectEnabled", false, g_cvarEnabled, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("mapSize", 2, g_cvarMapSize, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_bool_option("noFrustumClipping", true, g_cvarNoFrustumClipping, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("strength", 45, g_cvarStrength, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("pcf", 2, g_cvarPcf, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("bias", 55, g_cvarBias, error);
'''
    new = '''    // DUSKLIGHT_SHARP_SHADOW_V21
    // Reset the persisted v2 values by using new keys. Definition depends more on coverage and
    // filtering than on raw resolution: Windows uses 4096 with a tight box; Android uses 2048.
#if defined(__ANDROID__)
    constexpr int64_t botwMapSize = 1; // 2048
#else
    constexpr int64_t botwMapSize = 2; // 4096
#endif
    result = register_bool_option("effectEnabled", true, g_cvarEnabled, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("mapSizeSharpV21", botwMapSize, g_cvarMapSize, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_bool_option("noFrustumClippingSharpV21", false, g_cvarNoFrustumClipping, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("strengthSharpV21", 44, g_cvarStrength, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("pcfSharpV21", 0, g_cvarPcf, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("biasSharpV21", 15, g_cvarBias, error);
'''
    text = replace_once(text, old, new, "shadow defaults")
    text = replace_all_required(
        text,
        [
            (
                '    result = register_int_option("boxRadius", 6000, g_cvarBoxRadius, error);',
                '    result = register_int_option("boxRadiusSharpV21", 3500, g_cvarBoxRadius, error);',
                "shadow coverage default",
            ),
            (
                '    result = register_int_option("edgeFadeWidth", 32, g_cvarEdgeFadeWidth, error);',
                '    result = register_int_option("edgeFadeWidthSharpV21", 8, g_cvarEdgeFadeWidth, error);',
                "shadow edge-fade default",
            ),
            (
                '    result = register_bool_option("contactShadows", true, g_cvarContactShadows, error);',
                '    result = register_bool_option("contactShadowsSharpV21", false, g_cvarContactShadows, error);',
                "contact-shadow default",
            ),
        ],
    )
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Applied sharp projected-shadow source preset: {path}")
    return True


def patch_shadow_shader(root: Path) -> bool:
    path = root / "mods/shadow_mod/res/shadow.wgsl"
    text = path.read_text(encoding="utf-8")
    changed = False

    if SHADOW_SHADER_MARKER not in text:
        old = '''fn sample_shadow_pcf(light_uv: vec2f, receiver: f32) -> f32 {
    let radius = i32(uniforms.pcf_taps);
    var sum = 0.0;
    var count = 0.0;
    for (var y = -radius; y <= radius; y += 1i) {
        for (var x = -radius; x <= radius; x += 1i) {
            let offset = vec2f(f32(x), f32(y)) * uniforms.inv_size;
            sum += shadow_compare_bilinear(light_uv + offset, receiver);
            count += 1.0;
        }
    }
    return sum / count;
}
'''
        new = '''// DUSKLIGHT_SHARP_SHADOW_SAMPLE_V21
fn sample_shadow_pcf(light_uv: vec2f, receiver: f32) -> f32 {
    let radius = i32(uniforms.pcf_taps);

    // "Soft Shadows: Off" now means a real hard comparison. The old implementation still used
    // four bilinear comparisons here, so 4096 looked almost as blurry as 1024.
    if radius <= 0i {
        let texel = vec2<i32>(floor(light_uv * uniforms.size));
        return shadow_test(texel, receiver);
    }

    var sum = 0.0;
    var count = 0.0;
    for (var y = -radius; y <= radius; y += 1i) {
        for (var x = -radius; x <= radius; x += 1i) {
            let offset = vec2f(f32(x), f32(y)) * uniforms.inv_size;
            sum += shadow_compare_bilinear(light_uv + offset, receiver);
            count += 1.0;
        }
    }
    return sum / count;
}
'''
        text = replace_once(text, old, new, "shadow PCF sampler")
        changed = True

    if SHADOW_COLOR_MARKER not in text:
        old = '''    let value = 1.0 - uniforms.strength * occlusion;
    if uniforms.debug_mode == 2u {
        return vec4f(value, value, value, 1.0);
    }
    return vec4f(value, value, value, 1.0);
'''
        new = '''    let value = 1.0 - uniforms.strength * occlusion;
    if uniforms.debug_mode == 2u {
        return vec4f(value, value, value, 1.0);
    }

    // DUSKLIGHT_BOTW_COLORED_SHADOW
    // Keep the silhouette contrast high while using only a restrained cool tint.
    let amount = uniforms.strength * occlusion;
    let colored_factor = vec3f(
        1.0 - amount * 1.04,
        1.0 - amount * 1.00,
        1.0 - amount * 0.94);
    return vec4f(clamp(colored_factor, vec3f(0.0), vec3f(1.0)), 1.0);
'''
        text = replace_once(text, old, new, "shadow composite")
        changed = True

    if changed:
        path.write_text(text, encoding="utf-8", newline="\n")
        print(f"Applied sharp projected-shadow shader preset: {path}")
    else:
        print(f"Sharp projected-shadow shader preset already applied: {path}")
    return changed


def patch_visual_source(root: Path) -> bool:
    path = root / "mods/botw_visual_mod/src/mod.cpp"
    text = path.read_text(encoding="utf-8")
    if VISUAL_SOURCE_MARKER in text:
        print(f"Texture-safe visual source preset already applied: {path}")
        return False

    text = replace_once(
        text,
        '// Breath of the Wild-inspired visual suite v2.\n',
        '// Breath of the Wild-inspired visual suite v2.1.\n'
        '// DUSKLIGHT_TEXTURE_SAFE_SOURCE_V21\n',
        "visual source header",
    )

    # Disable destructive/heuristic screen-space effects in the compiled preset. The renderer-
    # integrated toon lighting remains active, while original texture texels are preserved.
    text = replace_all_required(
        text,
        [
            (
                '    uniforms.params2[1] = percent(g_waterStrength, 28);',
                '    uniforms.params2[1] = 0.0f; // Texture-safe: no color-based water guessing.',
                "water uniform",
            ),
            (
                '    uniforms.params3[1] = percent(g_textureStyle, 22);',
                '    uniforms.params3[1] = 0.0f; // Never blur/posterize original textures.',
                "texture-style uniform",
            ),
            (
                '    uniforms.params3[2] = percent(g_edgeAaStrength, 35);',
                '    uniforms.params3[2] = 0.0f; // Preserve texture and silhouette definition.',
                "edge-AA uniform",
            ),
            (
                '    uniforms.params3[3] = percent(g_skyStrength, 60);',
                '    uniforms.params3[3] = 0.0f; // Preserve the original game skybox.',
                "analytic-sky uniform",
            ),
            (
                '    uniforms.effects[0] = percent(g_foliageStrength, 38);',
                '    uniforms.effects[0] = 0.0f; // No color-based foliage classification.',
                "foliage uniform",
            ),
            (
                '    uniforms.effects[1] = percent(g_wetnessStrength, 45);',
                '    uniforms.effects[1] = 0.0f; // No global material wetness guessing.',
                "wetness uniform",
            ),
            (
                '    uniforms.sun_screen[2] = sunVisible ? percent(g_sunRayStrength, 18) : 0.0f;',
                '    uniforms.sun_screen[2] = 0.0f; // Disabled until an occlusion-safe implementation.',
                "sun-ray uniform",
            ),
            (
                '    uniforms.sun_screen[3] = percent(g_dofStrength, 18);',
                '    uniforms.sun_screen[3] = 0.0f; // Do not blur gameplay textures.',
                "DOF uniform",
            ),
        ],
    )

    # New config keys ignore the already-saved aggressive v2 values and start from conservative
    # defaults. Safe effects remain adjustable; destructive effects are hard-disabled above.
    replacements = [
        ('register_int("styleIntensity", 82', 'register_int("styleIntensitySafeV21", 45', "style key"),
        ('register_int("shadowLift", 28', 'register_int("shadowLiftSafeV21", 12', "shadow lift key"),
        ('register_int("exposure", 5', 'register_int("exposureSafeV21", 0', "exposure key"),
        ('register_int("contrast", 105', 'register_int("contrastSafeV21", 100', "contrast key"),
        ('register_int("vibrance", 112', 'register_int("vibranceSafeV21", 104', "vibrance key"),
        ('register_int("textureStyle", 22', 'register_int("textureStyleSafeV21", 0', "texture key"),
        ('register_int("fogStrength", 30', 'register_int("fogStrengthSafeV21", 12', "fog key"),
        ('register_int("bloomStrength", 18', 'register_int("bloomStrengthSafeV21", 8', "bloom key"),
        ('register_int("waterStrength", 28', 'register_int("waterStrengthSafeV21", 0', "water key"),
        ('register_int("contactAo", 18', 'register_int("contactAoSafeV21", 8', "AO key"),
        ('register_int("foliageTransmission", 38', 'register_int("foliageSafeV21", 0', "foliage key"),
        ('register_int("wetnessStrength", 45', 'register_int("wetnessSafeV21", 0', "wetness key"),
        ('register_int("edgeAaStrength", 35', 'register_int("edgeAaSafeV21", 0', "edge-AA key"),
        ('register_int("sunRayStrength", 18', 'register_int("sunRaysSafeV21", 0', "sun-ray key"),
        ('register_int("dofStrength", 18', 'register_int("dofSafeV21", 0', "DOF key"),
        ('register_int("skyStrength", 60', 'register_int("skySafeV21", 0', "sky key"),
    ]
    text = replace_all_required(text, replacements)

    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Applied texture-safe visual source preset: {path}")
    return True


def patch_visual_shader(root: Path) -> bool:
    path = root / "mods/botw_visual_mod/res/botw_visual.wgsl"
    text = path.read_text(encoding="utf-8")
    changed = False

    if VISUAL_PORTABILITY_MARKER not in text:
        old_hash = '''fn hash12(p: vec2f) -> f32 {
    let p3 = fract(vec3f(p.xyx) * 0.1031);
    let q = p3 + dot(p3, p3.yzx + vec3f(33.33));
    return fract((q.x + q.y) * q.z);
}
'''
        new_hash = '''// DUSKLIGHT_BOTW_WGSL_PORTABILITY
fn hash12(p: vec2f) -> f32 {
    let p3 = fract(p.xyx * 0.1031);
    let q = p3 + vec3f(dot(p3, p3.yzx + vec3f(33.33)));
    return fract((q.x + q.y) * q.z);
}
'''
        text = replace_once(text, old_hash, new_hash, "typed cloud-noise hash")
        text = replace_once(
            text,
            "    let radius = select(2, 3, u.flags.z >= 2u);",
            "    let radius = select(2i, 3i, u.flags.z >= 2u);",
            "typed AO radius",
        )
        changed = True

    if VISUAL_SHADER_MARKER not in text:
        old = '''    var color = edge_aa(coord, max(load_color(coord), vec3f(0.0)));
    color = stylize_texture(coord, color, geometry);

    let masks = classify_material(color, normal, geometry);
'''
        new = '''    // DUSKLIGHT_TEXTURE_SAFE_SHADER_V21
    // Preserve the exact scene texel. No full-screen neighbor averaging and no posterization.
    var color = max(load_color(coord), vec3f(0.0));

    // Masks remain available only for debug views; they no longer modify scene materials.
    let masks = classify_material(color, normal, geometry);
'''
        text = replace_once(text, old, new, "texture-safe scene sampling")
        text = replace_once(
            text,
            "    color = apply_material_response(color, masks, normal, view_dir, coord, depth);",
            "    // Material guessing is intentionally disabled: it was recoloring unrelated textures.",
            "material response call",
        )
        changed = True

    if changed:
        path.write_text(text, encoding="utf-8", newline="\n")
        print(f"Applied texture-safe visual shader preset: {path}")
    else:
        print(f"Texture-safe visual shader preset already applied: {path}")
    return changed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    if not (root / "mods/shadow_mod/src/mod.cpp").is_file():
        raise RuntimeError(f"Dusklight source tree not found at {root}")

    changed = patch_shadow_source(root)
    changed = patch_shadow_shader(root) or changed
    changed = patch_visual_source(root) or changed
    changed = patch_visual_shader(root) or changed
    print("BOTW texture-safe and sharp-shadow presets ready" + (" (source updated)" if changed else ""))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)

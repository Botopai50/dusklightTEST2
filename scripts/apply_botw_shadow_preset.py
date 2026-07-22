#!/usr/bin/env python3
"""Apply BOTW visual-suite v2.2 and sharp projected-shadow presets.

v2.2 keeps the full visual suite, but fixes the two regressions from the first implementation:

* texture stylization and edge AA are depth/contrast aware instead of filtering every texel;
* material masks are deliberately conservative so water, foliage and metal effects do not recolor
  unrelated surfaces;
* projected shadows use a real nearest-depth comparison when soft filtering is disabled and a
  tighter light frustum so map resolution translates into visible silhouette definition.

The transforms are deterministic and run against the pristine in-tree mod sources during CMake
configure. New config keys avoid reusing aggressive values saved by v2/v2.1 builds.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

SHADOW_SOURCE_MARKER = "DUSKLIGHT_SHARP_SHADOW_V22"
SHADOW_SHADER_MARKER = "DUSKLIGHT_SHARP_SHADOW_SAMPLE_V22"
SHADOW_COLOR_MARKER = "DUSKLIGHT_BOTW_COLORED_SHADOW_V22"
VISUAL_SOURCE_MARKER = "DUSKLIGHT_TEXTURE_SAFE_SOURCE_V22"
VISUAL_SHADER_MARKER = "DUSKLIGHT_TEXTURE_SAFE_SHADER_V22"
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
        print(f"BOTW v2.2 projected-shadow source already applied: {path}")
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
    new = '''    // DUSKLIGHT_SHARP_SHADOW_V22
    // DUSKLIGHT_BOTW_SHADOW_PRESET (legacy workflow marker)
    // Definition depends on texel density and filtering, not only raw map resolution. Windows uses
    // 4096 and Android 2048, while a tighter default coverage keeps character silhouettes readable.
#if defined(__ANDROID__)
    constexpr int64_t botwMapSize = 1; // 2048
    constexpr int64_t botwCoverage = 2600;
#else
    constexpr int64_t botwMapSize = 2; // 4096
    constexpr int64_t botwCoverage = 3000;
#endif
    result = register_bool_option("effectEnabled", true, g_cvarEnabled, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("mapSizeSharpV22", botwMapSize, g_cvarMapSize, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_bool_option("noFrustumClippingSharpV22", false, g_cvarNoFrustumClipping, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("strengthSharpV22", 42, g_cvarStrength, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("pcfSharpV22", 0, g_cvarPcf, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("biasSharpV22", 12, g_cvarBias, error);
'''
    text = replace_once(text, old, new, "shadow defaults")
    text = replace_all_required(
        text,
        [
            (
                '    result = register_int_option("boxRadius", 6000, g_cvarBoxRadius, error);',
                '    result = register_int_option("boxRadiusSharpV22", botwCoverage, g_cvarBoxRadius, error);',
                "shadow coverage default",
            ),
            (
                '    result = register_int_option("edgeFadeWidth", 32, g_cvarEdgeFadeWidth, error);',
                '    result = register_int_option("edgeFadeWidthSharpV22", 6, g_cvarEdgeFadeWidth, error);',
                "shadow edge-fade default",
            ),
            (
                '    result = register_bool_option("contactShadows", true, g_cvarContactShadows, error);',
                '    result = register_bool_option("contactShadowsSharpV22", false, g_cvarContactShadows, error);',
                "contact-shadow default",
            ),
        ],
    )
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Applied BOTW v2.2 projected-shadow source: {path}")
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
        new = '''// DUSKLIGHT_SHARP_SHADOW_SAMPLE_V22
fn sample_shadow_pcf(light_uv: vec2f, receiver: f32) -> f32 {
    let radius = i32(uniforms.pcf_taps);

    // A hard shadow must not call the bilinear comparison path. Snap to the texel center and run
    // one exact depth test. PCF is used only when the user explicitly selects 3x3 or 5x5.
    if radius <= 0i {
        let texel_f = floor(light_uv * uniforms.size);
        let texel = vec2<i32>(texel_f);
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
    return sum / max(count, 1.0);
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

    // DUSKLIGHT_BOTW_COLORED_SHADOW_V22
    // DUSKLIGHT_BOTW_COLORED_SHADOW (legacy workflow marker)
    // A restrained cool multiply preserves silhouette contrast without producing black shadows.
    let amount = uniforms.strength * occlusion;
    let colored_factor = vec3f(
        1.0 - amount * 1.03,
        1.0 - amount * 0.99,
        1.0 - amount * 0.93);
    return vec4f(clamp(colored_factor, vec3f(0.0), vec3f(1.0)), 1.0);
'''
        text = replace_once(text, old, new, "shadow composite")
        changed = True

    if changed:
        path.write_text(text, encoding="utf-8", newline="\n")
        print(f"Applied BOTW v2.2 projected-shadow shader: {path}")
    else:
        print(f"BOTW v2.2 projected-shadow shader already applied: {path}")
    return changed


def patch_visual_source(root: Path) -> bool:
    path = root / "mods/botw_visual_mod/src/mod.cpp"
    text = path.read_text(encoding="utf-8")
    if VISUAL_SOURCE_MARKER in text:
        print(f"BOTW v2.2 visual source already applied: {path}")
        return False

    text = replace_once(
        text,
        '// Breath of the Wild-inspired visual suite v2.\n',
        '// Breath of the Wild-inspired visual suite v2.2.\n'
        '// DUSKLIGHT_TEXTURE_SAFE_SOURCE_V22\n',
        "visual source header",
    )

    replacements = [
        ('register_int("styleIntensity", 82', 'register_int("styleIntensityV22", 58', "style key"),
        ('register_int("shadowLift", 28', 'register_int("shadowLiftV22", 16', "shadow lift key"),
        ('register_int("exposure", 5', 'register_int("exposureV22", 1', "exposure key"),
        ('register_int("contrast", 105', 'register_int("contrastV22", 103', "contrast key"),
        ('register_int("vibrance", 112', 'register_int("vibranceV22", 107', "vibrance key"),
        ('register_int("textureStyle", 22', 'register_int("textureStyleV22", 7', "texture key"),
        ('register_int("fogStrength", 30', 'register_int("fogStrengthV22", 20', "fog key"),
        ('register_int("bloomStrength", 18', 'register_int("bloomStrengthV22", 10', "bloom key"),
        ('register_int("waterStrength", 28', 'register_int("waterStrengthV22", 13', "water key"),
        ('register_int("contactAo", 18', 'register_int("contactAoV22", 10', "AO key"),
        ('register_int("foliageTransmission", 38', 'register_int("foliageV22", 18', "foliage key"),
        ('register_int("wetnessStrength", 45', 'register_int("wetnessV22", 22', "wetness key"),
        ('register_int("edgeAaStrength", 35', 'register_int("edgeAaV22", 18', "edge-AA key"),
        ('register_int("sunRayStrength", 18', 'register_int("sunRaysV22", 9', "sun-ray key"),
        ('register_int("dofStrength", 18', 'register_int("dofV22", 10', "DOF key"),
        ('register_int("skyStrength", 60', 'register_int("skyV22", 34', "sky key"),
    ]
    text = replace_all_required(text, replacements)

    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Applied BOTW v2.2 visual source defaults: {path}")
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
        old_classify = '''fn classify_material(color: vec3f, normal: vec3f, geometry: f32) -> MaterialMasks {
    let luma = luminance(color);
    let sat = saturation_range(color);
    let green_bias = color.g - max(color.r, color.b) * 0.86;
    let warm_bias = color.r - color.b;
    let cyan_bias = min(color.g, color.b) - color.r * 0.72;

    var masks: MaterialMasks;
    masks.foliage = smoothstep(0.015, 0.20, green_bias) *
        smoothstep(0.04, 0.88, luma) * geometry;
    masks.skin = smoothstep(0.035, 0.22, warm_bias) *
        smoothstep(0.16, 0.78, luma) *
        (1.0 - smoothstep(0.48, 0.88, sat)) * geometry;
    masks.metal = smoothstep(0.46, 0.90, luma) *
        (1.0 - smoothstep(0.18, 0.55, sat)) * geometry;
    masks.water = smoothstep(0.025, 0.24, cyan_bias) *
        smoothstep(0.28, 0.86, abs(normal.y)) * geometry;
    return masks;
}
'''
        new_classify = '''// DUSKLIGHT_TEXTURE_SAFE_SHADER_V22
fn classify_material(color: vec3f, normal: vec3f, geometry: f32) -> MaterialMasks {
    let luma = luminance(color);
    let sat = saturation_range(color);
    let green_bias = color.g - max(color.r, color.b) * 0.92;
    let warm_bias = color.r - color.b;
    let cyan_bias = min(color.g, color.b) - color.r * 0.88;
    let horizontal = smoothstep(0.70, 0.94, abs(normal.y));

    var masks: MaterialMasks;
    masks.foliage = smoothstep(0.075, 0.22, green_bias) *
        smoothstep(0.14, 0.42, sat) *
        smoothstep(0.05, 0.72, luma) * geometry;
    masks.skin = smoothstep(0.10, 0.26, warm_bias) *
        smoothstep(0.20, 0.70, luma) *
        (1.0 - smoothstep(0.34, 0.60, sat)) * geometry;
    masks.metal = smoothstep(0.62, 0.91, luma) *
        (1.0 - smoothstep(0.10, 0.28, sat)) * geometry;
    masks.water = smoothstep(0.085, 0.24, cyan_bias) *
        smoothstep(0.12, 0.38, sat) *
        (1.0 - smoothstep(0.70, 0.92, luma)) * horizontal * geometry;
    return masks;
}
'''
        text = replace_once(text, old_classify, new_classify, "conservative material masks")

        old_edge = '''fn edge_aa(coord: vec2i, center: vec3f) -> vec3f {
    if u.params3.z <= 0.001 {
        return center;
    }
    let north = load_color(coord + vec2i(0, -1));
    let south = load_color(coord + vec2i(0, 1));
    let east = load_color(coord + vec2i(1, 0));
    let west = load_color(coord + vec2i(-1, 0));
    let center_luma = luminance(center);
    let min_luma = min(center_luma, min(min(luminance(north), luminance(south)),
        min(luminance(east), luminance(west))));
    let max_luma = max(center_luma, max(max(luminance(north), luminance(south)),
        max(luminance(east), luminance(west))));
    let contrast = max_luma - min_luma;
    let edge = smoothstep(0.035, 0.18, contrast);
    let average = (north + south + east + west) * 0.25;
    let strength = u.params3.z * mix(0.55, 0.85, f32(u.flags.z) * 0.5);
    return mix(center, average, edge * strength);
}
'''
        new_edge = '''fn edge_aa(coord: vec2i, center: vec3f) -> vec3f {
    if u.params3.z <= 0.001 {
        return center;
    }

    let center_depth = depth_to_far(load_depth(coord));
    let offsets = array<vec2i, 4>(
        vec2i(0, -1), vec2i(0, 1), vec2i(1, 0), vec2i(-1, 0)
    );
    var weighted = center * 2.0;
    var weight = 2.0;
    var max_depth_delta = 0.0;
    for (var i = 0u; i < 4u; i += 1u) {
        let sample_coord = coord + offsets[i];
        let delta = abs(depth_to_far(load_depth(sample_coord)) - center_depth);
        max_depth_delta = max(max_depth_delta, delta);
        let same_surface = 1.0 - smoothstep(0.00035, 0.0045, delta);
        weighted += load_color(sample_coord) * same_surface;
        weight += same_surface;
    }

    let geometric_edge = smoothstep(0.00045, 0.006, max_depth_delta);
    let filtered = weighted / max(weight, 0.001);
    return mix(center, filtered, geometric_edge * u.params3.z * 0.32);
}
'''
        text = replace_once(text, old_edge, new_edge, "depth-aware edge AA")

        old_stylize = '''fn stylize_texture(coord: vec2i, color: vec3f, geometry: f32) -> vec3f {
    let amount = u.params3.y * geometry;
    if amount <= 0.001 {
        return color;
    }
    let center_luma = luminance(color);
    var weighted = color * 2.0;
    var weight = 2.0;
    let offsets = array<vec2i, 4>(
        vec2i(2, 0), vec2i(-2, 0), vec2i(0, 2), vec2i(0, -2)
    );
    for (var i = 0u; i < 4u; i += 1u) {
        let sample_color = load_color(coord + offsets[i]);
        let similarity = 1.0 - smoothstep(0.025, 0.24,
            abs(luminance(sample_color) - center_luma));
        weighted += sample_color * similarity;
        weight += similarity;
    }
    let painted = weighted / max(weight, 0.001);
    let levels = mix(48.0, 18.0, amount);
    let grouped = floor(max(painted, vec3f(0.0)) * levels + vec3f(0.5)) / levels;
    return mix(color, grouped, amount * 0.62);
}
'''
        new_stylize = '''fn stylize_texture(coord: vec2i, color: vec3f, geometry: f32) -> vec3f {
    let amount = u.params3.y * geometry;
    if amount <= 0.001 {
        return color;
    }

    let center_luma = luminance(color);
    let center_depth = depth_to_far(load_depth(coord));
    var weighted = color * 3.0;
    var weight = 3.0;
    var local_detail = 0.0;
    let offsets = array<vec2i, 4>(
        vec2i(1, 0), vec2i(-1, 0), vec2i(0, 1), vec2i(0, -1)
    );
    for (var i = 0u; i < 4u; i += 1u) {
        let sample_coord = coord + offsets[i];
        let sample_color = load_color(sample_coord);
        let luma_delta = abs(luminance(sample_color) - center_luma);
        let depth_delta = abs(depth_to_far(load_depth(sample_coord)) - center_depth);
        local_detail = max(local_detail, luma_delta);
        let color_similarity = 1.0 - smoothstep(0.018, 0.095, luma_delta);
        let same_surface = 1.0 - smoothstep(0.0003, 0.004, depth_delta);
        let sample_weight = color_similarity * same_surface;
        weighted += sample_color * sample_weight;
        weight += sample_weight;
    }

    let low_detail = 1.0 - smoothstep(0.020, 0.085, local_detail);
    let painted = weighted / max(weight, 0.001);
    return mix(color, painted, amount * low_detail * 0.28);
}
'''
        text = replace_once(text, old_stylize, new_stylize, "edge-preserving texture stylization")
        text = replace_once(
            text,
            '    let rain_wetness = u.env1.z * u.effects.y;',
            '    let rain_wetness = smoothstep(0.10, 0.45, u.env1.z) * u.effects.y;',
            "rain-only wetness",
        )
        text = replace_once(
            text,
            '    let not_absorbent = 1.0 - masks.foliage * 0.45 - masks.skin * 0.70;',
            '    let not_absorbent = clamp(1.0 - masks.foliage * 0.85 - masks.skin * 0.92, 0.0, 1.0);',
            "wetness material exclusion",
        )
        text = replace_once(
            text,
            '    let wet = clamp(rain_wetness * exposed * not_absorbent, 0.0, 1.0);',
            '    let wet = clamp(rain_wetness * exposed * not_absorbent, 0.0, 0.72);',
            "wetness cap",
        )
        changed = True

    if changed:
        path.write_text(text, encoding="utf-8", newline="\n")
        print(f"Applied BOTW v2.2 texture-safe visual shader: {path}")
    else:
        print(f"BOTW v2.2 visual shader already applied: {path}")
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
    print("BOTW visual-suite v2.2 presets ready" + (" (source updated)" if changed else ""))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)

#!/usr/bin/env python3
"""Apply build-time BOTW presets and WGSL portability fixes.

The upstream shadow-map implementation is retained. This transform changes only stable defaults,
the final shadow multiply color, and two explicit WGSL expressions that stricter mobile validators
require to be fully typed.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

SOURCE_MARKER = "DUSKLIGHT_BOTW_SHADOW_PRESET"
SHADER_MARKER = "DUSKLIGHT_BOTW_COLORED_SHADOW"
VISUAL_MARKER = "DUSKLIGHT_BOTW_WGSL_PORTABILITY"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise RuntimeError(f"Could not find expected {label} block")
    return text.replace(old, new, 1)


def patch_source(root: Path) -> bool:
    path = root / "mods/shadow_mod/src/mod.cpp"
    text = path.read_text(encoding="utf-8")
    if SOURCE_MARKER in text:
        print(f"BOTW projected-shadow defaults already applied: {path}")
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
    new = '''    // DUSKLIGHT_BOTW_SHADOW_PRESET
    // A compact projected-shadow preset: enabled by default, moderate coverage, a small PCF
    // kernel and no expensive off-camera caster retention. Users can still override every value.
#if defined(__ANDROID__)
    constexpr int64_t botwMapSize = 0; // 1024
    constexpr bool botwContactShadows = false;
#else
    constexpr int64_t botwMapSize = 0; // 1024; use 2048 manually on stronger GPUs
    constexpr bool botwContactShadows = true;
#endif
    result = register_bool_option("effectEnabled", true, g_cvarEnabled, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("mapSize", botwMapSize, g_cvarMapSize, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_bool_option("noFrustumClipping", false, g_cvarNoFrustumClipping, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("strength", 38, g_cvarStrength, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("pcf", 1, g_cvarPcf, error);
    if (result != MOD_OK) {
        return result;
    }
    result = register_int_option("bias", 35, g_cvarBias, error);
'''
    text = replace_once(text, old, new, "shadow defaults")
    text = replace_once(
        text,
        '    result = register_bool_option("contactShadows", true, g_cvarContactShadows, error);',
        '    result = register_bool_option("contactShadows", botwContactShadows, g_cvarContactShadows, error);',
        "contact-shadow default",
    )
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Applied BOTW projected-shadow defaults: {path}")
    return True


def patch_shadow_shader(root: Path) -> bool:
    path = root / "mods/shadow_mod/res/shadow.wgsl"
    text = path.read_text(encoding="utf-8")
    if SHADER_MARKER in text:
        print(f"BOTW projected-shadow color already applied: {path}")
        return False

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
    // BOTW-like projected shadows are slightly cooler and less blue-channel destructive than a
    // neutral black multiply. Fully lit pixels remain exactly vec3f(1.0).
    let amount = uniforms.strength * occlusion;
    let colored_factor = vec3f(
        1.0 - amount * 1.04,
        1.0 - amount * 0.97,
        1.0 - amount * 0.86);
    return vec4f(clamp(colored_factor, vec3f(0.0), vec3f(1.0)), 1.0);
'''
    text = replace_once(text, old, new, "shadow composite")
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Applied BOTW projected-shadow color: {path}")
    return True


def patch_visual_wgsl(root: Path) -> bool:
    path = root / "mods/botw_visual_mod/res/botw_visual.wgsl"
    text = path.read_text(encoding="utf-8")
    if VISUAL_MARKER in text:
        print(f"BOTW WGSL portability fixes already applied: {path}")
        return False

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
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Applied BOTW WGSL portability fixes: {path}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    if not (root / "mods/shadow_mod/src/mod.cpp").is_file():
        raise RuntimeError(f"Dusklight source tree not found at {root}")

    changed = patch_source(root)
    changed = patch_shadow_shader(root) or changed
    changed = patch_visual_wgsl(root) or changed
    print("BOTW build-time presets ready" + (" (source updated)" if changed else ""))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)

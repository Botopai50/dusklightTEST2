#!/usr/bin/env python3
"""Apply a BOTW-inspired material lighting patch to Aurora.

Aurora remains an untouched Git submodule in the repository. During CMake configure this script
patches the checked-out Aurora worktree deterministically. Re-running it refreshes the generated
shader block, which makes local rebuilds safe when the preset constants are adjusted below.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

GX_MARKER = "DUSKLIGHT_SHIPWRIGHT_CELSHADE_PER_PIXEL"
SHADER_BEGIN = "DUSKLIGHT_SHIPWRIGHT_CELSHADE_BEGIN"
SHADER_END = "DUSKLIGHT_SHIPWRIGHT_CELSHADE_END"

# Centralized defaults. The post-process mod also exposes runtime controls for perceived shadow
# brightness, grading, fog, AO, bloom and water. These constants define the material response.
SHADOW_STRENGTH = 0.55       # 0 = no toon darkening, 1 = ambient-only shadow
SHADOW_TINT_STRENGTH = 0.28  # cool sky / warm ground tint in shadowed regions
WARM_LIGHT_STRENGTH = 0.10   # warms direct light without changing light selection
RIM_STRENGTH = 0.10          # restrained back-light edge response
SPECULAR_STRENGTH = 0.24     # stylized GX specular band contribution
RAMP_LOW = 0.485
RAMP_HIGH = 0.515


def replace_exact(path: Path, old: str, new: str) -> bool:
    text = path.read_text(encoding="utf-8")
    if old not in text:
        raise RuntimeError(
            f"Could not find the expected Aurora source block in {path}. "
            "The Aurora submodule probably changed and the BOTW lighting patch must be rebased."
        )
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"Applied BOTW lighting patch: {path}")
    return True


def patch_gx_header(aurora_dir: Path) -> bool:
    path = aurora_dir / "lib/gx/gx.hpp"
    text = path.read_text(encoding="utf-8")
    if GX_MARKER in text:
        print(f"BOTW per-pixel lighting already enabled: {path}")
        return False

    old = "constexpr bool UsePerPixelLighting = false;"
    new = (
        "// DUSKLIGHT_SHIPWRIGHT_CELSHADE_PER_PIXEL\n"
        "// BOTW-style bands must use the real fragment normal. Vertex lighting would blur the\n"
        "// boundary across large triangles and make characters look inconsistently shaded.\n"
        "constexpr bool UsePerPixelLighting = true;"
    )
    return replace_exact(path, old, new)


def generated_shader_block() -> str:
    return f'''  // {SHADER_BEGIN}
  // BOTW-inspired material lighting. Alpha and unlit channels keep the original GX path.
  // Diffuse channels use a hard half-Lambert band, cool indirect light, warm direct light,
  // lifted shadows and a restrained back-light rim. GX specular channels are quantized into
  // a small colored highlight instead of using an unrestricted glossy response.
  const bool botwDiffuseEligible = !alpha && diffFn != GX_DF_NONE && cc.attnFn != GX_AF_SPEC;
  const bool botwSpecularEligible = !alpha && cc.attnFn == GX_AF_SPEC;
  if (botwDiffuseEligible) {{
    const std::string_view normalVar = UsePerPixelLighting ? "in.mv_nrm"sv : "mv_nrm"sv;
    return fmt::format(R"""(
    {{{{
      let botw_nrm = normalize({{10}});
      let botw_view_dir = normalize(-{{6}}.xyz);
      var botw_ambient = {{5}};
      var botw_light_color = vec4f(0.0);
      var botw_light_dir = vec3f(0.0, 0.0, 1.0);
      var botw_light_strength = -1.0;
      for (var i = 0u; i < {{1}}u; i++) {{{{
          if ((ubuf.lightState{{0}}{{9}} & (1u << i)) == 0u) {{{{ continue; }}}}
          var light = ubuf.lights[i];
          var ldir = light.pos - {{6}};
          var dist2 = dot(ldir, ldir);
          var dist = max(sqrt(dist2), 0.000001);
          ldir = ldir / dist;
          var attn: f32;{{2}}

          let candidate_strength = max(attn, 0.0) *
              dot(light.color.rgb, vec3f(0.2126, 0.7152, 0.0722));
          if (candidate_strength > botw_light_strength) {{{{
              botw_light_strength = candidate_strength;
              botw_light_color = max(attn, 0.0) * light.color;
              botw_light_dir = ldir;
          }}}}
      }}}}

      // Hard two-tone half-Lambert boundary. A tiny transition prevents temporal sparkle.
      let botw_half_lambert = clamp(dot(botw_nrm, botw_light_dir) * 0.5 + 0.5, 0.0, 1.0);
      let botw_ramp = smoothstep({RAMP_LOW:.6f}, {RAMP_HIGH:.6f}, botw_half_lambert);

      // Hemisphere-style indirect color. View-space Y remains aligned to camera-up in normal play,
      // providing a stable cool-sky / warm-ground separation without another texture or light.
      let botw_sky_factor = clamp(botw_nrm.y * 0.5 + 0.5, 0.0, 1.0);
      let botw_ground_tint = vec3f(0.94, 0.88, 0.82);
      let botw_sky_tint = vec3f(0.78, 0.90, 1.10);
      let botw_hemi_tint = mix(botw_ground_tint, botw_sky_tint, botw_sky_factor);
      let botw_tinted_ambient = vec4f(
          botw_ambient.rgb * mix(vec3f(1.0), botw_hemi_tint, {SHADOW_TINT_STRENGTH:.6f}),
          botw_ambient.a);

      let botw_warm_scale = vec3f(
          {1.0 + WARM_LIGHT_STRENGTH:.6f},
          {1.0 + WARM_LIGHT_STRENGTH * 0.35:.6f},
          {1.0 - WARM_LIGHT_STRENGTH * 0.45:.6f});
      let botw_direct = vec4f(botw_light_color.rgb * botw_warm_scale, botw_light_color.a);
      let botw_lit = botw_tinted_ambient + botw_direct;

      // Preserve part of the direct light inside the dark band. This is intentionally brighter
      // than the previous ambient-only shadow and fixes crushed faces and clothing indoors.
      let botw_shadow = botw_tinted_ambient + botw_direct * {1.0 - SHADOW_STRENGTH:.6f};
      var botw_lighting = mix(botw_shadow, botw_lit, botw_ramp);

      // Restrained back-light rim. It is geometric and light-aware, so it does not draw a black
      // outline or add a constant halo around every object.
      let botw_fresnel = pow(clamp(1.0 - max(dot(botw_nrm, botw_view_dir), 0.0), 0.0, 1.0), 3.0);
      let botw_backlight = smoothstep(0.05, 0.75, max(dot(-botw_light_dir, botw_view_dir), 0.0));
      let botw_rim = botw_fresnel * botw_backlight * {RIM_STRENGTH:.6f};
      let botw_rim_color = mix(vec3f(0.42, 0.62, 0.92), botw_direct.rgb, 0.30);
      botw_lighting = vec4f(botw_lighting.rgb + botw_rim_color * botw_rim, botw_lighting.a);

      {{7}}{{8}} = ({{4}} * clamp(botw_lighting, vec4f(0.0), vec4f(1.0))){{8}};
    }}}})""",
                       i, GX::MaxLights, lightAttnFn, lightDiffFn, matSrc, ambSrc, posVar, outVar,
                       swizzle, alpha ? "a"sv : ""sv, normalVar);
  }}

  if (botwSpecularEligible) {{
    return fmt::format(R"""(
    {{{{
      var botw_spec_lighting = {{5}};
      for (var i = 0u; i < {{1}}u; i++) {{{{
          if ((ubuf.lightState{{0}}{{9}} & (1u << i)) == 0u) {{{{ continue; }}}}
          var light = ubuf.lights[i];
          var ldir = light.pos - {{6}};
          var dist2 = dot(ldir, ldir);
          var dist = max(sqrt(dist2), 0.000001);
          ldir = ldir / dist;
          var attn: f32;{{2}}
          var diff = {{3}};
          let botw_spec_signal = max(attn * max(diff, 0.0), 0.0);
          let botw_spec_band = smoothstep(0.42, 0.72, botw_spec_signal);
          let botw_spec_color = light.color * vec4f(1.08, 1.03, 0.92, 1.0);
          botw_spec_lighting = botw_spec_lighting +
              botw_spec_band * botw_spec_color * {SPECULAR_STRENGTH:.6f};
      }}}}
      {{7}}{{8}} = ({{4}} * clamp(botw_spec_lighting, vec4f(0.0), vec4f(1.0))){{8}};
    }}}})""",
                       i, GX::MaxLights, lightAttnFn, lightDiffFn, matSrc, ambSrc, posVar, outVar,
                       swizzle, alpha ? "a"sv : ""sv);
  }}
  // {SHADER_END}
'''


def patch_shader_generator(aurora_dir: Path) -> bool:
    path = aurora_dir / "lib/gx/shader.cpp"
    text = path.read_text(encoding="utf-8")
    block = generated_shader_block()

    if SHADER_BEGIN in text:
        pattern = re.compile(
            rf"  // {re.escape(SHADER_BEGIN)}.*?  // {re.escape(SHADER_END)}\n",
            re.DOTALL,
        )
        updated, count = pattern.subn(block, text, count=1)
        if count != 1:
            raise RuntimeError(f"Could not refresh the existing BOTW shader block in {path}")
        path.write_text(updated, encoding="utf-8", newline="\n")
        print(f"Refreshed BOTW lighting patch: {path}")
        return True

    anchor = '''  return fmt::format(R"""(
    {{
      var lighting = {5};
      for (var i = 0u; i < {1}u; i++) {{
          if ((ubuf.lightState{0}{9} & (1u << i)) == 0u) {{ continue; }}
          var light = ubuf.lights[i];
          var ldir = light.pos - {6};
          var dist2 = dot(ldir, ldir);
          var dist = sqrt(dist2);
          ldir = ldir / dist;
          var attn: f32;{2}
          var diff = {3};
          lighting = lighting + (attn * diff * light.color);
      }}
      {7}{8} = ({4} * clamp(lighting, vec4f(0.0), vec4f(1.0))){8};
    }})""",
                     i, GX::MaxLights, lightAttnFn, lightDiffFn, matSrc, ambSrc, posVar, outVar, swizzle,
                     alpha ? "a"sv : ""sv);
'''
    if anchor not in text:
        raise RuntimeError(
            f"Could not find the expected Aurora lighting return block in {path}. "
            "The Aurora submodule probably changed and the BOTW lighting patch must be rebased."
        )
    path.write_text(text.replace(anchor, block + "\n" + anchor, 1), encoding="utf-8", newline="\n")
    print(f"Applied BOTW lighting patch: {path}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--aurora-dir", type=Path, required=True)
    args = parser.parse_args()

    aurora_dir = args.aurora_dir.resolve()
    if not (aurora_dir / "lib/gx/shader.cpp").is_file():
        raise RuntimeError(f"Aurora source tree not found at {aurora_dir}")

    changed = patch_gx_header(aurora_dir)
    changed = patch_shader_generator(aurora_dir) or changed
    print("BOTW visual lighting patch ready" + (" (source updated)" if changed else ""))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)

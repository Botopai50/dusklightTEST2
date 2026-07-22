#!/usr/bin/env python3
"""Apply the Ship of Harkinian-style toon-lighting patch to Aurora.

Aurora is kept as a Git submodule, so the patch is applied during CMake configure instead of
requiring a separate Aurora fork. The operation is deterministic and idempotent: a clean checkout
is patched once, while an already-patched worktree is left unchanged.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

GX_MARKER = "DUSKLIGHT_SHIPWRIGHT_CELSHADE_PER_PIXEL"
SHADER_MARKER = "DUSKLIGHT_SHIPWRIGHT_CELSHADE_BEGIN"


def replace_exact(path: Path, old: str, new: str, marker: str) -> bool:
    text = path.read_text(encoding="utf-8")
    if marker in text:
        print(f"Shipwright celshade already applied: {path}")
        return False
    if old not in text:
        raise RuntimeError(
            f"Could not find the expected Aurora source block in {path}. "
            "The Aurora submodule probably changed and the celshade patch must be rebased."
        )
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"Applied Shipwright celshade patch: {path}")
    return True


def patch_gx_header(aurora_dir: Path) -> bool:
    path = aurora_dir / "lib/gx/gx.hpp"
    old = "constexpr bool UsePerPixelLighting = false;"
    new = (
        "// DUSKLIGHT_SHIPWRIGHT_CELSHADE_PER_PIXEL\n"
        "// Ship of Harkinian evaluates its toon ramp from the real fragment normal. Aurora's\n"
        "// default vertex lighting would interpolate already-lit colors and blur the band boundary.\n"
        "constexpr bool UsePerPixelLighting = true;"
    )
    return replace_exact(path, old, new, GX_MARKER)


def patch_shader_generator(aurora_dir: Path) -> bool:
    path = aurora_dir / "lib/gx/shader.cpp"
    old = '''  return fmt::format(R"""(
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
    new = '''  // DUSKLIGHT_SHIPWRIGHT_CELSHADE_BEGIN
  // Match the Ship of Harkinian renderer: only diffuse RGB lighting is replaced. Alpha,
  // unlit channels and specular attenuation keep the original GX calculation so cutouts,
  // particles, water and special TEV materials retain their intended behavior.
  const bool shipwrightToonEligible = !alpha && diffFn != GX_DF_NONE && cc.attnFn != GX_AF_SPEC;
  if (shipwrightToonEligible) {
    const std::string_view normalVar = UsePerPixelLighting ? "in.mv_nrm"sv : "mv_nrm"sv;
    return fmt::format(R"""(
    {{
      let toon_nrm = normalize({10});
      var toon_ambient = {5};
      var toon_light_color = vec4f(0.0);
      var toon_light_dir = vec3f(0.0, 0.0, 1.0);
      var toon_light_strength = -1.0;
      for (var i = 0u; i < {1}u; i++) {{
          if ((ubuf.lightState{0}{9} & (1u << i)) == 0u) {{ continue; }}
          var light = ubuf.lights[i];
          var ldir = light.pos - {6};
          var dist2 = dot(ldir, ldir);
          var dist = max(sqrt(dist2), 0.000001);
          ldir = ldir / dist;
          var attn: f32;{2}

          // Shipwright uses one dominant light plus ambient. Select the strongest enabled
          // GX light independent of surface facing, then form the band from its true N dot L.
          let candidate_strength = max(attn, 0.0) *
              dot(light.color.rgb, vec3f(0.2126, 0.7152, 0.0722));
          if (candidate_strength > toon_light_strength) {{
              toon_light_strength = candidate_strength;
              toon_light_color = max(attn, 0.0) * light.color;
              toon_light_dir = ldir;
          }}
      }}

      // Hard two-tone half-Lambert ramp. The narrow 0.485..0.515 transition keeps the
      // shadow edge stable under animation while making it substantially sharper than before.
      let toon_half_lambert = clamp(dot(toon_nrm, toon_light_dir) * 0.5 + 0.5, 0.0, 1.0);
      let toon_ramp = smoothstep(0.485, 0.515, toon_half_lambert);
      let toon_lit = toon_ambient + toon_light_color;
      let toon_shadow = toon_ambient;
      let toon_lighting = mix(toon_shadow, toon_lit, toon_ramp);
      {7}{8} = ({4} * clamp(toon_lighting, vec4f(0.0), vec4f(1.0))){8};
    }})""",
                       i, GX::MaxLights, lightAttnFn, lightDiffFn, matSrc, ambSrc, posVar, outVar,
                       swizzle, alpha ? "a"sv : ""sv, normalVar);
  }
  // DUSKLIGHT_SHIPWRIGHT_CELSHADE_END

  return fmt::format(R"""(
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
    return replace_exact(path, old, new, SHADER_MARKER)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--aurora-dir", type=Path, required=True)
    args = parser.parse_args()

    aurora_dir = args.aurora_dir.resolve()
    if not (aurora_dir / "lib/gx/shader.cpp").is_file():
        raise RuntimeError(f"Aurora source tree not found at {aurora_dir}")

    changed = patch_gx_header(aurora_dir)
    changed = patch_shader_generator(aurora_dir) or changed
    print("Shipwright celshade patch ready" + (" (source updated)" if changed else ""))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # CI needs one concise, actionable failure.
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)

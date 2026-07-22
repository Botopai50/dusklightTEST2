# Cel Shading Mod

A lightweight WebGPU post-process for Dusklight that stylizes opaque world geometry without modifying Aurora's GX/TEV shader generator.

## Features

- Three configurable lighting bands
- Depth and reconstructed-normal outlines
- Adjustable outline thickness from 1 to 3 pixels
- Cool rim lighting
- Saturation and contrast controls
- Light-direction presets
- Debug views for toon bands, normals, depth and outlines

## Building with Dusklight

Configure the main project with code mods enabled:

```sh
cmake -S . -B build -DDUSK_ENABLE_CODE_MODS=ON
cmake --build build --target dusklight_mods --config Release
```

The packaged mod is written to:

```text
build/mods/celshade_mod.dusk
```

It is also registered as a bundled in-tree mod for normal Dusklight packaging.

## Standalone mod build

```sh
cmake -S mods/celshade_mod -B build-celshade
cmake --build build-celshade --config Release
```

## Recommended starting preset

- Toon Strength: 85%
- Shadow Level: 62%
- Middle Level: 86%
- Highlight Level: 108%
- Band Smoothing: 3%
- Outline Strength: 82%
- Outline Thickness: 1 px
- Depth Threshold: 16
- Normal Threshold: 22%
- Rim Light: 18%
- Saturation: 110%
- Contrast: 105%

The effect runs at `GFX_STAGE_SCENE_AFTER_OPAQUE`, so HUD elements and most translucent effects are left untouched.

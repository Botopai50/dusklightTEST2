# Breath of the Wild Visual Suite v2.2

v2.2 keeps the complete BOTW-inspired feature set and fixes the two regressions reported in v2:
texture distortion and a projected shadow that stayed soft even at high map resolutions.

## Effects retained

- renderer-integrated hard two-tone cel shading;
- cool indirect light, warm direct light and lifted shadows;
- stylized GX specular response and restrained rim light;
- dynamic time-of-day, rain, clouds, twilight and environment-aware grading;
- analytic sky and horizon blended only over depth-confirmed sky pixels;
- depth atmosphere and fog;
- selective bloom and contact AO;
- foliage back-light transmission;
- rain-driven wet surfaces;
- stylized water tint, shimmer, Fresnel and shoreline emphasis;
- radial sun shafts with scene-depth occlusion;
- selective depth of field only when the game requests blur;
- texture stylization and edge anti-aliasing;
- projected dynamic shadows.

## Texture-safe corrections

The systems above were corrected instead of disabled:

- texture stylization is now a small bilateral cleanup applied only to low-detail regions;
- posterization and color quantization were removed;
- high-contrast texture patterns, faces and text are excluded automatically;
- edge AA uses scene depth and never averages foreground with background;
- foliage, skin, metal and water masks use narrower color, saturation, luminance, normal and depth
  conditions;
- rain wetness starts only after real rain accumulates and strongly excludes skin and foliage;
- all effects use new v2.2 configuration keys, so aggressive values saved by older builds are ignored.

Defaults are intentionally moderate, but every system remains adjustable from the in-game controls.

## Sharp projected shadows

The projected-shadow preset uses:

- 4096 shadow map on Windows and 2048 on Android;
- 3000-unit coverage on Windows and 2600 on Android;
- 12-unit default bias;
- 6-texel edge fade;
- no PCF by default;
- true nearest-depth comparison when `Soft Shadows` is set to `Off`;
- optional 3x3 or 5x5 filtering only when explicitly selected.

Map resolution alone did not fix the previous shadow because the `Off` path still performed a hidden
bilinear comparison. v2.2 removes that filtering from the hard-shadow path and increases effective
texel density by tightening the light-camera coverage.

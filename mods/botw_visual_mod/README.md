# Breath of the Wild Visual Suite v2.3

v2.3 keeps the complete BOTW-inspired feature set while replacing the expensive default shadow-map
path with a lightweight directional screen-space shadow.

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
- texture-safe bilateral stylization and depth-aware edge anti-aliasing.

## Lightweight directional shadow

The default shadow is now integrated into the BOTW visual pass that already reads scene depth. It:

- performs no second scene render;
- allocates no shadow map;
- uses 5 depth samples on Low/Mobile, 7 on Medium and 9 on High;
- fades with distance and exits early when it finds an occluder;
- follows the real sun/moon direction;
- preserves the original Twilight Princess actor/blob shadows;
- is intended to work through Dawn on Vulkan, D3D12 and OpenGL ES backends.

Controls:

- `Directional Shadow`: strength of the lightweight shadow;
- `Shadow Reach`: screen-space ray length toward the sun. Lower values are cheaper and more stable.

Defaults are 30% strength and 260 units of reach.

## Optional full shadow map

The old scene-replay shadow map remains available as a separate high-end option, but is disabled by
default. It is intended mainly for Vulkan or strong desktop GPUs.

The optional path was reduced to:

- actor/object caster lists only, without replaying terrain lists;
- 2048 on Windows and 1024 on Android;
- 2600-unit coverage on Windows and 2200 on Android;
- depth-only snapshot during normal use;
- no PCF by default;
- original game shadows disabled only while this optional full-map mode is active.

This substantially reduces its cost, but it remains more expensive and less portable than the
default screen-space path.

## Texture-safe corrections retained from v2.2

- texture stylization is a small bilateral cleanup applied only to low-detail regions;
- posterization and color quantization are removed;
- high-contrast texture patterns, faces and text are excluded automatically;
- edge AA uses scene depth and does not average foreground with background;
- foliage, skin, metal and water masks use narrow color, saturation, luminance, normal and depth
  conditions;
- rain wetness starts only after real rain accumulates and strongly excludes skin and foliage.

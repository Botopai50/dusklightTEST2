# Breath of the Wild Visual Suite v2

This bundled mod complements the renderer-integrated cel shading with effects that belong after
scene rendering. It uses the game's real environment state instead of a fixed color preset.

## Implemented systems

- Dynamic grading driven by Twilight Princess time of day, rain, clouds and twilight state.
- Analytic sky, horizon, sun/moon glow and lightweight procedural clouds.
- Depth atmosphere using the game's current fog and sky colors.
- Approximate screen-space material classification for foliage, skin, metal and water.
- Warm foliage transmission, readable skin shadows and compact colored metal highlights.
- Persistent rain wetness with darkened albedo and sky-colored Fresnel/specular response.
- Stylized water tint, shimmer, Fresnel and shoreline emphasis.
- Texture-detail grouping for a cleaner painted look without replacing original assets.
- Lightweight edge anti-aliasing and hard-band stability.
- Selective bloom, contact AO, sun shafts and game-triggered depth of field.
- Low, Medium and High quality modes plus adaptive resolution/device sample caps.
- Debug views for material masks, normals, fog, wetness, foliage, sky, rays and DOF.

The material shader remains responsible for the hard two-tone light boundary, cool indirect light,
warm direct light, stylized GX specular response, restrained rim light and material-aware shadow
retention. The existing shadow-map mod is configured at build time with a lighter BOTW preset and a
cool blue-gray projected-shadow multiplier.

Android defaults to the Low/Mobile quality tier. Windows defaults to Medium. Every visible effect
can be adjusted or disabled from the mod controls without rebuilding the game.

# Breath of the Wild Visual Suite v2.1

This revision keeps the renderer-integrated toon lighting while restoring the original Twilight
Princess texture texels. The first v2 preset was too aggressive because it applied full-screen
neighbor averaging, posterization and color-based material guesses to the whole scene.

## Texture-safe rendering

The v2.1 preset permanently disables the unsafe full-screen effects:

- texture posterization and painted-detail grouping;
- neighbor-based edge blur over texture details;
- color-based foliage, skin, metal and water recoloring;
- global rain wetness material guessing;
- analytic sky replacement over the original skybox;
- gameplay depth of field and radial sun-ray blur.

The stable effects remain available with conservative defaults:

- renderer-integrated hard two-tone cel shading;
- cool indirect light and warm direct light;
- restrained rim and stylized GX specular response;
- dynamic environment-aware color grading;
- subtle depth fog;
- low-strength selective bloom;
- low-strength contact AO.

New configuration keys reset any aggressive values saved by v2. Original game textures are sampled
directly and are no longer averaged or quantized by the finishing pass.

## Sharp projected shadows

The projected-shadow preset now uses:

- 4096 shadow map on Windows and 2048 on Android;
- a tighter 3500-unit coverage radius;
- 15-unit default bias;
- 8-texel edge fade;
- no PCF by default;
- true nearest-depth comparison when `Soft Shadows` is set to `Off`;
- optional 3x3 or 5x5 filtering only when explicitly selected.

Increasing map resolution alone did not fix v2 because even the `Off` option still performed a
bilinear four-comparison filter. That hidden filter has been removed from the hard-shadow path.

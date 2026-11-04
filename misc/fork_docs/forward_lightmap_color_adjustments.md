# LightmapGI adjustments through a 3D LUT

Grading depends only on the L0 color (the tonal axis uses the lightmap's baked luminance range, not the texel position), so it is baked into a 64x64x64 RGBA16F 3D LUT (2 MiB per lightmap) instead of into a copy of the lightmap. The original, compressed lightmap stays bound; Forward+ and Mobile sample L0 as usual and grade it with one trilinear LUT fetch (`lightmap_apply_color_lut()`). Directional coefficients are untouched, as they are stored relative to L0.

The LUT is indexed per channel by log2 color, covering the luminance range from 2 stops below to 2 stops above (at least 8 stops), and stores log2 of the graded color so interpolation is multiplicative. Outside that range the edge grading continues with slope 1 in stops, so hot texels are neither clipped nor flattened.

The compute pass (`lightmap_color_cache.glsl`) regenerates only the 262k LUT cells when adjustments, the luminance range or the lightmap textures change; edits cost the same regardless of lightmap size. Stable frames do no work. Disabled or neutral adjustments skip the LUT through flag bit 9 of the lightmap record (`LIGHTMAP_FLAG_COLOR_LUT`), whose `color_lut_params` hold the LUT mapping. LUTs are bound at set 1 binding 40 (Forward+) and 26 (Mobile).

Grading happens after filtering, so strongly nonlinear edits can differ slightly from grading each texel. Previously a full RGBA16F copy of every lightmap was generated (8 bytes per texel, all layers), which for large lightmaps exceeded VRAM and made both rendering and edits slow.

Tonal slider values now map linearly from the original L0 minimum luminance (0) to maximum luminance (1), across all L0 layers at base resolution. The range is computed when baking and stored in `LightmapGIData`, so no texture is read back at runtime. Lightmaps baked before this was stored are measured once in the editor, when adjustments first become active: a compute pass point-samples each L0 layer on a 512x512 grid and reads back only those samples. `LightmapGI` stores the result in its `LightmapGIData` (via `RenderingServer::lightmap_get_tonal_luminance_range()`) and marks it edited, so saving the scene keeps it. Games never read the texture back: without a stored range they assume 12 stops below 1.0 and warn once. Uniform textures map to level 0. The grading and the debug mask use this same range, before global saturation. Existing scenes retain their slider values with the new range interpretation. Editor debug masks bypass material shading, directional modulation and fog, and cancel camera and auto exposure like editor gizmos; tonemapping and screen effects still apply.

The tonal debug view also goes through the LUT: in debug mode the LUT holds the mask instead of the grading (flag bit 8, `LIGHTMAP_FLAG_COLOR_PREVIEW`), sampled without extrapolation. Only the editor's Forward+ shader displays it (`LIGHTMAP_COLOR_PREVIEW`, compiled when `Engine::is_editor_hint()`), since showing it keeps a value alive through the whole fragment shader; elsewhere debug mode shows the lightmap without adjustments.

## Levels per tonal range

Lightmap Shadows, Midtones, and Highlights provide Black Point (0), White Point (1), and Gamma (1). Raise Black Point to deepen the darkest values, lower White Point to strengthen the bright values, and raise Gamma to brighten intermediate values. Black and white are normalized against the maximum luminance of the original L0 texture. The original texture also determines the masks, so adjustments do not move their own tonal selection. Levels scale RGB by the luminance change and retain HDR values above white. They apply after tonal exposure and contrast, before global tint and energy, in the LUT.

Environment Color Correction adds Black Point and White Point to Master, Shadows, Midtones, and Highlights and reuses their Gamma controls. Levels use luminance in the correction working space: scene-linear HDR in Forward+, and the existing post-tonemap correction space in Mobile and Compatibility. Master levels apply first, then the levels blended with the original pre-tonemap tonal masks, then existing lift/gamma/gain. Neutral points skip the remap and preserve existing scenes.

## Range and softness

`tonal_range_bottom` and `tonal_range_top` add stops to the baked 1st and 99th percentiles that place the tonal ranges, levels axis and contrast pivot; in dark scenes with a few hot lamps, lower the top. `tonal_softness` widens both transitions by stops, so narrowing the range doesn't sharpen them. No rebake is needed.

`LightmapGI` sends its controls as one `LightmapAdjustments` through `RenderingServer::lightmap_set_color_adjustments()` (C++ only, not bound); the baked luminance range still arrives separately from `LightmapGIData`.

## Lower and Upper per range

Each range has Lower and Upper in EV (-4..4). Inside a range, the pixel's position on the log axis goes from 0 at the range's dark end to 1 at its bright end (shadows start at the range bottom, highlights end at its top, midtones span both transitions, all widened by softness). Lower adds `weight * (1 - position)` of its stops and Upper `weight * position`, together with the range's Exposure, so only the darker or brighter part of the range moves and nothing clips. The debug view tints the range blue where Lower acts and orange where Upper acts. The grading record has 16 rows (rows 14 and 15 hold Lower and Upper); it only feeds the LUT generator, so the lightmap record stays 80 bytes.

# Forward+ Environment adjustments

All modules require the global `adjustment_enabled` switch. Subgroup toggles
remain independent; disabled groups retain serialized settings.

| Group | Before tonemapping | After tonemapping |
| --- | --- | --- |
| Basic controls | Brightness, contrast (affine or soft), saturation | — |
| White Balance | Per-channel black/white levels | — |
| Color Grading | Temperature/tint, vibrance, vignette | — |
| Color Correction | Master and weighted shadow/midtone/highlight color, contrast, lift, gamma, gain, saturation | — |
| Curves | Hue vs hue, hue vs luminance, hue vs saturation, saturation vs saturation, luminance vs saturation | Master/RGB curves, user 1D/3D LUT (sRGB) |

Saturation, vibrance and the hue/saturation curves run before tonemapping, as in
Unity's HDR grading mode (`LutBuilderHdr`: SMH, LGG, HSV curves and saturation
in linear, before the tonemap) and Blender (compositor in scene linear, view
transform at display), so the tonemapper keeps desaturating hot highlights.
The hue/saturation curves read hue, saturation and luminance from the
sRGB-encoded color (values above 1 kept), so their axes mean what they did
after tonemapping. RGB curves and the user LUT stay after tonemapping in sRGB,
like Unity's user LUT, because they are authored on display images.

Color Correction masks are computed from the scene-linear color entering the
extended modules, before tonemapping, with exposure divided back out in every
renderer, so masks and their preview don't move with camera or auto exposure.
Like LightmapGI, the masks use a log2 luminance (EV) axis: the shadows and
highlights limits are fractions of
`adjustment_tonal_range_bottom`..`adjustment_tonal_range_top` (EV, default
-6..2, so 18% gray falls in the midtones), and each transition is widened by
its softness in EV, split evenly. The storage places the limits in EV once;
`grading_tonal_weights()` takes log2 of the luminance. The tonal range lives in
the Color Correction subgroup, next to the mask preview, because only Color
Correction uses these masks. Overlapping ranges are normalized for
correction. Master-only correction
skips masks. Preview is editor-only. Enabled correction applies directly without an overall strength blend.

White Balance levels are not duplicated in the curve atlas. RGB curves transform
values in 0–1 and preserve values outside this domain. User LUTs sample their
0–1 domain and add back the input's excess headroom. This is a defined extension
of an SDR LUT, not a claim that an arbitrary SDR LUT has an HDR-authored response.

Shadows, Midtones and Highlights also have Lower and Upper (EV). They split each
range by the pixel's position inside it on the EV axis and act like exposure,
in scene-linear HDR before the rest of the correction, in every renderer. The
preview tints each range blue where Lower acts and orange where Upper acts.
Rows 12 and 13 of the correction block carry the range bottom/top and the
per-range Lower/Upper stops, so the block is 14 rows.

## Resources and update work

- `Environment` sends everything but the upstream basic controls as one
  `EnvironmentColorGrading::Settings` through
  `RenderingServer::environment_set_color_grading()` (C++ only, not bound).
  The storage clamps it and derives `EnvironmentColorGrading::Resolved`, which
  renderers read with one `environment_get_color_grading()` call.
  `environment_set_adjustment()` keeps the upstream signature.
- Storage derives neutral/module flags when properties change. A disabled global
  switch produces a zero module mask.
- The existing affine transform uses a cached 48-byte buffer only while active.
- Extended effects use an optional 208-byte std140 buffer at set 4. Its rows are
  packed and compared on active draws; upload occurs only if the packed values
  differ. Different viewports/environments can require updates between draws.
- Curves/LUT use set 5 only in curve variants. The 1024×5 curve atlas is rebuilt
  on curve/property changes and enable transitions, never from the render loop.
- Specialization IDs 4–25 correspond to retired IDs 4–5, correction, correction
  saturation, vibrance, vignette, curve texture, LUT, correction mask, correction contrast, correction LGG, correction color,
  correction positions, correction levels, correction preview, the six curves (RGB, hue vs hue, hue vs saturation,
  saturation vs saturation, luminance vs saturation, hue vs luminance) and a 1D LUT.
  Positions (and Lower/Upper) are compiled only when a range has Lower/Upper or the
  preview shows a range; masks and positions share one log2. Per-curve constants
  replace the per-pixel atlas flag reads, and RGB curves skip the atlas levels, which
  are always neutral in Forward+. The preview constant is never set in games, because
  Environment sends no preview outside the editor. The tone mapper sets the 1D LUT bit.
  They remove inactive effect blocks from a pipeline.
- Disabled/neutral extended groups do not prepare their uniform data or bind
  their buffer/textures. Existing basic tonemapper parameter preparation remains.
- Mobile and Compatibility retain their separate processing implementation.
  Lightmap adjustments remain removed from rendering.

Validation in Codex is restricted to source review, layout/binding checks and
interpreted numerical checks. Engine and shader compilation, visual correctness,
and measured runtime performance remain unverified until run by the user.

Legacy Offset, tonal wheels and global Lift/Gamma/Gain controls are hidden and
inert across all backends. Their shader code and uniform data have been removed.
Master/Shadows/Midtones/Highlights Color Correction remains active.

Forward+ curve enable flags are computed on property changes and passed in the existing grading buffer. No atlas metadata fetches or level fetches are needed in the Forward+ pixel path. Contrast uses the equivalent single-logarithm expression with cached CPU pivot terms.

# Forward+ Environment adjustments

All modules require the global `adjustment_enabled` switch. Subgroup toggles
remain independent; disabled groups retain serialized settings.

| Group | Before tonemapping | After tonemapping |
| --- | --- | --- |
| Basic controls | Brightness, contrast (affine or soft) | Saturation, optional luminance preservation |
| White Balance | Per-channel black/white levels | — |
| Color Grading | Temperature/tint, vignette | Vibrance |
| Color Correction | Master and weighted shadow/midtone/highlight color, contrast, lift, gamma, gain | Weighted saturation |
| Curves | Master/RGB curves, hue vs hue, hue vs luminance, user 1D/3D LUT | Hue vs saturation, saturation vs saturation, luminance vs saturation |

Color Correction masks are computed from the scene-linear color entering the
extended modules, before tonemapping. Overlapping ranges are normalized for
correction. Master-only correction
skips masks. Preview is editor-only. Enabled correction applies directly without an overall strength blend. Selective saturation curves use the post-tonemap inputs.

White Balance levels are not duplicated in the curve atlas. RGB curves transform
values in 0–1 and preserve values outside this domain. User LUTs sample their
0–1 domain and add back the input's excess headroom. This is a defined extension
of an SDR LUT, not a claim that an arbitrary SDR LUT has an HDR-authored response.

## Resources and update work

- Storage derives neutral/module flags when properties change. A disabled global
  switch produces a zero module mask.
- The existing affine transform uses a cached 48-byte buffer only while active.
- Extended effects use an optional 208-byte std140 buffer at set 4. Its rows are
  packed and compared on active draws; upload occurs only if the packed values
  differ. Different viewports/environments can require updates between draws.
- Curves/LUT use set 5 only in curve variants. The 1024×5 curve atlas is rebuilt
  on curve/property changes and enable transitions, never from the render loop.
- Specialization IDs 4–15 correspond to retired IDs 4–5, correction, correction
  saturation, vibrance, vignette, curve texture, LUT, correction mask, correction contrast, correction LGG, and correction color flags.
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

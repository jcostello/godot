# Forward+ GPU packed HDR lightmap cache

The cache is generated with compute only when adjustments or source identity/content version change. Stable frames perform a constant-time cache check; no image readback, CPU texel processing, compression, parameter upload or dispatch occurs on stable game frames. Disabled/neutral adjustments use the original texture.

In the editor (`Engine::is_editor_hint()`), adjustments run per fragment on the sampled original L0, before directional diffuse and generated specular evaluation. Property changes only update parameters; they do not generate a derived texture. The game continues to use the compute-generated cache. Both paths share the grading function in `lightmap_color_adjustments_inc.glsl`. Nonlinear grading after filtering can differ slightly from the game cache, which grades texels before filtering; the editor also retains original directional coefficient precision.

The material shaders retain their descriptor bindings; the lightmap buffer record is now 288 bytes, including 224 bytes of grading parameters. To support this, the derived texture preserves all original layers, resolution and mipmaps. Only L0 is graded; directional coefficients are copied without grading but quantized to the packed format. Generated specular shares adjusted L0 through the original material path. Probes, normals, fog and shadowmasks remain unchanged.

The format is B10G11R11_UFLOAT_PACK32 (GLSL r11f_g11f_b10f), four bytes per texel, positive HDR with no alpha. Values are clamped to [0,64512] to fit the lowest precision channel. Signed BC6H sources and devices without sampling/storage support retain the original lightmap with one error per invalidation. No silent RGBA16F fallback. CPU CVTT is no longer required.

Memory caveat: a full 4096x4096x8 cache occupies 512 MiB (without mipmaps), versus 256 MiB for the former packed-L0 RGBA16F cache and 128 MiB for the full BC6H source. Four-byte texels halve storage relative to an equivalent full-layer RGBA16F cache, not relative to the earlier L0-only cache. Directional coefficient quantization can change the image. No performance benefit is claimed without runtime measurement.

The cache uses a 224-byte parameter buffer uploaded only at regeneration. Shared per-mip views belong to the cached texture and are freed with it; parameter buffers and shader/pipeline resources are freed at destruction. Failed generation is cached until another source/property change.

Validation uses source review, XML parsing and structural checks; no compilation or runtime test performed.

Tonal slider values now map linearly from the original L0 minimum luminance (0) to maximum luminance (1), across all L0 layers at base resolution. The range is read back and scanned once per source identity/content version, including decompression when needed. Property edits reuse it; large source changes can cause a one-time stall. Uniform textures map to level 0. Both preview and cache use this same range, before global saturation. Existing scenes retain their slider values with the new range interpretation. Editor debug masks bypass material shading, directional modulation and fog; camera tonemapping and screen effects still apply.

## Levels per tonal range

Lightmap Shadows, Midtones, and Highlights provide Black Point (0), White Point (1), and Gamma (1). Raise Black Point to deepen the darkest values, lower White Point to strengthen the bright values, and raise Gamma to brighten intermediate values. Black and white are normalized against the maximum luminance of the original L0 texture. The original texture also determines the masks, so adjustments do not move their own tonal selection. Levels scale RGB by the luminance change and retain HDR values above white. They apply after tonal exposure and contrast, before global tint and energy, in both editor preview and the runtime texture cache.

Environment Color Correction adds Black Point and White Point to Master, Shadows, Midtones, and Highlights and reuses their Gamma controls. Levels use luminance in the correction working space: scene-linear HDR in Forward+, and the existing post-tonemap correction space in Mobile and Compatibility. Master levels apply first, then the levels blended with the original pre-tonemap tonal masks, then existing lift/gamma/gain. Neutral points skip the remap and preserve existing scenes.

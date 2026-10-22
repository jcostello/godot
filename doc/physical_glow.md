# Pyramidal bloom

Select **Environment > Glow > Blend Mode > Physical (Pyramidal)** in Forward+ or
Mobile. The existing `GLOW_BLEND_MODE_PHYSICAL = 5` API value is retained.

- **Mix** controls the added bloom amount. Start at 0.04–0.05. This does not fade
  out the sharp source image, even at 1.0.
- **Strength** controls the tent reconstruction radius in source mip texels.
  Start at 1.0. Large radii can expose gaps in the sparse sampling pattern.
- Legacy Levels, Normalize, Intensity, threshold, cap, and glow map controls are
  ignored and hidden in this mode. Existing classic blend modes retain their
  sampling paths, controls, and saved enum values.
- Compatibility continues to use its existing Screen implementation.

## Reference and implementation

The real-time reference is Jorge Jimenez, *Next Generation Post Processing in
Call of Duty: Advanced Warfare*, SIGGRAPH 2014:
https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/

The author describes a threshold-free HDR hierarchy, first-reduction firefly
suppression using normalized luminance weights, and progressive tent filtering.
Unity's original sampling library explicitly attributes its 13-tap reduction
layout to Jimenez 2014 and documents the 9-tap tent reconstruction:
https://github.com/Unity-Technologies/PostProcessing/blob/v2/PostProcessing/Shaders/Sampling.hlsl

The new path has actual reduction and reconstruction passes:

1. Start at half resolution, then reduce through up to six scales with the
   13-bilinear-sample overlapping-box kernel. Each kernel has unit DC gain.
2. Only in the first reduction, weight the samples in each four-sample group
   by `1 / (1 + luminance)` and divide by the sum of weights. The renderer's
   luminance multiplier converts storage-scaled colors into the same luminance
   basis. This suppresses outliers while preserving uniform HDR signals.
3. Reconstruct from the coarsest scale back to half resolution using a normalized
   `[1, 2, 1] × [1, 2, 1]` tent. Accumulate each reduction level with weight
   `1 / level_count`. Source and destination use distinct mip subresources.
4. Sample the reconstructed half-resolution image, apply the same exposure as
   the direct scene, and add `Mix × bloom` before tonemapping. Forward+ includes
   automatic exposure; Mobile uses its existing manual exposure path.

Forward+ runs compute passes and Mobile runs raster passes. They include the
same `bloom_inc.glsl` kernels. Both sample with a linear edge-clamping sampler.
Each view has its own downsample and reconstruction slices. Temporal upscaling
uses the upscaled scene as the bloom source. Existing blur buffers are reused.

The final composition is `exposed_scene + Mix × exposed_bloom`. It preserves
the direct image instead of interpolating toward a blurred replacement. A
uniform image is intentionally brightened by `1 + Mix`, before tonemapping;
this additive effect does not conserve scene energy.

## Adaptations and limitations

This is an engine adaptation of the reference's filtering approach, not the
original game code or a measured optical point spread function. The fixed six
scales, equal scale weights, mip-texel radius, and existing final linear/bicubic
sampling are explicit implementation choices. The reference author describes
customizable radii and a default radius in UV space; this implementation uses
source mip texels. Available scales are reduced for small render targets.

Threshold-free bloom introduces a small amount of softness and veiling across
the whole image. Bright emitters need sufficient HDR contrast to produce a
visible halo at low bloom amounts. The first-pass firefly suppression is
nonlinear and can attenuate legitimate isolated lights. It cannot recover
subpixel lights lost during scene rendering. Invalid samples are zeroed and
negative samples are clamped to zero; there is no artistic upper luminance cap.

Spencer et al., *Physically-Based Glare Effects for Digital Images* (1995), is
background motivation for optical scattering, not the implemented filter:
https://www.graphics.cornell.edu/pubs/1995/SSZG95.pdf

There is no diffraction, spectral PSF, angular/FOV calibration, off-screen light
transport, or temporal bloom history. Storage precision, finite-image borders,
and HDR overflow remain limitations. Sharing kernels does not guarantee identical
GPU results across renderer formats. Up to twelve passes are used; performance
has not been measured.

## Validation

Non-compiling checks cover C++/GLSL formatting, whitespace, documentation XML,
and static review of descriptor sets, mip selection, buffer aliasing and exposure.
A CPU reference using the shader's actual 13 offsets and five group weights
checked 216 constant-image cases: odd dimensions, edges, one/three/six scales,
HDR values, storage multipliers and radii 0/1/2. Additional checks cover a positive
broad impulse halo, preserving the direct component, outlier suppression and
invariance to storage scaling.

These are algorithm checks, not GPU tests. Shader compilation, rendering,
temporal stability and performance remain unverified. Visual validation should
compare small moving emitters, broad bright surfaces, saturated lights, exposure
changes, viewport resizing, HDR output, multiview, and classic-mode scenes.

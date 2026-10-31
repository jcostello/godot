// Optional Forward+ modules. Color correction and vignette use scene-linear HDR;
// saturation, curves, LUT and vibrance are applied after tonemapping.
#ifdef USE_FORWARD_GRADING
layout(set = 4, binding = 0, std140) uniform ForwardGradingData {
	vec4 limits;
	vec4 correction[12];
	vec4 extra; // Vibrance, vignette strength, vignette start/end.
	vec4 controls; // Curve flags, active module flags, LUT dimension, scene-linear correction pivot.
} forward_grading;

layout(constant_id = 6) const bool use_forward_correction = false;
layout(constant_id = 7) const bool use_forward_correction_saturation = false;
layout(constant_id = 8) const bool use_forward_vibrance = false;
layout(constant_id = 9) const bool use_forward_vignette = false;
layout(constant_id = 10) const bool use_forward_curve_textures = false;
layout(constant_id = 11) const bool use_forward_lut = false;
layout(constant_id = 12) const bool use_forward_correction_weights = false;
layout(constant_id = 13) const bool use_forward_correction_contrast = false;
layout(constant_id = 14) const bool use_forward_correction_lgg = false;
layout(constant_id = 15) const bool use_forward_correction_color = false;

#ifdef USE_FORWARD_CURVES
layout(set = 5, binding = 0) uniform sampler2D hue_vs_hue_curve;
layout(set = 5, binding = 1) uniform sampler2D hue_vs_saturation_curve;
layout(set = 5, binding = 2) uniform sampler2D saturation_vs_saturation_curve;
layout(set = 5, binding = 3) uniform sampler2D luminance_vs_saturation_curve;
layout(set = 5, binding = 4) uniform sampler2D forward_lut_1d;
layout(set = 5, binding = 5) uniform sampler3D forward_lut_3d;
#define COLOR_GRADING_CURVES
#endif
// color_adjustments_inc.glsl is included by tonemap.glsl.
#include "../../../shaders/color_grading_inc.glsl"

vec4 forward_correction_parameters(vec3 weights) {
	if (!use_forward_correction_weights) {
		return forward_grading.correction[0];
	}
	return forward_grading.correction[0] * (vec4(1.0) + (forward_grading.correction[1] - vec4(1.0)) * weights.x + (forward_grading.correction[2] - vec4(1.0)) * weights.y + (forward_grading.correction[3] - vec4(1.0)) * weights.z);
}

vec3 forward_adjustments_pre(vec3 color, vec2 uv, out vec3 correction_weights) {
	vec3 weights = vec3(0.0, 1.0, 0.0);
	if (use_forward_correction_weights) {
		weights = grading_tonal_weights(dot(color, vec3(0.2126729, 0.7151522, 0.0721750)), forward_grading.limits);
	}
	correction_weights = use_forward_correction_weights ? grading_normalized_tonal_weights(weights) : vec3(0.0, 1.0, 0.0);
	if (use_forward_correction) {
		vec4 parameters = forward_correction_parameters(correction_weights);
		if (use_forward_correction_color) {
			vec3 modulation = forward_grading.correction[6].rgb;
			if (use_forward_correction_weights) {
				modulation *= clamp(vec3(1.0) + (forward_grading.correction[7].rgb - vec3(1.0)) * correction_weights.x + (forward_grading.correction[8].rgb - vec3(1.0)) * correction_weights.y + (forward_grading.correction[9].rgb - vec3(1.0)) * correction_weights.z, vec3(0.0), vec3(1.0));
			}
			color *= modulation;
		}
		if (use_forward_correction_contrast && parameters.y != 1.0) {
			color = adjustment_log_contrast(color, parameters.y, forward_grading.controls.w);
		}
		color = grading_tonal_levels(color, correction_weights, forward_grading.correction);
		if (use_forward_correction_lgg) {
			// Lift is converted to scene-linear on the CPU.
			float lift = forward_grading.correction[4].x;
			if (use_forward_correction_weights) {
				lift += dot(forward_grading.correction[4].yzw, correction_weights);
			}
			if (parameters.z != 1.0 || parameters.w != 1.0 || lift != 0.0) {
				vec3 lifted = color * parameters.w + vec3(lift);
				vec3 magnitude = abs(lifted);
				float inverse_gamma = 1.0 / max(parameters.z, 0.0025);
				// Gamma pins 0 and 1; HDR above 1 continues with the slope at 1 instead of exploding.
				vec3 graded = mix(pow(magnitude, vec3(inverse_gamma)), vec3(1.0) + (magnitude - vec3(1.0)) * inverse_gamma, greaterThan(magnitude, vec3(1.0)));
				color = sign(lifted) * min(graded, vec3(65504.0));
			}
		}
	}
	if (use_forward_vignette) {
		color *= grading_vignette(uv, params.pixel_size.y / params.pixel_size.x, forward_grading.extra.y, forward_grading.extra.zw);
	}
	return color;
}

vec3 forward_adjustments_post(vec3 color, vec3 correction_weights) {
	if (use_forward_correction_saturation) {
		float saturation = forward_correction_parameters(correction_weights).x;
		if (saturation != 1.0) {
			color = adjustment_gamut_saturation(max(color, vec3(0.0)), saturation);
		}
	}
#ifdef USE_FORWARD_CURVES
	if (use_forward_curve_textures || use_forward_lut) {
		// Curves and LUTs are authored on display values, like the other renderers: grade sRGB.
		vec3 encoded = adjustment_linear_to_srgb(max(color, vec3(0.0)));
		if (use_forward_curve_textures) {
			encoded = apply_color_grading_curves(encoded);
		}
		if (use_forward_lut) {
			// User LUTs cover 0..1. Values above 1 keep their offset, so the result stays continuous.
			vec3 input_color = clamp(encoded, vec3(0.0), vec3(1.0));
			vec3 mapped;
			if (forward_grading.controls.z > 0.5) {
				float width = float(textureSize(forward_lut_1d, 0).x);
				vec3 x = (input_color * (width - 1.0) + 0.5) / width;
				mapped = vec3(texture(forward_lut_1d, vec2(x.r, 0.5)).r, texture(forward_lut_1d, vec2(x.g, 0.5)).g, texture(forward_lut_1d, vec2(x.b, 0.5)).b);
			} else {
				vec3 size = vec3(textureSize(forward_lut_3d, 0));
				mapped = texture(forward_lut_3d, (input_color * (size - 1.0) + 0.5) / size).rgb;
			}
			encoded = mapped + (encoded - input_color);
		}
		color = adjustment_srgb_to_linear(max(encoded, vec3(0.0)));
	}
#endif
	if (use_forward_vibrance) {
		color = apply_grading_vibrance(color, forward_grading.extra.x);
	}
	return color;
}
#endif // USE_FORWARD_GRADING

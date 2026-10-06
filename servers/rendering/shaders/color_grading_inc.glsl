vec3 grading_rgb_to_hsv(vec3 c) {
	vec4 k = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
	vec4 p = mix(vec4(c.bg, k.wz), vec4(c.gb, k.xy), step(c.b, c.g));
	vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
	float d = q.x - min(q.w, q.y);
	float e = 1.0e-10;
	return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 grading_hsv_to_rgb(vec3 c) {
	vec3 p = abs(fract(c.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
	return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
}

vec3 apply_grading_vibrance(vec3 color, float vibrance) {
	// Contrast and offset can produce negative RGB. HSV requires non-negative
	// components, but its value component must remain unbounded for HDR.
	vec3 hsv = grading_rgb_to_hsv(max(color, vec3(0.0)));
	if (vibrance > 0.0) {
		hsv.y += hsv.y * (1.0 - hsv.y) * vibrance;
	} else {
		hsv.y *= 1.0 + vibrance;
	}
	hsv.y = clamp(hsv.y, 0.0, 1.0);
	return grading_hsv_to_rgb(hsv);
}

vec3 grading_tonal_weights(float luminance, vec4 limits) {
	float shadows_transition = limits.y > limits.x ? smoothstep(limits.x, limits.y, luminance) : step(limits.x, luminance);
	float highlights_transition = limits.w > limits.z ? smoothstep(limits.z, limits.w, luminance) : step(limits.z, luminance);
	float shadows = 1.0 - shadows_transition;
	float highlights = highlights_transition;
	return vec3(shadows, 1.0 - shadows - highlights, highlights);
}

vec3 apply_grading_tonal_wheels(vec3 color, vec3 weights, vec4 shadows, vec4 midtones, vec4 highlights) {
	return color * (shadows.rgb * weights.x + midtones.rgb * weights.y + highlights.rgb * weights.z);
}

vec3 apply_grading_lift_gamma_gain(vec3 color, vec3 lift, vec3 inverse_gamma, vec3 gain) {
	vec3 lifted = color * gain + lift;
	// Match Unity's sign(x) * pow(abs(x), gamma) and only cap values that cannot
	// be represented by the half-float HDR targets used by these render paths.
	if (all(equal(inverse_gamma, vec3(1.0)))) {
		return clamp(lifted, vec3(-65504.0), vec3(65504.0));
	}
	vec3 powered = pow(abs(lifted), inverse_gamma);
	return sign(lifted) * min(powered, vec3(65504.0));
}

float grading_vignette(vec2 uv, float aspect, float strength, vec2 range) {
	vec2 centered_uv = uv * 2.0 - 1.0;
	centered_uv.x *= aspect;
	float radius = length(centered_uv) / max(length(vec2(aspect, 1.0)), 0.00001);
	float start = clamp(range.x, 0.0, 0.999);
	float end = clamp(range.y, start + 0.001, 1.0);
	return 1.0 - clamp(strength * 0.5, 0.0, 1.0) * smoothstep(start, end, radius);
}

vec3 grading_normalized_tonal_weights(vec3 weights) {
	// Artist-defined ranges may overlap. Correction parameters and masks must
	// remain convex, even when the legacy wheel weights have negative midtones.
	weights = max(weights, vec3(0.0));
	return weights / max(dot(weights, vec3(1.0)), 0.00001);
}

float grading_tonal_correction_mask(vec3 weights, int preview) {
	if (preview == 1) { // Master covers the whole image.
		return 1.0;
	}
	return weights[clamp(preview - 2, 0, 2)];
}

// Uniform checks avoid masks for disabled correction and Master-only changes.
bool grading_correction_needs_weights(vec4 correction[10]) {
	if (correction[5].y < 0.5) {
		return false;
	}
	if (correction[5].z > 1.5) {
		return true;
	}
	return correction[5].x > 0.0 && (any(notEqual(correction[1], vec4(1.0))) || any(notEqual(correction[2], vec4(1.0))) || any(notEqual(correction[3], vec4(1.0))) || any(notEqual(correction[4].yzw, vec3(0.0))) || any(notEqual(correction[7].rgb, vec3(1.0))) || any(notEqual(correction[8].rgb, vec3(1.0))) || any(notEqual(correction[9].rgb, vec3(1.0))));
}

vec3 apply_grading_linear(vec3 color, vec4 limits, vec4 shadows, vec4 midtones, vec4 highlights, vec4 lift, vec4 gamma, vec4 gain, float amount, vec4 correction[10], out vec3 correction_weights) {
	correction_weights = vec3(0.0, 1.0, 0.0);
	bool use_wheels = any(notEqual(shadows.rgb, vec3(1.0))) || any(notEqual(midtones.rgb, vec3(1.0))) || any(notEqual(highlights.rgb, vec3(1.0)));
	bool use_correction_weights = grading_correction_needs_weights(correction);
	vec3 weights = vec3(0.0, 1.0, 0.0);
	if (use_wheels || use_correction_weights) {
		weights = grading_tonal_weights(dot(color, vec3(0.2126729, 0.7151522, 0.0721750)), limits);
		if (use_correction_weights) {
			correction_weights = grading_normalized_tonal_weights(weights);
		}
	}
	if (!use_wheels && gain.a <= 0.5) {
		return color;
	}
	vec3 original = color;
	if (use_wheels) {
		color = apply_grading_tonal_wheels(color, weights, shadows, midtones, highlights);
	}
	if (gain.a > 0.5) {
		color = apply_grading_lift_gamma_gain(color, lift.rgb, gamma.rgb, gain.rgb);
	}
	return amount == 1.0 ? color : mix(original, color, amount);
}

vec3 apply_grading_tonal_correction(vec3 color, vec3 weights, vec4 correction[10]) {
	vec4 controls = correction[5];
	if (controls.y < 0.5 || controls.x == 0.0) {
		return color;
	}
	// Blend parameters using the same pre-tonemap masks as the tonal wheels.
	// Master multipliers affect all ranges; Master Lift adds to the local lift.
	vec4 parameters = correction[0] * (vec4(1.0) + (correction[1] - vec4(1.0)) * weights.x + (correction[2] - vec4(1.0)) * weights.y + (correction[3] - vec4(1.0)) * weights.z);
	float combined_lift = correction[4].x + dot(correction[4].yzw, weights);
	vec3 modulation = correction[6].rgb * clamp(vec3(1.0) + (correction[7].rgb - vec3(1.0)) * weights.x + (correction[8].rgb - vec3(1.0)) * weights.y + (correction[9].rgb - vec3(1.0)) * weights.z, vec3(0.0), vec3(1.0));
	if (all(equal(parameters, vec4(1.0))) && combined_lift == 0.0 && all(equal(modulation, vec3(1.0)))) {
		return color;
	}
	vec3 corrected = color;
	if (any(notEqual(modulation, vec3(1.0)))) {
		corrected = adjustment_linear_to_srgb(adjustment_srgb_to_linear(max(color, vec3(0.0))) * modulation);
	}
	if (parameters.x != 1.0) {
		corrected = adjustment_luminance_saturation(corrected, parameters.x);
	}
	if (parameters.y != 1.0) {
		corrected = adjustment_soft_contrast(corrected, parameters.y, controls.w);
	}
	if (parameters.z != 1.0 || parameters.w != 1.0 || combined_lift != 0.0) {
		corrected = apply_grading_lift_gamma_gain(corrected, vec3(combined_lift), vec3(1.0 / max(parameters.z, 0.0025)), vec3(parameters.w));
	}
	return controls.x == 1.0 ? corrected : mix(color, corrected, controls.x);
}

#ifdef COLOR_GRADING_CURVES
float sample_periodic_hue_curve(sampler2D curve_texture, float hue) {
	float texel_size = 1.0 / float(textureSize(curve_texture, 0).x);
	// CurveTexture stores sample i at i / width. Address its texel center.
	float value = texture(curve_texture, vec2(hue + 0.5 * texel_size, 0.5)).r;
	// Join the edge texels, preserving edits at red. Limit the transition
	// to one texel so narrow features are not erased by a fixed-width seam.
	if (hue >= texel_size && hue <= 1.0 - texel_size) {
		return value;
	}
	float seam_value = 0.5 * (texture(curve_texture, vec2(0.0, 0.5)).r + texture(curve_texture, vec2(1.0, 0.5)).r);
	if (hue < texel_size) {
		return mix(seam_value, value, hue / texel_size);
	}
	if (hue > 1.0 - texel_size) {
		return mix(value, seam_value, (hue - (1.0 - texel_size)) / texel_size);
	}
	return value;
}

// Environment can pack its additional curves into the existing fourth binding.
// A one-row texture still has the RenderingServer API's original meaning.
vec4 sample_grading_atlas(float input_value, int row) {
	vec2 size = vec2(textureSize(luminance_vs_saturation_curve, 0));
	vec2 uv = vec2((clamp(input_value, 0.0, 1.0) * (size.x - 1.0) + 0.5) / size.x, (float(row) + 0.5) / size.y);
	return texture(luminance_vs_saturation_curve, uv);
}

float sample_grading_hue_luminance(float hue) {
	float texel_size = 1.0 / float(textureSize(luminance_vs_saturation_curve, 0).x);
	float value = sample_grading_atlas(hue, 1).r;
	if (hue >= texel_size && hue <= 1.0 - texel_size) {
		return value;
	}
	float seam = 0.5 * (sample_grading_atlas(0.0, 1).r + sample_grading_atlas(1.0, 1).r);
	if (hue < texel_size) {
		return mix(seam, value, hue / texel_size);
	}
	if (hue > 1.0 - texel_size) {
		return mix(value, seam, (hue - (1.0 - texel_size)) / texel_size);
	}
	return value;
}

vec3 apply_grading_rgb_curves(vec3 color) {
	vec4 white = texelFetch(luminance_vs_saturation_curve, ivec2(0, 4), 0);
	if (white.a == 0.0) {
		return color;
	}
	vec3 black = texelFetch(luminance_vs_saturation_curve, ivec2(0, 3), 0).rgb;
	vec3 leveled = (color - black) / max(white.rgb - black, vec3(0.0001));
	vec3 curved = leveled;
	// RGB curves cover display values 0..1. Preserve HDR and negative headroom
	// outside that interval rather than clamping it at the texture boundary.
	for (int channel = 0; channel < 3; channel++) {
		if (leveled[channel] >= 0.0 && leveled[channel] <= 1.0) {
			curved[channel] = sample_grading_atlas(leveled[channel], 2)[channel];
		}
	}
	return mix(color, curved, white.a);
}

vec3 apply_grading_hue_luminance(vec3 color, float hue, float saturation) {
	float curve_value = clamp(sample_grading_hue_luminance(hue), 0.0, 1.0);
	if (curve_value == 0.5 || saturation == 0.0) {
		return color;
	}
	// 0.5 is neutral; endpoints are -2 and +2 exposure stops. Fade the
	// correction near gray, where hue is unstable and otherwise causes banding.
	float multiplier = exp2((curve_value - 0.5) * 4.0 * smoothstep(0.0, 0.05, saturation));
	vec3 linear_color = adjustment_srgb_to_linear(max(color, vec3(0.0)));
	float luminance = dot(linear_color, vec3(0.2126, 0.7152, 0.0722));
	float upper = max(1.0, max(linear_color.r, max(linear_color.g, linear_color.b)));
	float target = min(luminance * multiplier, upper);
	vec3 chroma = (linear_color - vec3(luminance)) * multiplier;
	float scale = 1.0;
	for (int channel = 0; channel < 3; channel++) {
		if (chroma[channel] > 0.0) {
			scale = min(scale, (upper - target) / chroma[channel]);
		} else if (chroma[channel] < 0.0) {
			scale = min(scale, -target / chroma[channel]);
		}
	}
	return adjustment_linear_to_srgb(max(vec3(target) + chroma * scale, vec3(0.0)));
}

vec3 apply_color_grading_curves(vec3 color) {
	bool extended_curves = textureSize(luminance_vs_saturation_curve, 0).y == 5;
	vec4 selective_flags = vec4(1.0);
	vec4 luminance_flags = vec4(1.0);
	if (extended_curves) {
		color = apply_grading_rgb_curves(color);
		selective_flags = texelFetch(luminance_vs_saturation_curve, ivec2(0, 0), 0);
		luminance_flags = texelFetch(luminance_vs_saturation_curve, ivec2(0, 1), 0);
		if (all(equal(selective_flags.yzw, vec3(0.0))) && all(equal(luminance_flags.yz, vec2(0.0)))) {
			return color;
		}
	}
	// Use perceptual sRGB luma for the existing luminance vs saturation curve.
	float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
	vec3 hsv = grading_rgb_to_hsv(max(color, vec3(0.0)));
	float input_hue = hsv.x;
	float input_saturation = hsv.y;
	if (selective_flags.y > 0.5) {
		hsv.x = fract(hsv.x + sample_periodic_hue_curve(hue_vs_hue_curve, input_hue) - 0.5);
	}
	if (selective_flags.z > 0.5) {
		hsv.y *= 2.0 * sample_periodic_hue_curve(hue_vs_saturation_curve, input_hue);
	}
	if (selective_flags.w > 0.5) {
		hsv.y *= 2.0 * texture(saturation_vs_saturation_curve, vec2(input_saturation, 0.5)).r;
	}
	if (luminance_flags.y > 0.5) {
		float saturation_factor = extended_curves ? sample_grading_atlas(luminance, 0).r : texture(luminance_vs_saturation_curve, vec2(clamp(luminance, 0.0, 1.0), 0.5)).r;
		hsv.y *= 2.0 * saturation_factor;
	}
	hsv.y = clamp(hsv.y, 0.0, 1.0);
	color = grading_hsv_to_rgb(hsv);
	return extended_curves && luminance_flags.z > 0.5 ? apply_grading_hue_luminance(color, input_hue, input_saturation) : color;
}

#endif // COLOR_GRADING_CURVES

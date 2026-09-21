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

#ifdef COLOR_GRADING_CURVES
float sample_periodic_hue_curve(sampler2D curve_texture, float hue) {
	float texel_size = 1.0 / float(textureSize(curve_texture, 0).x);
	// CurveTexture stores sample i at i / width. Address its texel center.
	float value = texture(curve_texture, vec2(hue + 0.5 * texel_size, 0.5)).r;
	// Join the edge texels, preserving edits at red. Limit the transition
	// to one texel so narrow features are not erased by a fixed-width seam.
	float seam_value = 0.5 * (texture(curve_texture, vec2(0.0, 0.5)).r + texture(curve_texture, vec2(1.0, 0.5)).r);
	if (hue < texel_size) {
		return mix(seam_value, value, hue / texel_size);
	}
	if (hue > 1.0 - texel_size) {
		return mix(value, seam_value, (hue - (1.0 - texel_size)) / texel_size);
	}
	return value;
}

vec3 apply_color_grading_curves(vec3 color) {
	// Use perceptual sRGB luma to match the tonal wheel ranges.
	float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
	// Preserve the value component so neutral curves are an identity transform
	// for HDR colors. Only physically invalid negative light is discarded.
	vec3 hsv = grading_rgb_to_hsv(max(color, vec3(0.0)));
	float input_hue = hsv.x;
	float input_saturation = hsv.y;
	hsv.x = fract(hsv.x + sample_periodic_hue_curve(hue_vs_hue_curve, input_hue) - 0.5);
	hsv.y *= 2.0 * sample_periodic_hue_curve(hue_vs_saturation_curve, input_hue);
	hsv.y *= 2.0 * texture(saturation_vs_saturation_curve, vec2(input_saturation, 0.5)).r;
	hsv.y *= 2.0 * texture(luminance_vs_saturation_curve, vec2(clamp(luminance, 0.0, 1.0), 0.5)).r;
	hsv.y = clamp(hsv.y, 0.0, 1.0);
	return grading_hsv_to_rgb(hsv);
}

#endif // COLOR_GRADING_CURVES

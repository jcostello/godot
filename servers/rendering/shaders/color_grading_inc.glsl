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

float sample_periodic_hue_curve(sampler2D curve_texture, float hue) {
	const float seam_width = 1.0 / 64.0;
	float value = texture(curve_texture, vec2(hue, 0.5)).r;
	float seam_value = 0.5 * (texture(curve_texture, vec2(seam_width, 0.5)).r + texture(curve_texture, vec2(1.0 - seam_width, 0.5)).r);
	if (hue < seam_width) {
		return mix(seam_value, value, hue / seam_width);
	}
	if (hue > 1.0 - seam_width) {
		return mix(value, seam_value, (hue - (1.0 - seam_width)) / seam_width);
	}
	return value;
}

vec3 apply_color_grading_curves(vec3 color) {
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

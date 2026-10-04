// Optional alternatives to the legacy sRGB contrast and saturation controls.
// Callers provide sRGB-encoded color after tonemapping.
float adjustment_soft_contrast_channel(float value, float contrast, float pivot) {
	// Preserve HDR headroom and negative values from preceding adjustments.
	if (contrast == 1.0 || value <= 0.0 || value >= 1.0) {
		return value;
	}
	if (contrast <= 0.0) {
		return pivot;
	}
	// A pivot-centered sigmoid. Log odds avoid overflow with large contrast.
	float log_odds = contrast * (log(value / pivot) - log((1.0 - value) / (1.0 - pivot))) + log(pivot / (1.0 - pivot));
	float e = exp(-abs(log_odds));
	return log_odds >= 0.0 ? 1.0 / (1.0 + e) : e / (1.0 + e);
}

vec3 adjustment_soft_contrast(vec3 color, float contrast, float pivot) {
	return vec3(adjustment_soft_contrast_channel(color.r, contrast, pivot),
			adjustment_soft_contrast_channel(color.g, contrast, pivot),
			adjustment_soft_contrast_channel(color.b, contrast, pivot));
}

vec3 adjustment_srgb_to_linear(vec3 color) {
	return mix(pow((color + vec3(0.055)) / 1.055, vec3(2.4)), color / 12.92, lessThan(color, vec3(0.04045)));
}

vec3 adjustment_linear_to_srgb(vec3 color) {
	return mix(1.055 * pow(color, vec3(1.0 / 2.4)) - vec3(0.055), 12.92 * color, lessThan(color, vec3(0.0031308)));
}

vec3 adjustment_luminance_saturation(vec3 color, float saturation) {
	if (saturation == 1.0) {
		return color;
	}
	vec3 linear_color = adjustment_srgb_to_linear(max(color, vec3(0.0)));
	float luminance = dot(linear_color, vec3(0.2126, 0.7152, 0.0722));
	vec3 chroma = (linear_color - vec3(luminance)) * max(saturation, 0.0);
	// Preserve existing HDR headroom. Reduce chroma toward the same-luminance
	// neutral axis to fit the RGB gamut, rather than clipping channels separately.
	float upper = max(1.0, max(linear_color.r, max(linear_color.g, linear_color.b)));
	float scale = 1.0;
	for (int channel = 0; channel < 3; channel++) {
		if (chroma[channel] > 0.0) {
			scale = min(scale, (upper - luminance) / chroma[channel]);
		} else if (chroma[channel] < 0.0) {
			scale = min(scale, -luminance / chroma[channel]);
		}
	}
	return adjustment_linear_to_srgb(max(vec3(luminance) + chroma * scale, vec3(0.0)));
}

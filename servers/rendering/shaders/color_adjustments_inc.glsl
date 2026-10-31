// Optional alternatives to the legacy sRGB contrast and saturation controls.
// The sRGB helpers expect sRGB-encoded color after tonemapping.

// A pivot-centered sigmoid. p_bias is (1 - contrast) * logit(pivot), so callers can cache it.
// Log odds avoid overflow with large contrast.
float adjustment_sigmoid_contrast(float value, float contrast, float pivot, float bias) {
	// Preserve HDR headroom and negative values from preceding adjustments.
	if (contrast == 1.0 || value <= 0.0 || value >= 1.0) {
		return value;
	}
	if (contrast <= 0.0) {
		return pivot;
	}
	float log_odds = contrast * log(value / (1.0 - value)) + bias;
	float e = exp(-abs(log_odds));
	return log_odds >= 0.0 ? 1.0 / (1.0 + e) : e / (1.0 + e);
}

vec3 adjustment_sigmoid_contrast(vec3 color, float contrast, float pivot, float bias) {
	return vec3(adjustment_sigmoid_contrast(color.r, contrast, pivot, bias),
			adjustment_sigmoid_contrast(color.g, contrast, pivot, bias),
			adjustment_sigmoid_contrast(color.b, contrast, pivot, bias));
}

// Scales chroma around luminance. Chroma is reduced toward the same-luminance neutral
// axis to fit the RGB gamut, rather than clipping channels separately. HDR headroom is kept.
vec3 adjustment_gamut_saturation(vec3 color, float saturation) {
	float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
	vec3 chroma = (color - vec3(luminance)) * saturation;
	float upper = max(1.0, max(color.r, max(color.g, color.b)));
	float scale = 1.0;
	for (int channel = 0; channel < 3; channel++) {
		if (chroma[channel] > 0.0) {
			scale = min(scale, (upper - luminance) / chroma[channel]);
		} else if (chroma[channel] < 0.0) {
			scale = min(scale, -luminance / chroma[channel]);
		}
	}
	return max(vec3(luminance) + chroma * scale, vec3(0.0));
}

vec3 adjustment_soft_contrast(vec3 color, float contrast, float pivot) {
	return adjustment_sigmoid_contrast(color, contrast, pivot, (1.0 - contrast) * log(pivot / (1.0 - pivot)));
}

// Scene-linear contrast in log2 space around a pivot. Continuous over the whole HDR range and never clips.
vec3 adjustment_log_contrast(vec3 color, float contrast, float pivot) {
	return min(pivot * pow(max(color, vec3(1e-8)) / pivot, vec3(contrast)), vec3(65504.0));
}

// Same curve driven by luminance, so hue and saturation are kept.
vec3 adjustment_luminance_log_contrast(vec3 color, float contrast, float pivot) {
	color = max(color, vec3(0.0));
	float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
	if (luminance <= 1e-8) {
		return color;
	}
	return min(color * (pivot * pow(luminance / pivot, contrast) / luminance), vec3(65504.0));
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
	return adjustment_linear_to_srgb(adjustment_gamut_saturation(adjustment_srgb_to_linear(max(color, vec3(0.0))), max(saturation, 0.0)));
}

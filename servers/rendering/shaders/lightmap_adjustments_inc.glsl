// Runtime grading in linear HDR space. Keep in sync with LightmapAdjustments::_apply().
vec3 lightmap_tonal_weights(float luma, vec4 ranges) {
	float range = max(luma, 0.0) / (1.0 + max(luma, 0.0));
	float shadows = 1.0 - smoothstep(ranges.x, ranges.y, range);
	float highlights = smoothstep(ranges.z, ranges.w, range);
	return vec3(shadows, 1.0 - shadows - highlights, highlights);
}

vec3 lightmap_apply_adjustments(vec3 light, vec4 tint_energy, vec4 grading, vec4 tonal, vec4 shadows_tint, vec4 midtones_tint, vec4 highlights_tint, vec4 tonal_contrast, vec4 ranges) {
	if (grading.w == 0.0 || tonal.w != 0.0) {
		return light;
	}
	if (tint_energy.a == 0.0) {
		return vec3(0.0);
	}
	if (grading.x != 1.0) {
		float luminance = dot(light, vec3(0.2126, 0.7152, 0.0722));
		light = mix(vec3(luminance), light, grading.x);
	}
	if (tonal_contrast.w != 0.0) {
		light = max(light, vec3(0.0));
		float luma = dot(light, vec3(0.2126, 0.7152, 0.0722));
		if (luma > 0.00001) {
			vec3 weights = lightmap_tonal_weights(luma, ranges);
			float stops = dot(tonal.xyz, weights);
			float range_saturation = dot(vec3(shadows_tint.a, midtones_tint.a, highlights_tint.a), weights);
			float range_contrast = dot(tonal_contrast.xyz, weights);
			if (range_saturation != 1.0) {
				light = max(mix(vec3(luma), light, range_saturation), vec3(0.0));
			}
			float adjusted_luma = dot(light, vec3(0.2126, 0.7152, 0.0722));
			light *= shadows_tint.rgb * weights.x + midtones_tint.rgb * weights.y + highlights_tint.rgb * weights.z;
			float exponent = min(grading.y * range_contrast, 4.0);
			if (exponent != 1.0) {
				light *= 0.18 * pow(adjusted_luma / 0.18, exponent) / max(adjusted_luma, 0.00001);
			}
			if (stops != 0.0) {
				light *= exp2(stops);
			}
		}
	}
	return light * tint_energy.rgb * tint_energy.a;
}

vec3 lightmap_debug_color(vec3 light, vec4 grading, vec4 ranges, float mode, float exposure_normalization) {
	// Show only the range weights, before per-range adjustments.
	float luminance = dot(light, vec3(0.2126, 0.7152, 0.0722));
	vec3 saturated = grading.x == 1.0 ? light : mix(vec3(luminance), light, grading.x);
	float luma = dot(max(saturated, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
	vec3 weights = lightmap_tonal_weights(luma, ranges);
	float weight = mode == 1.0 ? weights.x : (mode == 2.0 ? weights.y : weights.z);
	// Match the lightmap's render exposure; final camera exposure is applied later.
	return vec3(weight) * exposure_normalization;
}

// Shared by realtime lighting, volumetric fog, GI and the lightmapper.
#ifndef SPOT_LIGHT_INC_GLSL
#define SPOT_LIGHT_INC_GLSL

highp float light_range_fade(highp float distance, highp float inv_range, highp float fade_start) {
	highp float normalized_distance = distance * inv_range;
	if (fade_start < 0.0) {
		highp float fade = max(1.0 - pow(normalized_distance, 4.0), 0.0);
		return fade * fade;
	}
	highp float t = clamp((1.0 - normalized_distance) / max(1.0 - fade_start, 0.0001), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

// A non-negative fade start enables the two-radius omni profile. Keep the
// inverse-power profile for existing lights with fade_start == -1.
highp float omni_light_attenuation(highp float distance, highp float inv_range, highp float attenuation, highp float fade_start) {
	if (fade_start < 0.0) {
		return light_range_fade(distance, inv_range, fade_start) * pow(max(distance, 0.0001), -attenuation);
	}
	highp float normalized_distance = distance * inv_range;
	if (normalized_distance >= 1.0) {
		return 0.0;
	}
	if (normalized_distance <= fade_start) {
		return 1.0;
	}
	highp float t = clamp((1.0 - normalized_distance) / max(1.0 - fade_start, 0.0001), 0.0, 1.0);
	highp float bias = clamp(attenuation, 0.0001, 10000.0);
	t = t * bias / (1.0 - t + t * bias);
	return t * t * (3.0 - 2.0 * t);
}

highp float spot_light_attenuation(highp float cos_angle, highp float cos_outer, highp float cos_inner, highp float inv_attenuation) {
	// An empty cone must not divide by zero, including in the legacy profile.
	if (cos_outer >= 1.0) {
		return 0.0;
	}
	// A cosine above 1 selects the original profile for existing lights.
	if (cos_inner > 1.0) {
		highp float scos = max(cos_angle, cos_outer);
		highp float spot_rim = max(0.0001, (1.0 - scos) / (1.0 - cos_outer));
		return 1.0 - pow(spot_rim, inv_attenuation);
	}

	// Test endpoints before dividing: coincident cones give a hard edge.
	if (cos_angle <= cos_outer) {
		return 0.0;
	}
	if (cos_angle >= cos_inner) {
		return 1.0;
	}

	// Interpolate in angle space so the fade occupies the same fraction of
	// the visible angular interval regardless of the cone aperture.
	highp float angle = acos(clamp(cos_angle, -1.0, 1.0));
	highp float outer_angle = acos(clamp(cos_outer, -1.0, 1.0));
	highp float inner_angle = acos(clamp(cos_inner, -1.0, 1.0));
	highp float t = clamp((outer_angle - angle) / (outer_angle - inner_angle), 0.0, 1.0);
	// Bias the transition, then smooth it with zero slope at both boundaries.
	// Clamp only the new profile, including infinite reciprocals from attenuation 0.
	highp float bias = clamp(inv_attenuation, 0.0001, 10000.0);
	t = (t * bias) / (1.0 - t + t * bias);
	return t * t * (3.0 - 2.0 * t);
}

#endif // SPOT_LIGHT_INC_GLSL

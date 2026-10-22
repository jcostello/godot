#ifndef BLOOM_INC_GLSL
#define BLOOM_INC_GLSL

// Pyramidal bloom inspired by Jimenez, SIGGRAPH 2014:
// https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/
// The 13-tap overlapping-box layout is also documented in Unity's sampling library:
// https://github.com/Unity-Technologies/PostProcessing/blob/v2/PostProcessing/Shaders/Sampling.hlsl
// Both renderers include these kernels, keeping sampling positions and gains identical.
const vec2 bloom_downsample_offsets[13] = vec2[13](
		vec2(-1.0, -1.0), vec2(0.0, -1.0), vec2(1.0, -1.0),
		vec2(-0.5, -0.5), vec2(0.5, -0.5),
		vec2(-1.0, 0.0), vec2(0.0, 0.0), vec2(1.0, 0.0),
		vec2(-0.5, 0.5), vec2(0.5, 0.5),
		vec2(-1.0, 1.0), vec2(0.0, 1.0), vec2(1.0, 1.0));

vec3 bloom_average4(vec3 a, vec3 b, vec3 c, vec3 d, bool p_first_pass, float p_luminance_multiplier) {
	if (!p_first_pass) {
		return (a + b + c + d) * 0.25;
	}

	// Karis-style firefly suppression is a normalized weighted average, not
	// a compression followed by an inverse. A uniform HDR source is unchanged.
	vec3 luma = vec3(0.2126, 0.7152, 0.0722) * p_luminance_multiplier;
	vec4 weights = 1.0 / (1.0 + vec4(dot(a, luma), dot(b, luma), dot(c, luma), dot(d, luma)));
	return (a * weights.x + b * weights.y + c * weights.z + d * weights.w) / dot(weights, vec4(1.0));
}

vec3 bloom_downsample_13(sampler2D p_source, vec2 p_uv, vec2 p_pixel_size, bool p_first_pass, float p_luminance_multiplier) {
	vec3 taps[13];
	for (int i = 0; i < 13; i++) {
		vec3 value = textureLod(p_source, p_uv + bloom_downsample_offsets[i] * p_pixel_size, 0.0).rgb;
		// Invalid source pixels must not contaminate every subsequent scale.
		taps[i] = any(isnan(value)) || any(isinf(value)) ? vec3(0.0) : max(value, vec3(0.0));
	}

	vec3 bloom = bloom_average4(taps[3], taps[4], taps[8], taps[9], p_first_pass, p_luminance_multiplier) * 0.5;
	bloom += bloom_average4(taps[0], taps[1], taps[5], taps[6], p_first_pass, p_luminance_multiplier) * 0.125;
	bloom += bloom_average4(taps[1], taps[2], taps[6], taps[7], p_first_pass, p_luminance_multiplier) * 0.125;
	bloom += bloom_average4(taps[5], taps[6], taps[10], taps[11], p_first_pass, p_luminance_multiplier) * 0.125;
	bloom += bloom_average4(taps[6], taps[7], taps[11], taps[12], p_first_pass, p_luminance_multiplier) * 0.125;
	return bloom;
}

vec3 bloom_upsample_tent(sampler2D p_source, vec2 p_uv, vec2 p_pixel_size) {
	vec3 bloom = vec3(0.0);
	// Separable [1, 2, 1] x [1, 2, 1], normalized to unit gain.
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			float weight = float((2 - abs(x)) * (2 - abs(y))) * (1.0 / 16.0);
			bloom += textureLod(p_source, p_uv + vec2(x, y) * p_pixel_size, 0.0).rgb * weight;
		}
	}
	return bloom;
}

#endif // BLOOM_INC_GLSL

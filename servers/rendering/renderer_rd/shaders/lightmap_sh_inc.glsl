// Non-negative evaluation of L1 SH lightmaps.

// Non-linear L1 reconstruction from Graham Hazel, "Reflections on Spherical Harmonics" (Geomerics).
// Unlike the linear p_l0 + dot(p_l1, n) it never goes negative.
float lightmap_sh_l1_shape(float p_l0, vec3 p_l1, vec3 p_n) {
	float l1_len = length(p_l1);
	if (p_l0 <= 0.0 || l1_len < 1e-6) {
		return max(p_l0, 0.0);
	}
	// r > 1 has no non-negative solution, so clamp it.
	float r = min(0.5 * l1_len / p_l0, 1.0);
	float q = 0.5 * (1.0 + dot(p_l1 / l1_len, p_n));
	float p = 1.0 + 2.0 * r;
	float a = (1.0 - r) / (1.0 + r);
	return p_l0 * (a + (1.0 - a) * (p + 1.0) * pow(max(q, 0.0), p));
}

// The bake stores |L1| up to 4 * L0 so the linear result is exact at the baked (vertex) normal, but it goes
// negative when normal maps tilt away from the light. Use the non-linear shape, rescaled to match the linear
// result at p_baked_normal, so surfaces without normal maps look exactly as before.
vec3 lightmap_sh_l1_evaluate(vec3 p_l0, vec3 p_l1x, vec3 p_l1y, vec3 p_l1z, vec3 p_normal, vec3 p_baked_normal) {
	vec3 result;
	for (int i = 0; i < 3; i++) {
		vec3 l1 = vec3(p_l1x[i], p_l1y[i], p_l1z[i]);
		float linear_baked = max(p_l0[i] + dot(l1, p_baked_normal), 0.0);
		float shape_baked = lightmap_sh_l1_shape(p_l0[i], l1, p_baked_normal);
		float scale = shape_baked > 1e-6 ? min(linear_baked / shape_baked, 4.0) : 0.0;
		result[i] = lightmap_sh_l1_shape(p_l0[i], l1, p_normal) * scale;
	}
	return result;
}

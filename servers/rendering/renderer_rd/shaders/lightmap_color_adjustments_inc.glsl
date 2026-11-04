// Grades a lightmap color through the LUT baked by lightmap_color_cache.glsl.
// params: x = log2 of the first cell, y = 1 / stops covered, z = (size - 1) / size, w = 0.5 / size.
// Grading extrapolates past either end (slope 1 in stops); the debug mask stays at its edge value.
vec3 lightmap_apply_color_lut(texture3D lut, sampler lut_sampler, vec3 light, vec4 params, bool extrapolate) {
	vec3 t = (log2(max(light, vec3(1e-8))) - params.x) * params.y;
	vec3 inside = clamp(t, 0.0, 1.0);
	vec3 graded = textureLod(sampler3D(lut, lut_sampler), inside * params.z + params.w, 0.0).rgb;
	return exp2(extrapolate ? graded + (t - inside) / params.y : graded);
}

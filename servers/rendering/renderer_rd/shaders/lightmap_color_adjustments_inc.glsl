// Shared by the texture cache and per-pixel editor preview.
vec3 lightmap_grade_color(vec3 light, vec4 data[11]) {
	vec4 basic = data[4];
	if (basic.y == 1.0 && basic.z == 0.0 && basic.w == 0.0) {
		return vec3(dot(data[0].xyz, light), dot(data[1].xyz, light), dot(data[2].xyz, light));
	}
	const vec3 luminance_weights = vec3(0.2126, 0.7152, 0.0722);
	float source_luminance = dot(max(light, vec3(0.0)), luminance_weights);
	if (basic.x != 1.0) {
		light = mix(vec3(dot(light, luminance_weights)), light, basic.x);
	}
	light = max(light, vec3(0.0));
	float luminance = dot(light, luminance_weights);
	vec3 weights = vec3(0.0, 1.0, 0.0);
	if (basic.z != 0.0 || basic.w != 0.0) {
		vec4 ranges = data[5];
		// Use the original texture range so edits never change their own selection.
		float level = clamp((source_luminance - data[3].w) * data[10].w, 0.0, 1.0);
		float shadows = 1.0 - smoothstep(ranges.x, ranges.y, level);
		float highlights = smoothstep(ranges.z, ranges.w, level);
		weights = max(vec3(shadows, 1.0 - shadows - highlights, highlights), vec3(0.0));
		weights /= max(dot(weights, vec3(1.0)), 0.00001);
	}
	if (basic.w != 0.0) {
		return vec3(weights[clamp(int(basic.w) - 1, 0, 2)]);
	}
	float contrast = basic.y;
	if (basic.z != 0.0) {
		vec4 shadows = data[7];
		vec4 midtones = data[8];
		vec4 highlights = data[9];
		float saturation = dot(vec3(shadows.w, midtones.w, highlights.w), weights);
		if (saturation != 1.0) {
			light = max(mix(vec3(luminance), light, saturation), vec3(0.0));
		}
		luminance = dot(light, luminance_weights);
		light *= shadows.rgb * weights.x + midtones.rgb * weights.y + highlights.rgb * weights.z;
		contrast = min(contrast * dot(data[10].xyz, weights), 4.0);
		float stops = dot(data[6].xyz, weights);
		if (stops != 0.0) {
			light *= exp2(stops);
		}
	}
	if (contrast != 1.0 && luminance > 0.00001) {
		light *= 0.18 * pow(luminance / 0.18, contrast) / luminance;
	}
	return light * data[3].xyz;
}


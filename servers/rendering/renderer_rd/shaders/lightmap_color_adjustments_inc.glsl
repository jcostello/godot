// Shared by the texture cache and per-pixel editor preview.
vec3 lightmap_grade_color(vec3 light, vec4 data[16]) {
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
	// Position inside each range: 0 at its dark end, 1 at its bright end.
	vec3 positions = vec3(0.5);
	if (basic.z != 0.0 || basic.w != 0.0) {
		vec4 ranges = data[5];
		// Use the original texture range so edits never change their own selection.
		// The axis is log2 luminance between the lightmap's 1st and 99th percentiles.
		// Unclamped, so transitions widened past the range ends keep fading instead of stopping.
		float level = (log2(max(source_luminance, 1e-8)) - data[3].w) * data[10].w;
		// Softness is in stops, so narrowing the range with its offsets doesn't sharpen the transitions.
		float soft = 0.5 * data[6].w * data[10].w;
		float shadows = 1.0 - smoothstep(ranges.x - soft, ranges.y + soft, level);
		float highlights = smoothstep(ranges.z - soft, ranges.w + soft, level);
		weights = max(vec3(shadows, 1.0 - shadows - highlights, highlights), vec3(0.0));
		weights /= max(dot(weights, vec3(1.0)), 0.00001);
		// Shadows span from the range bottom, highlights to its top, midtones across both transitions.
		vec3 dark_end = vec3(0.0, ranges.x - soft, ranges.z - soft);
		vec3 bright_end = vec3(ranges.y + soft, ranges.w + soft, 1.0);
		positions = clamp((vec3(level) - dark_end) / max(bright_end - dark_end, vec3(0.0001)), 0.0, 1.0);
	}
	if (basic.w != 0.0) {
		// Blue marks the part Lower affects, orange the part Upper affects.
		int range = clamp(int(basic.w) - 1, 0, 2);
		return weights[range] * mix(vec3(0.4, 0.65, 1.0), vec3(1.0, 0.7, 0.35), positions[range]);
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
		float stops = dot(data[6].xyz, weights) + dot(data[14].xyz, weights * (1.0 - positions)) + dot(data[15].xyz, weights * positions);
		if (stops != 0.0) {
			light *= exp2(stops);
		}
	}
	if (contrast != 1.0 && luminance > 0.00001) {
		// Pivot at the lightmap's geometric middle luminance; a fixed value would also change brightness.
		float pivot = data[11].w;
		light *= pivot * pow(luminance / pivot, contrast) / luminance;
	}
	if (basic.z != 0.0) {
		float black = dot(data[11].xyz, weights);
		float white = dot(data[12].xyz, weights);
		float gamma = dot(data[13].xyz, weights);
		if (black != 0.0 || white != 1.0 || gamma != 1.0) {
			float current_luminance = dot(light, luminance_weights);
			if (current_luminance > 0.0) {
				// Levels act on the same log2 axis as the tonal ranges. Outside black..white the
				// mapping stays linear in stops, so nothing is crushed to zero or clipped.
				float level = (log2(current_luminance) - data[3].w) * data[10].w;
				float leveled = (level - black) / max(white - black, 0.001);
				if (leveled > 0.0 && leveled < 1.0) {
					leveled = pow(leveled, 1.0 / max(gamma, 0.05));
				}
				float target = min(exp2(leveled / data[10].w + data[3].w), 64512.0);
				light = min((light / current_luminance) * target, vec3(64512.0));
			}
		}
	}
	return light * data[3].xyz;
}


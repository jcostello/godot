#[versions]

lines = "#define MODE_LINES";
triangles = "#define MODE_TRIANGLES";
margins = "#define MODE_MARGINS";

#[vertex]

#version 450

#VERSION_DEFINES

#include "lm_common_inc.glsl"

layout(push_constant, std430) uniform Params {
	uint base_index;
	uint slice;
	vec2 uv_offset;
	float blend;
	uint subslices;
	uint pad;
}
params;

layout(location = 0) out vec3 uv_interp;
#if defined(MODE_MARGINS) || defined(MODE_LINES)
layout(location = 1) flat out uint seam_index;
#endif

void main() {
#ifdef MODE_TRIANGLES
	uint triangle_idx = params.base_index + gl_VertexIndex / 3;
	uint triangle_subidx = gl_VertexIndex % 3;

	vec2 uv;
	if (triangle_subidx == 0) {
		uv = vertices.data[triangles.data[triangle_idx].indices.x].uv;
	} else if (triangle_subidx == 1) {
		uv = vertices.data[triangles.data[triangle_idx].indices.y].uv;
	} else {
		uv = vertices.data[triangles.data[triangle_idx].indices.z].uv;
	}

	uv_interp = vec3(uv, float(params.slice));
	gl_Position = vec4((uv + params.uv_offset) * 2.0 - 1.0, 0.0001, 1.0);
#endif

#ifdef MODE_MARGINS
	seam_index = params.base_index + gl_VertexIndex / 6;
	Seam seam = seams.data[seam_index];
	// Only this source slice is resident in the reusable geometry textures.
	if (seam.src_slice != params.pad) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		uv_interp = vec3(0.0);
		return;
	}
	const vec2 corners[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(0, 1), vec2(1, 0), vec2(1, 1));
	vec2 corner = corners[gl_VertexIndex % 6];
	vec2 edge = (seam.dst_uv.zw - seam.dst_uv.xy) * vec2(bake_params.atlas_size);
	float edge_length = length(edge);
	vec2 outward = edge_length > 0.0 ? vec2(-edge.y, edge.x) / edge_length : vec2(0.0);
	if (dot(outward, (seam.opposite_uv.zw - seam.dst_uv.xy) * vec2(bake_params.atlas_size)) > 0.0) {
		outward = -outward;
	}
	vec2 dst_uv = mix(seam.dst_uv.xy, seam.dst_uv.zw, corner.x);
	// Cover the endpoint footprint as well as the edge strip. Limit the cap to
	// one atlas texel (and half the edge length for very short seams).
	vec2 tangent = edge_length > 0.0 ? edge / edge_length : vec2(0.0);
	dst_uv += tangent * (corner.x * 2.0 - 1.0) * min(1.0, edge_length * 0.5) / vec2(bake_params.atlas_size);
	dst_uv += outward * corner.y * params.blend / vec2(bake_params.atlas_size);
	uv_interp = vec3(dst_uv, float(params.slice));
	// The closest seam wins where temporary margins overlap.
	gl_Position = vec4(dst_uv * 2.0 - 1.0, 0.1 + 0.8 * corner.y, 1.0);
#endif

#ifdef MODE_LINES
	uint seam_idx = params.base_index + gl_VertexIndex / 2;
	seam_index = seam_idx;
	bool end_vertex = (gl_VertexIndex % 2) != 0;
	Seam seam = seams.data[seam_idx];
	vec2 src_uv = end_vertex ? seam.src_uv.zw : seam.src_uv.xy;
	vec2 dst_uv = (end_vertex ? seam.dst_uv.zw : seam.dst_uv.xy) + params.uv_offset;

	uint src_slice = seam.src_slice;
	uv_interp = vec3(src_uv, float(src_slice));
	gl_Position = vec4(dst_uv * 2.0 - 1.0, 0.0001, 1.0);
#endif
}

#[fragment]

#version 450

#VERSION_DEFINES

#include "lm_common_inc.glsl"

layout(push_constant, std430) uniform Params {
	uint base_index;
	uint slice;
	vec2 uv_offset;
	float blend;
	uint subslices;
	uint pad;
}
params;

layout(location = 0) in vec3 uv_interp;
#if defined(MODE_MARGINS) || defined(MODE_LINES)
layout(location = 1) flat in uint seam_index;
#endif

layout(location = 0) out vec4 dst_color;

#ifdef MODE_MARGINS
layout(location = 1) out vec4 dst_normal;
layout(set = 1, binding = 0) uniform utexture2DArray mesh_tex;
layout(set = 1, binding = 1) uniform texture2DArray normal_tex;
layout(set = 1, binding = 2) uniform texture2DArray unocclude_tex;
#elif defined(MODE_LINES)
layout(set = 1, binding = 0) uniform texture2DArray src_color_tex;
layout(set = 1, binding = 1) uniform utexture2DArray invalid_samples_tex;
layout(set = 1, binding = 2) uniform utexture2DArray mesh_tex;
#else
layout(set = 1, binding = 0) uniform texture2DArray src_color_tex;
#endif

#ifdef MODE_LINES
bool source_texel_covered(Seam seam, ivec2 p_pos) {
	if (any(lessThan(p_pos, ivec2(0))) || any(greaterThanEqual(p_pos, bake_params.atlas_size))) {
		return false;
	}
	vec2 edge = seam.src_uv.zw - seam.src_uv.xy;
	vec2 interior = seam.opposite_uv.xy - seam.src_uv.xy;
	float determinant = edge.x * interior.y - edge.y * interior.x;
	if (determinant == 0.0) {
		return false;
	}
	vec2 delta = (vec2(p_pos) + 0.5) / vec2(bake_params.atlas_size) - seam.src_uv.xy;
	vec2 barycentric = vec2(delta.x * interior.y - delta.y * interior.x, edge.x * delta.y - edge.y * delta.x) / determinant;
	if (any(lessThan(barycentric, vec2(-1e-5))) || barycentric.x + barycentric.y > 1.00001) {
		return false;
	}
	ivec3 source = ivec3(p_pos, seam.src_slice);
	return texelFetch(usampler2DArray(mesh_tex, linear_sampler), source, 0).r == seam.src_mesh &&
			texelFetch(sampler2DArray(src_color_tex, linear_sampler), source, 0).a > 0.5;
}
#endif

void main() {
#ifdef MODE_MARGINS
	Seam seam = seams.data[seam_index];
	ivec2 dst_pos = ivec2(gl_FragCoord.xy);
	if (seam.uv_transform[0].w == 0.0 || texelFetch(usampler2DArray(mesh_tex, linear_sampler), ivec3(dst_pos, params.slice), 0).r != 0u) {
		discard;
	}
	vec2 src_uv = vec2(dot(seam.uv_transform[0].xyz, vec3(uv_interp.xy, 1.0)), dot(seam.uv_transform[1].xyz, vec3(uv_interp.xy, 1.0)));
	vec2 sample_uv = src_uv;
	vec2 sample_pos = sample_uv * vec2(bake_params.atlas_size) - 0.5;
	ivec2 base = ivec2(floor(sample_pos));
	vec2 fraction = fract(sample_pos);
	vec2 edge = seam.src_uv.zw - seam.src_uv.xy;
	vec2 interior = seam.opposite_uv.xy - seam.src_uv.xy;
	float determinant = edge.x * interior.y - edge.y * interior.x;
	if (determinant == 0.0) {
		discard;
	}
	vec2 dst_edge = seam.dst_uv.zw - seam.dst_uv.xy;
	float along = clamp(dot(uv_interp.xy - seam.dst_uv.xy, dst_edge) / dot(dst_edge, dst_edge), 0.0, 1.0);
	vec3 expected_normal = normalize(mix(seam.normal[0].xyz, seam.normal[1].xyz, along));
	bool full_footprint_valid = true;
	float best_weight = 0.0;
	ivec2 best_tap = ivec2(0);
	for (int y = 0; y < 2; y++) {
		for (int x = 0; x < 2; x++) {
			float weight = (x == 0 ? 1.0 - fraction.x : fraction.x) * (y == 0 ? 1.0 - fraction.y : fraction.y);
			if (weight == 0.0) {
				continue;
			}
			ivec2 tap = base + ivec2(x, y);
			if (any(lessThan(tap, ivec2(0))) || any(greaterThanEqual(tap, bake_params.atlas_size))) {
				full_footprint_valid = false;
				continue;
			}
			vec2 delta = (vec2(tap) + 0.5) / vec2(bake_params.atlas_size) - seam.src_uv.xy;
			vec2 barycentric = vec2(delta.x * interior.y - delta.y * interior.x, edge.x * delta.y - edge.y * delta.x) / determinant;
			if (any(lessThan(barycentric, vec2(-1e-5))) || barycentric.x + barycentric.y > 1.00001) {
				full_footprint_valid = false;
				continue;
			}
			ivec3 source = ivec3(tap, seam.src_slice);
			if (texelFetch(usampler2DArray(mesh_tex, linear_sampler), source, 0).r != seam.src_mesh) {
				full_footprint_valid = false;
				continue;
			}
			vec3 normal = texelFetch(sampler2DArray(normal_tex, linear_sampler), ivec3(tap, 0), 0).xyz;
			vec3 tap_expected_normal = expected_normal;
			if (seam.mesh == seam.src_mesh) {
				// Smooth internal seams curve away from their endpoint normals.
				// Validate against the actual source triangle at this texel.
				tap_expected_normal = normalize(seam.source_normal[0].xyz * (1.0 - barycentric.x - barycentric.y) +
						seam.source_normal[1].xyz * barycentric.x + seam.source_normal[2].xyz * barycentric.y);
			}
			if (dot(normal, tap_expected_normal) < 0.999 || texelFetch(sampler2DArray(unocclude_tex, linear_sampler), ivec3(tap, 0), 0).r > 0.0) {
				full_footprint_valid = false;
				continue;
			}
			if (weight > best_weight) {
				best_weight = weight;
				best_tap = tap;
			}
		}
	}
	if (best_weight == 0.0) {
		discard;
	}
	if (!full_footprint_valid) {
		// At triangle boundaries, use the validated texel with the largest weight.
		// Sample its center so filtering cannot pull in a rejected neighbor.
		sample_uv = (vec2(best_tap) + 0.5) / vec2(bake_params.atlas_size);
	}
	// UV, source atlas slice, and the mesh allowed to consume this margin.
	dst_color = vec4(sample_uv, float(seam.src_slice), float(seam.mesh));
	// Match light padding's validated footprint and preserve empty coverage.
	dst_normal = vec4(textureLod(sampler2DArray(normal_tex, linear_sampler), vec3(sample_uv, 0.0), 0.0).rgb, 0.0);
#else
#ifdef MODE_LINES
	Seam seam = seams.data[seam_index];
	vec2 source_uv = uv_interp.xy;
	vec2 sample_pos = source_uv * vec2(bake_params.atlas_size) - 0.5;
	ivec2 base = ivec2(floor(sample_pos));
	vec2 fraction = fract(sample_pos);
	bool full_footprint_valid = true;
	float best_weight = 0.0;
	ivec2 best_tap = ivec2(0);
	for (int y = 0; y < 2; y++) {
		for (int x = 0; x < 2; x++) {
			float weight = (x == 0 ? 1.0 - fraction.x : fraction.x) * (y == 0 ? 1.0 - fraction.y : fraction.y);
			if (weight == 0.0) {
				continue;
			}
			ivec2 tap = base + ivec2(x, y);
			if (!source_texel_covered(seam, tap)) {
				full_footprint_valid = false;
				continue;
			}
			if (weight > best_weight) {
				best_weight = weight;
				best_tap = tap;
			}
		}
	}
	// Preserve bilinear sampling when its whole footprint is covered. At an
	// edge, use a covered tap before searching farther into the triangle.
	if (!full_footprint_valid && best_weight > 0.0) {
		source_uv = (vec2(best_tap) + 0.5) / vec2(bake_params.atlas_size);
	}
	if (best_weight == 0.0) {
		vec2 source_edge = (seam.src_uv.zw - seam.src_uv.xy) * vec2(bake_params.atlas_size);
		float source_edge_length = length(source_edge);
		vec2 source_inward = source_edge_length > 0.0 ? vec2(-source_edge.y, source_edge.x) / source_edge_length : vec2(0.0);
		if (dot(source_inward, (seam.opposite_uv.xy - seam.src_uv.xy) * vec2(bake_params.atlas_size)) < 0.0) {
			source_inward = -source_inward;
		}

		bool found_source = false;
		for (int radius = 1; radius <= 8; radius++) {
			vec2 candidate_uv = uv_interp.xy + source_inward * float(radius) / vec2(bake_params.atlas_size);
			ivec2 candidate_pos = ivec2(floor(candidate_uv * vec2(bake_params.atlas_size)));
			if (source_texel_covered(seam, candidate_pos) &&
					texelFetch(usampler2DArray(invalid_samples_tex, linear_sampler), ivec3(candidate_pos, seam.src_slice), 0).r < max(params.pad / 32u, 1u)) {
				source_uv = (vec2(candidate_pos) + 0.5) / vec2(bake_params.atlas_size);
				found_source = true;
				break;
			}
		}
		if (!found_source) {
			discard;
		}
	}
	vec4 src_color = textureLod(sampler2DArray(src_color_tex, linear_sampler), vec3(source_uv, uv_interp.z), 0.0);
	dst_color = vec4(src_color.rgb, params.blend);
#else
	vec4 src_color = textureLod(sampler2DArray(src_color_tex, linear_sampler), uv_interp, 0.0);
	dst_color = vec4(src_color.rgb, params.blend);
#endif
#endif
}

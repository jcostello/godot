#[versions]

primary = "#define MODE_DIRECT_LIGHT";
secondary = "#define MODE_BOUNCE_LIGHT";
dilate = "#define MODE_DILATE";
pad_oidn = "#define MODE_PAD_OIDN";
pad_oidn_shadowmask = "#define MODE_PAD_OIDN\n#define PAD_OIDN_SHADOWMASK";
unocclude = "#define MODE_UNOCCLUDE";
light_probes = "#define MODE_LIGHT_PROBES";
denoise = "#define MODE_DENOISE";
pack_coeffs = "#define MODE_PACK_L1_COEFFS";
ambient_occlusion = "#define MODE_AMBIENT_OCCLUSION";

#[compute]

#version 450

#VERSION_DEFINES

#if defined(MODE_DIRECT_LIGHT) || defined(MODE_BOUNCE_LIGHT) || defined(MODE_LIGHT_PROBES) || defined(MODE_UNOCCLUDE) || defined(MODE_AMBIENT_OCCLUSION)
#define LM_RAY_TRACING_PASS
#endif

#define M_PI 3.14159265359

#extension GL_EXT_samplerless_texture_functions : enable

#ifdef USE_HARDWARE_RAYTRACING
#extension GL_EXT_ray_query : require
#endif

// One 2D local group focusing in one layer at a time, though all
// in parallel (no barriers) makes more sense than a 3D local group
// as this can take more advantage of the cache for each group.

#ifdef MODE_LIGHT_PROBES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#else

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#endif

#include "../../servers/rendering/shaders/spot_light_inc.glsl"
#include "lm_area_lights_inc.glsl"
#include "lm_common_inc.glsl"

#ifdef MODE_LIGHT_PROBES

layout(set = 1, binding = 0, std430) restrict buffer LightProbeData {
	vec4 data[];
}
light_probes;

layout(set = 1, binding = 1) uniform texture2DArray source_light;
layout(set = 1, binding = 2) uniform texture2D environment;
#endif

#ifdef MODE_UNOCCLUDE

layout(rgba32f, set = 1, binding = 0) uniform restrict image2DArray position;
layout(rgba32f, set = 1, binding = 1) uniform restrict image2DArray unocclude;

#endif

#if defined(MODE_DIRECT_LIGHT) || defined(MODE_BOUNCE_LIGHT)
layout(rgba16f, set = 1, binding = 0) uniform restrict writeonly image2DArray dest_light;
#endif

#if defined(MODE_DIRECT_LIGHT) || defined(MODE_BOUNCE_LIGHT)
layout(set = 1, binding = 1) uniform texture2DArray source_light;
layout(set = 1, binding = 2) uniform texture2DArray source_position;
layout(set = 1, binding = 3) uniform texture2DArray source_normal;
layout(rgba16f, set = 1, binding = 4) uniform restrict image2DArray accum_light;
layout(set = 1, binding = 8) uniform texture2DArray source_geometry;

#endif

#ifdef MODE_DIRECT_LIGHT
layout(rgba16f, set = 1, binding = 7) uniform restrict writeonly image2DArray direct_light;
#endif

#if defined(MODE_DIRECT_LIGHT) && defined(USE_SHADOWMASK)
layout(rgba8, set = 1, binding = 5) uniform restrict writeonly image2DArray shadowmask;
#elif defined(MODE_BOUNCE_LIGHT)
layout(set = 1, binding = 5) uniform texture2D environment;
layout(r32ui, set = 1, binding = 7) uniform restrict uimage2DArray invalid_samples;
// Adaptive sampling: x = sum of sample luminance, y = sum of squared luminance (negative once converged).
layout(rg32f, set = 1, binding = 10) uniform restrict image2DArray adaptive_stats;
#endif

#if defined(MODE_DIRECT_LIGHT) || defined(MODE_BOUNCE_LIGHT) || defined(MODE_LIGHT_PROBES)
layout(set = 1, binding = 6) uniform texture2D area_light_atlas;
#endif

#if defined(MODE_BOUNCE_LIGHT) || defined(MODE_LIGHT_PROBES)
// Layout: per-cell pdf (w * h), marginal row CDF (h), per-row conditional CDFs (w * h).
layout(set = 1, binding = 9, std430) restrict readonly buffer EnvironmentCDF {
	float data[];
}
env_cdf;
#endif

#if defined(MODE_PAD_OIDN) || defined(MODE_DILATE) || defined(MODE_DENOISE) || defined(MODE_PACK_L1_COEFFS)
#ifdef PAD_OIDN_SHADOWMASK
layout(rgba8, set = 1, binding = 0) uniform restrict writeonly image2DArray dest_light;
#else
layout(rgba16f, set = 1, binding = 0) uniform restrict writeonly image2DArray dest_light;
#endif
layout(set = 1, binding = 1) uniform texture2DArray source_light;
#endif

#ifdef MODE_PAD_OIDN
layout(set = 1, binding = 2) uniform texture2DArray oidn_margin_tex;
layout(set = 1, binding = 3) uniform utexture2DArray oidn_mesh_tex;
#endif

#ifdef MODE_AMBIENT_OCCLUSION
layout(rgba16f, set = 1, binding = 0) uniform restrict image2DArray ao_light;
layout(set = 1, binding = 1) uniform texture2DArray ao_position;
layout(set = 1, binding = 2) uniform texture2DArray ao_normal;
layout(rgba16f, set = 1, binding = 3) uniform restrict readonly image2DArray ao_direct_light;
layout(set = 1, binding = 4) uniform texture2DArray ao_geometry;
#endif

#ifdef MODE_DENOISE
layout(set = 1, binding = 2) uniform texture2DArray source_normal;
layout(set = 1, binding = 3) uniform texture2DArray unocclude_mask;
layout(set = 1, binding = 4) uniform DenoiseParams {
	float spatial_bandwidth;
	float light_bandwidth;
	float albedo_bandwidth;
	float normal_bandwidth;

	int half_search_window;
	float filter_strength;
	uint slice_count;
}
denoise_params;
#endif

layout(push_constant, std430) uniform Params {
	uint atlas_slice;
	uint geometry_slice;
	ivec2 region_ofs;

	uint ray_count;
	uint ray_from;
	uint ray_to;
	uint probe_count;

	uint denoiser_range;
	uint material_slice;
	uint output_slice;
	uint ray_pass_size;
}
params;

#ifdef LM_RAY_TRACING_PASS
//check it, but also return distance and barycentric coords (for uv lookup)
bool ray_hits_triangle(vec3 from, vec3 dir, float max_dist, vec3 p0, vec3 p1, vec3 p2, out float r_distance, out vec3 r_barycentric) {
	const float EPSILON = 0.00001;
	const vec3 e0 = p1 - p0;
	const vec3 e1 = p0 - p2;
	vec3 triangle_normal = cross(e1, e0);

	float n_dot_dir = dot(triangle_normal, dir);

	if (abs(n_dot_dir) < EPSILON) {
		return false;
	}

	const vec3 e2 = (p0 - from) / n_dot_dir;
	const vec3 i = cross(dir, e2);

	r_barycentric.y = dot(i, e1);
	r_barycentric.z = dot(i, e0);
	r_barycentric.x = 1.0 - (r_barycentric.z + r_barycentric.y);
	r_distance = dot(triangle_normal, e2);

	return (r_distance > bake_params.bias) && (r_distance < max_dist) && all(greaterThanEqual(r_barycentric, vec3(0.0)));
}

const uint RAY_MISS = 0;
const uint RAY_FRONT = 1;
const uint RAY_BACK = 2;
const uint RAY_ANY = 3;

bool ray_box_test(vec3 p_from, vec3 p_inv_dir, float p_min_distance, float p_max_distance, vec3 p_box_min, vec3 p_box_max) {
	vec3 t0 = (p_box_min - p_from) * p_inv_dir;
	vec3 t1 = (p_box_max - p_from) * p_inv_dir;
	vec3 tmin = min(t0, t1), tmax = max(t0, t1);
	float entry_distance = max(tmin.x, max(tmin.y, tmin.z));
	float exit_distance = min(tmax.x, min(tmax.y, tmax.z));
	return max(entry_distance, p_min_distance) <= min(exit_distance, p_max_distance);
}

#if CLUSTER_SIZE > 32
#define CLUSTER_TRIANGLE_ITERATION
#endif

vec4 sample_triangle_albedo_alpha(uint p_triangle, vec3 p_barycentric) {
	Triangle triangle = triangles.data[p_triangle];
	Vertex vert0 = vertices.data[triangle.indices.x];
	Vertex vert1 = vertices.data[triangle.indices.y];
	Vertex vert2 = vertices.data[triangle.indices.z];
	vec3 uvw = vec3(p_barycentric.x * vert0.uv + p_barycentric.y * vert1.uv + p_barycentric.z * vert2.uv, float(triangle.slice));
	return textureLod(sampler2DArray(albedo_tex, linear_sampler), uvw, 0.0);
}

vec4 apply_triangle_alpha_scissor(uint p_triangle, vec4 p_albedo_alpha) {
	float threshold = triangles.data[p_triangle].alpha_scissor_threshold;
	if (threshold >= 0.0) {
		// The material bake already applied the material threshold. Reconstruct
		// its filtered binary mask with a fixed midpoint, not the original threshold.
		p_albedo_alpha.a = p_albedo_alpha.a >= 0.5 ? 1.0 : 0.0;
	}
	return p_albedo_alpha;
}

bool alpha_scissor_rejects(uint p_triangle, vec3 p_barycentric) {
	return apply_triangle_alpha_scissor(p_triangle, sample_triangle_albedo_alpha(p_triangle, p_barycentric)).a == 0.0;
}

#ifdef USE_HARDWARE_RAYTRACING
#include "lm_ray_query_inc.glsl"
#else

uint trace_ray(vec3 p_from, vec3 p_to, bool p_any_hit, bool p_sample_albedo, out float r_distance, out vec3 r_normal, out uint r_triangle, out vec3 r_barycentric, out vec4 r_albedo_alpha) {
	// Negative alpha marks an opaque/transparent material that has not been sampled yet.
	r_albedo_alpha = vec4(0.0, 0.0, 0.0, -1.0);
	// World coordinates.
	vec3 rel = p_to - p_from;
	float rel_len = length(rel);
	vec3 dir = normalize(rel);
	vec3 inv_dir = 1.0 / dir;

	// Cell coordinates.
	vec3 from_cell = (p_from - bake_params.to_cell_offset) * bake_params.to_cell_size;
	vec3 to_cell = (p_to - bake_params.to_cell_offset) * bake_params.to_cell_size;

	// Prepare DDA.
	vec3 rel_cell = to_cell - from_cell;
	ivec3 icell = ivec3(from_cell);
	ivec3 iendcell = ivec3(to_cell);
	// Keep DDA distances in world units so they can also bound the AABB tests.
	vec3 dir_cell = dir * bake_params.to_cell_size;
	vec3 delta = abs(1.0 / dir_cell);
	ivec3 step = ivec3(sign(rel_cell));
	const vec3 init_next_cell = vec3(icell) + max(vec3(0), sign(step));
	vec3 t_max = mix(vec3(1e20), (init_next_cell - from_cell) / dir_cell, notEqual(step, ivec3(0))); // Distance to next boundary.
	float cell_entry_distance = 0.0;

	uint iters = 0;
	while (all(greaterThanEqual(icell, ivec3(0))) && all(lessThan(icell, ivec3(bake_params.grid_size))) && (iters < 1000)) {
		uvec2 cell_data = texelFetch(grid, icell, 0).xy;
		uint triangle_count = cell_data.x;
		if (triangle_count > 0) {
			float cell_exit_distance = min(rel_len, min(t_max.x, min(t_max.y, t_max.z)));
			uint hit = RAY_MISS;
			float best_distance = 1e20;
			uint cluster_start = cluster_indices.data[cell_data.y * 2];
			uint cell_triangle_start = cluster_indices.data[cell_data.y * 2 + 1];
			uint cluster_count = (triangle_count + CLUSTER_SIZE - 1) / CLUSTER_SIZE;
			uint cluster_base_index = 0;
			while (cluster_base_index < cluster_count) {
				// To minimize divergence, all Ray-AABB tests on the clusters contained in the cell are performed
				// before checking against the triangles. We do this 32 clusters at a time and store the intersected
				// clusters on each bit of the 32-bit integer.
				uint cluster_test_count = min(32, cluster_count - cluster_base_index);
				uint cluster_hits = 0;
				for (uint i = 0; i < cluster_test_count; i++) {
					uint cluster_index = cluster_start + cluster_base_index + i;
					ClusterAABB cluster_aabb = cluster_aabbs.data[cluster_index];
					if (ray_box_test(p_from, inv_dir, cell_entry_distance, min(cell_exit_distance, best_distance + bake_params.bias), cluster_aabb.min_bounds, cluster_aabb.max_bounds)) {
						cluster_hits |= (1 << i);
					}
				}

				// Check the triangles in any of the clusters that were intersected by toggling off the bits in the
				// 32-bit integer counter until no bits are left.
				while (cluster_hits > 0) {
					uint cluster_index = findLSB(cluster_hits);
					cluster_hits &= ~(1 << cluster_index);
					cluster_index += cluster_base_index;

					// Do the same divergence execution trick with triangles as well.
					uint triangle_base_index = 0;
#ifdef CLUSTER_TRIANGLE_ITERATION
					while (triangle_base_index < min(CLUSTER_SIZE, triangle_count - cluster_index * CLUSTER_SIZE))
#endif
					{
						uint triangle_cluster_offset = cluster_index * CLUSTER_SIZE + triangle_base_index;
						uint triangle_start_index = cell_triangle_start + triangle_cluster_offset;
						uint triangle_test_count = min(32u, min(CLUSTER_SIZE - triangle_base_index, triangle_count - triangle_cluster_offset));
						uint triangle_hits = 0;
						for (uint i = 0; i < triangle_test_count; i++) {
							uint triangle_index = triangle_indices.data[triangle_start_index + i];
							if (ray_box_test(p_from, inv_dir, cell_entry_distance, min(cell_exit_distance, best_distance + bake_params.bias), triangles.data[triangle_index].min_bounds, triangles.data[triangle_index].max_bounds)) {
								triangle_hits |= (1 << i);
							}
						}

						while (triangle_hits > 0) {
							uint cluster_triangle_index = findLSB(triangle_hits);
							triangle_hits &= ~(1 << cluster_triangle_index);
							cluster_triangle_index += triangle_start_index;

							uint triangle_index = triangle_indices.data[cluster_triangle_index];
							Triangle triangle = triangles.data[triangle_index];

							// Gather the triangle vertex positions.
							vec3 vtx0 = vertices.data[triangle.indices.x].position;
							vec3 vtx1 = vertices.data[triangle.indices.y].position;
							vec3 vtx2 = vertices.data[triangle.indices.z].position;
							float distance;
							vec3 barycentric;
							if (ray_hits_triangle(p_from, dir, rel_len, vtx0, vtx1, vtx2, distance, barycentric)) {
								vec3 position = p_from + dir * distance;
								vec3 hit_cell = (position - bake_params.to_cell_offset) * bake_params.to_cell_size;
								if (icell != ivec3(hit_cell)) {
									// It's possible for the ray to hit a triangle in a position outside the bounds of the cell
									// if it's large enough to cover multiple ones. The hit must be ignored if this is the case.
									continue;
								}

								if (p_any_hit) {
									if (triangle.alpha_scissor_threshold >= 0.0 && alpha_scissor_rejects(triangle_index, barycentric)) {
										continue;
									}
									// Return early if any hit was requested.
									return RAY_ANY;
								}

								// Only compute the normal after confirming an intersection in this cell.
								vec3 normal = -cross(vtx0 - vtx1, vtx0 - vtx2);
								bool backface = dot(normal, dir) >= 0.0;
								if (!backface) {
									// The case of meshes having both a front and back face in the same plane is more common than
									// expected, so if this is a front-face, bias it closer to the ray origin, so it always wins
									// over the back-face.
									distance = max(bake_params.bias, distance - bake_params.bias);
								}

								if (distance < best_distance) {
									vec4 albedo_alpha = vec4(0.0, 0.0, 0.0, -1.0);
									if (triangle.alpha_scissor_threshold >= 0.0) {
										albedo_alpha = apply_triangle_alpha_scissor(triangle_index, sample_triangle_albedo_alpha(triangle_index, barycentric));
										if (albedo_alpha.a == 0.0) {
											continue;
										}
									}
									if (p_sample_albedo) {
										r_albedo_alpha = albedo_alpha;
									}
									switch (triangle.cull_mode) {
										case CULL_DISABLED:
											backface = false;
											break;
										case CULL_FRONT:
											backface = !backface;
											break;
										case CULL_BACK: // Default behavior.
											break;
									}

									hit = backface ? RAY_BACK : RAY_FRONT;
									best_distance = distance;
									r_distance = distance;
									r_normal = normalize(normal);
									r_triangle = triangle_index;
									r_barycentric = barycentric;
								}
							}
						}

#ifdef CLUSTER_TRIANGLE_ITERATION
						triangle_base_index += 32;
#endif
					}
				}

				cluster_base_index += 32;
			}

			if (hit != RAY_MISS) {
				if (p_sample_albedo && r_albedo_alpha.a < 0.0) {
					// Alpha scissor hits already carry the sample used for the coverage test.
					r_albedo_alpha = sample_triangle_albedo_alpha(r_triangle, r_barycentric);
				}
				return hit;
			}
		}

		if (icell == iendcell) {
			break;
		}

		cell_entry_distance = min(t_max.x, min(t_max.y, t_max.z));
		if (cell_entry_distance > rel_len) {
			break;
		}

		// There should be only one axis updated at a time for DDA to work properly.
		if (t_max.x < t_max.y && t_max.x < t_max.z) {
			icell.x += step.x;
			t_max.x += delta.x;
		} else if (t_max.y < t_max.z) {
			icell.y += step.y;
			t_max.y += delta.y;
		} else {
			icell.z += step.z;
			t_max.z += delta.z;
		}
		iters++;
	}

	return RAY_MISS;
}

#endif // USE_HARDWARE_RAYTRACING

uint trace_ray_closest_hit_triangle(vec3 p_from, vec3 p_to, out uint r_triangle, out vec3 r_barycentric, out vec4 r_albedo_alpha) {
	float distance;
	vec3 normal;
	return trace_ray(p_from, p_to, false, true, distance, normal, r_triangle, r_barycentric, r_albedo_alpha);
}

uint trace_ray_closest_hit_triangle_albedo_alpha(vec3 p_from, vec3 p_to, out vec4 albedo_alpha, out vec3 hit_position) {
	float distance;
	vec3 normal;
	uint tidx;
	vec3 barycentric;

	uint ret = trace_ray(p_from, p_to, false, true, distance, normal, tidx, barycentric, albedo_alpha);
	if (ret != RAY_MISS) {
		Vertex vert0 = vertices.data[triangles.data[tidx].indices.x];
		Vertex vert1 = vertices.data[triangles.data[tidx].indices.y];
		Vertex vert2 = vertices.data[triangles.data[tidx].indices.z];

		hit_position = barycentric.x * vert0.position + barycentric.y * vert1.position + barycentric.z * vert2.position;
	}

	return ret;
}

uint trace_ray_closest_hit_distance(vec3 p_from, vec3 p_to, out float r_distance, out vec3 r_normal) {
	uint triangle;
	vec3 barycentric;
	vec4 albedo_alpha;
	return trace_ray(p_from, p_to, false, false, r_distance, r_normal, triangle, barycentric, albedo_alpha);
}

uint trace_ray_any_hit(vec3 p_from, vec3 p_to) {
	float distance;
	vec3 normal;
	uint triangle;
	vec3 barycentric;
	vec4 albedo_alpha;
	return trace_ray(p_from, p_to, true, false, distance, normal, triangle, barycentric, albedo_alpha);
}

// https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
uint hash(uint value) {
	uint state = value * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

uint random_seed(ivec3 seed) {
	return hash(seed.x ^ hash(seed.y ^ hash(seed.z)));
}

// generates a random value in range [0.0, 1.0)
float randomize(inout uint value) {
	value = hash(value);
	return float(value / 4294967296.0);
}

const float PI = 3.14159265f;

// http://www.realtimerendering.com/raytracinggems/unofficial_RayTracingGems_v1.4.pdf (chapter 15)
vec3 generate_hemisphere_cosine_weighted_direction(vec2 p_rand) {
	float noise1 = p_rand.x;
	float noise2 = p_rand.y * 2.0 * PI;

	return vec3(sqrt(noise1) * cos(noise2), sqrt(noise1) * sin(noise2), sqrt(1.0 - noise1));
}

vec3 generate_hemisphere_cosine_weighted_direction(inout uint noise) {
	return generate_hemisphere_cosine_weighted_direction(vec2(randomize(noise), randomize(noise)));
}

// R2 low-discrepancy sequence, offset by p_rotation (Cranley-Patterson) so each texel gets a decorrelated but still stratified set.
// https://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/
vec2 r2_sequence(uint p_index, vec2 p_rotation) {
	const vec2 alpha = vec2(0.7548776662466927, 0.5698402909980532);
	return fract(p_rotation + alpha * float(p_index));
}

// Shuffled, Owen-scrambled 2D Sobol, from Brent Burley, "Practical Hash-based Owen Scrambling" (JCGT 2020).
// Each (p_seed) gives an independent, still stratified 2D sequence over p_index, used for path decisions after the first ray.
uint laine_karras_permutation(uint p_x, uint p_seed) {
	p_x += p_seed;
	p_x ^= p_x * 0x6c50b47cu;
	p_x ^= p_x * 0xb82f1e52u;
	p_x ^= p_x * 0xc7afe638u;
	p_x ^= p_x * 0x8d22f6e6u;
	return p_x;
}

uint nested_uniform_scramble(uint p_x, uint p_seed) {
	return bitfieldReverse(laine_karras_permutation(bitfieldReverse(p_x), p_seed));
}

uint sobol_dimension_1(uint p_index) {
	uint result = 0u;
	uint v = 1u << 31;
	for (uint index = p_index; index != 0u; index >>= 1u) {
		if ((index & 1u) != 0u) {
			result ^= v;
		}
		v ^= v >> 1u;
	}
	return result;
}

vec2 sobol_owen_2d(uint p_index, uint p_seed) {
	uint index = nested_uniform_scramble(p_index, p_seed);
	uint x = nested_uniform_scramble(bitfieldReverse(index), hash(p_seed ^ 0xa511e9b3u));
	uint y = nested_uniform_scramble(sobol_dimension_1(index), hash(p_seed ^ 0x63d83595u));
	return vec2(x >> 8u, y >> 8u) / 16777216.0; // 24 bits keep the result strictly below 1.0 in float.
}

#ifdef MODE_LIGHT_PROBES
// A uniform random quaternion gives the complete Fibonacci sphere a different
// orientation per probe without changing its even distribution.
vec4 generate_probe_rotation(inout uint noise) {
	float u = randomize(noise);
	float a = 2.0 * PI * randomize(noise);
	float b = 2.0 * PI * randomize(noise);
	return vec4(sqrt(1.0 - u) * vec2(sin(a), cos(a)), sqrt(u) * vec2(sin(b), cos(b)));
}

vec3 generate_probe_direction(uint sample_index, uint sample_count, vec4 rotation) {
	float z = 1.0 - 2.0 * (float(sample_index) + 0.5) / float(sample_count);
	float radius = sqrt(max(0.0, 1.0 - z * z));
	float angle = 2.39996323 * float(sample_index); // Golden angle.
	vec3 direction = vec3(radius * cos(angle), radius * sin(angle), z);
	vec3 t = 2.0 * cross(rotation.xyz, direction);
	return direction + rotation.w * t + cross(rotation.xyz, t);
}
#endif

vec3 generate_ray_dir_from_normal(vec3 normal, vec2 p_rand) {
	vec3 v0 = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
	vec3 tangent = normalize(cross(v0, normal));
	vec3 bitangent = normalize(cross(tangent, normal));
	mat3 normal_mat = mat3(tangent, bitangent, normal);
	return normal_mat * generate_hemisphere_cosine_weighted_direction(p_rand);
}

vec3 generate_ray_dir_from_normal(vec3 normal, inout uint noise) {
	return generate_ray_dir_from_normal(normal, vec2(randomize(noise), randomize(noise)));
}

#if defined(MODE_DIRECT_LIGHT) || defined(MODE_BOUNCE_LIGHT) || defined(MODE_LIGHT_PROBES)

float get_omni_attenuation(float distance, float inv_range, float decay, float fade_start) {
	return light_range_fade(distance, inv_range, fade_start) * pow(max(distance, 0.0001), -decay);
}

const int AA_SAMPLES = 16;

const vec2 halton_map[AA_SAMPLES] = vec2[](
		vec2(0.5, 0.33333333),
		vec2(0.25, 0.66666667),
		vec2(0.75, 0.11111111),
		vec2(0.125, 0.44444444),
		vec2(0.625, 0.77777778),
		vec2(0.375, 0.22222222),
		vec2(0.875, 0.55555556),
		vec2(0.0625, 0.88888889),
		vec2(0.5625, 0.03703704),
		vec2(0.3125, 0.37037037),
		vec2(0.8125, 0.7037037),
		vec2(0.1875, 0.14814815),
		vec2(0.6875, 0.48148148),
		vec2(0.4375, 0.81481481),
		vec2(0.9375, 0.25925926),
		vec2(0.03125, 0.59259259));

vec2 get_vogel_disk(float p_i, float p_rotation, float p_sample_count_sqrt) {
	const float golden_angle = 2.4;

	float r = sqrt(p_i + 0.5) / p_sample_count_sqrt;
	float theta = p_i * golden_angle + p_rotation;

	return vec2(cos(theta), sin(theta)) * r;
}

vec3 projector_srgb_to_linear(vec3 p_color) {
	return mix(
			pow((p_color + vec3(0.055)) * (1.0 / 1.055), vec3(2.4)),
			p_color * (1.0 / 12.92),
			lessThan(p_color, vec3(0.04045)));
}

void trace_direct_light(vec3 p_position, vec3 p_normal, vec3 p_geometry_normal, uint p_light_index, bool p_soft_shadowing, out vec3 r_light, out vec3 r_light_dir, inout uint r_noise, float p_texel_size, out float r_shadow) {
	const float EPSILON = 0.00001;

	r_light = vec3(0.0f);
	r_shadow = 0.0f;

	vec3 light_pos;
	float dist;
	float attenuation;
	float soft_shadowing_disk_size;
	vec3 light_texture_color = vec3(1.0);
	vec3 shadow_dir;
	Light light_data = lights.data[p_light_index];
	if (light_data.type == LIGHT_TYPE_DIRECTIONAL) {
		vec3 light_vec = light_data.direction;
		light_pos = p_position - light_vec * length(bake_params.world_size);
		r_light_dir = normalize(light_pos - p_position);
		shadow_dir = r_light_dir;
		dist = length(bake_params.world_size);
		attenuation = 1.0;
		attenuation *= max(0.0, dot(p_normal, r_light_dir));
		soft_shadowing_disk_size = light_data.size;
	} else if (light_data.type == LIGHT_TYPE_AREA) {
		r_light_dir = vec3(0.0); // has to be initialized!
		if (dot(light_data.direction, p_position - light_data.position) <= 0) {
			return;
		}
		vec3 area_width = light_data.area_width.xyz;
		vec3 area_height = light_data.area_height.xyz;
		vec3 h_area_width = area_width / 2.0;
		vec3 h_area_height = area_height / 2.0;
		vec3 area_width_norm = normalize(area_width);
		vec3 area_height_norm = normalize(area_height);
		float a_half_len = length(area_width) / 2.0;
		float b_half_len = length(area_height) / 2.0;

		vec3 points[4];
		points[0] = light_data.position - h_area_width - h_area_height - p_position;
		points[1] = light_data.position + h_area_width - h_area_height - p_position;
		points[2] = light_data.position + h_area_width + h_area_height - p_position;
		points[3] = light_data.position - h_area_width + h_area_height - p_position;

		float ltc_diffuse;
		ltc_evaluate_diff(p_normal, points, light_data.area_texture_rect, light_data.cos_spot_angle, area_light_atlas, area_light_atlas_sampler, ltc_diffuse, light_texture_color);

		vec3 light_to_vert = p_position - light_data.position;
		vec3 pos_local_to_light = vec3(dot(light_to_vert, area_width_norm), dot(light_to_vert, area_height_norm), dot(light_to_vert, -light_data.direction)); // p_position in LIGHT SPACE

		vec3 closest_point_local_to_light = vec3(clamp(pos_local_to_light.x, -a_half_len, a_half_len), clamp(pos_local_to_light.y, -b_half_len, b_half_len), 0);
		dist = max(0.0001, distance(closest_point_local_to_light, pos_local_to_light));
		if (dist > light_data.range) {
			return;
		}
		// set light pos to closest point
		light_pos = light_data.position;
		vec3 closest_point = light_data.position + closest_point_local_to_light.x * area_width_norm + closest_point_local_to_light.y * area_height_norm;
		r_light_dir = normalize(closest_point - p_position);
		shadow_dir = normalize(light_pos - p_position);
		attenuation = get_omni_attenuation(dist, 1.0 / light_data.range, light_data.attenuation, light_data.range_fade_start) * dist * dist; // LTC integral already decreases by inverse square, so attenuation power is 2.0 by default -> subtract 2.0
		attenuation *= ltc_diffuse;
		soft_shadowing_disk_size = light_data.size / dist;
	} else {
		light_pos = light_data.position;
		r_light_dir = normalize(light_pos - p_position);
		shadow_dir = r_light_dir;
		dist = distance(p_position, light_pos);
		if (dist > light_data.range) {
			return;
		}

		soft_shadowing_disk_size = light_data.size / dist;

		attenuation = light_data.type == LIGHT_TYPE_OMNI ? omni_light_attenuation(dist, 1.0 / light_data.range, light_data.attenuation, light_data.range_fade_start) : get_omni_attenuation(dist, 1.0 / light_data.range, light_data.attenuation, light_data.range_fade_start);

		if (light_data.type == LIGHT_TYPE_SPOT) {
			vec3 rel = normalize(p_position - light_pos);
			float cos_spot_angle = light_data.cos_spot_angle;
			float cos_angle = dot(rel, light_data.direction);

			if (cos_angle < cos_spot_angle) {
				return;
			}

			attenuation *= spot_light_attenuation(cos_angle, cos_spot_angle, light_data.cos_spot_inner_angle, light_data.inv_spot_attenuation);
		}

		if (light_data.projector_rect.z > 0.0 && light_data.projector_rect.w > 0.0) {
			vec3 light_to_point = p_position - light_pos;
			vec2 projector_uv;

			if (light_data.type == LIGHT_TYPE_SPOT) {
				float projector_depth = dot(light_to_point, light_data.direction);
				float tan_spot_angle = sqrt(max(0.0, 1.0 - light_data.cos_spot_angle * light_data.cos_spot_angle)) / light_data.cos_spot_angle;
				projector_uv = vec2(0.5) + vec2(dot(light_to_point, light_data.projector_x.xyz), -dot(light_to_point, light_data.projector_y.xyz)) / (2.0 * projector_depth * tan_spot_angle);
				projector_uv = light_data.projector_rect.xy + projector_uv * light_data.projector_rect.zw;
			} else {
				vec3 local_v = normalize(vec3(
						dot(light_to_point, light_data.projector_x.xyz),
						dot(light_to_point, light_data.projector_y.xyz),
						dot(light_to_point, light_data.projector_z.xyz)));
				vec2 hemisphere_offset = vec2(0.0);
				if (local_v.z >= 0.0) {
					hemisphere_offset.y = light_data.projector_rect.w * 0.5;
				}
				local_v.z = 1.0 + abs(local_v.z);
				projector_uv = local_v.xy / local_v.z;
				projector_uv = projector_uv * 0.5 + 0.5;
				projector_uv = light_data.projector_rect.xy + hemisphere_offset +
						projector_uv * vec2(light_data.projector_rect.z, light_data.projector_rect.w * 0.5);
			}

			vec2 atlas_texel_size = 1.0 / vec2(textureSize(sampler2D(area_light_atlas, area_light_atlas_sampler), 0));
			vec2 projector_min = light_data.projector_rect.xy + atlas_texel_size * 0.5;
			vec2 projector_max = light_data.projector_rect.xy + light_data.projector_rect.zw - atlas_texel_size * 0.5;
			projector_uv = clamp(projector_uv, projector_min, projector_max);
			vec4 projector_color = textureLod(sampler2D(area_light_atlas, area_light_atlas_sampler), projector_uv, 0.0);
			light_texture_color = projector_srgb_to_linear(projector_color.rgb) * projector_color.a;
		}
		attenuation *= max(0.0, dot(p_normal, r_light_dir));
	}

	if (attenuation * light_data.energy <= 0.0001 || (light_data.type != LIGHT_TYPE_AREA && dot(p_geometry_normal, r_light_dir) <= 0.0)) {
		return;
	}

	float penumbra = 0.0;
	vec3 penumbra_color = vec3(0.0);
	if (p_soft_shadowing) {
		// Area lights always sample their rectangle, their size comes from area_width/area_height, not light_data.size.
		const bool is_area = light_data.type == LIGHT_TYPE_AREA;
		const bool use_soft_shadows = (light_data.size > 0.0) || is_area;
		const vec2 area_rotation = is_area ? vec2(randomize(r_noise), randomize(r_noise)) : vec2(0.0);
		const uint ray_count = AA_SAMPLES;
		const uint total_ray_count = use_soft_shadows ? params.ray_count : ray_count;
		const uint shadowing_rays_check_penumbra_denom = 2;
		const uint shadowing_ray_count = max(1, params.ray_count / ray_count);
		const float shadowing_ray_count_sqrt = sqrt(float(total_ray_count));

		// Setup tangent pass to calculate AA samples over the current texel.
		vec3 aux = abs(p_geometry_normal.y) < 0.777 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		vec3 tangent = normalize(cross(p_geometry_normal, aux));
		vec3 bitan = normalize(cross(p_geometry_normal, tangent));

		// Setup light tangent pass to calculate samples over disk aligned towards the light
		vec3 light_to_point = -shadow_dir;
		vec3 light_aux = abs(light_to_point.y) < 0.777 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		vec3 light_to_point_tan = normalize(cross(light_to_point, light_aux));
		vec3 light_to_point_bitan = normalize(cross(light_to_point, light_to_point_tan));

		float aa_power = 0.0;
		for (uint i = 0; i < ray_count; i++) {
			// Create a random sample within the texel.
			vec2 disk_sample = (halton_map[i] - vec2(0.5)) * p_texel_size * light_data.shadow_blur;
			// Align the sample to world space.
			vec3 disk_aligned = (disk_sample.x * tangent + disk_sample.y * bitan);
			vec3 origin = p_position - disk_aligned;
			vec3 light_dir = normalize(light_pos - origin);
			// Keep the jitter and both ray bias offsets on the same side of nearby surfaces.
			if (trace_ray_any_hit(p_position, origin + light_dir * bake_params.bias * 2.0) != RAY_MISS) {
				origin = p_position;
				light_dir = normalize(light_pos - origin);
			}

			float power = 0.0;
			vec3 light_color = vec3(0.0);
			uint power_accm = 0;
			vec3 prev_pos = origin;
			if (use_soft_shadows) {
				uint soft_shadow_hits = 0;
				for (uint j = 0; j < shadowing_ray_count; j++) {
					origin = prev_pos;
					// Optimization:
					// Once already traced an important proportion of rays, if all are hits or misses,
					// assume we're not in the penumbra so we can infer the rest would have the same result.
					if (!is_area && j == shadowing_ray_count / shadowing_rays_check_penumbra_denom) {
						if (soft_shadow_hits == j) {
							// Assume totally lit
							soft_shadow_hits = shadowing_ray_count;
							break;
						} else if (soft_shadow_hits == 0) {
							// Assume totally dark
							soft_shadow_hits = 0;
							break;
						}
					}

					vec3 light_disk_to_point;
					vec3 shadow_ray_end;
					if (is_area) {
						// Target points across the actual rectangle so the penumbra follows the light's shape.
						vec2 rect_sample = r2_sequence(i * shadowing_ray_count + j, area_rotation) - vec2(0.5);
						vec3 target = light_data.position + rect_sample.x * light_data.area_width.xyz + rect_sample.y * light_data.area_height.xyz;
						light_disk_to_point = normalize(origin - target);
						if (dot(p_geometry_normal, -light_disk_to_point) <= 0.0) {
							continue; // Part of the light below the surface, the LTC integral already excludes it.
						}
						shadow_ray_end = target + light_disk_to_point * bake_params.bias;
					} else {
						float a = randomize(r_noise) * 2.0 * PI;
						float vogel_index = float(total_ray_count - 1 - (i * shadowing_ray_count + j)); // Start from (total_ray_count - 1) so we check the outer points first.
						vec2 light_disk_sample = get_vogel_disk(vogel_index, a, shadowing_ray_count_sqrt) * soft_shadowing_disk_size * light_data.shadow_blur;
						light_disk_to_point = normalize(light_to_point + light_disk_sample.x * light_to_point_tan + light_disk_sample.y * light_to_point_bitan);
						shadow_ray_end = p_position - light_disk_to_point * dist;
					}
					float sample_penumbra = 0.0;
					vec3 sample_penumbra_color = light_data.color.rgb * light_texture_color;
					bool sample_did_hit = false;

					for (uint iter = 0; iter < bake_params.transparency_rays; iter++) {
						vec4 hit_albedo = vec4(1.0);
						vec3 hit_position;
						// Offset the ray origin for AA, offset the light position for soft shadows.
						uint ret = trace_ray_closest_hit_triangle_albedo_alpha(origin - light_disk_to_point * bake_params.bias, shadow_ray_end, hit_albedo, hit_position);
						if (ret == RAY_MISS) {
							if (!sample_did_hit) {
								sample_penumbra = 1.0;
							}
							soft_shadow_hits += 1;
							break;
						} else if (ret == RAY_FRONT || ret == RAY_BACK) {
							bool contribute = ret == RAY_FRONT || !sample_did_hit;
							if (!sample_did_hit) {
								sample_penumbra = 1.0;
								sample_did_hit = true;
							}

							soft_shadow_hits += 1;

							if (contribute) {
								sample_penumbra_color = mix(sample_penumbra_color, sample_penumbra_color * hit_albedo.rgb, hit_albedo.a);
								sample_penumbra *= 1.0 - hit_albedo.a;
							}
							origin = hit_position + shadow_dir * bake_params.bias;

							if (sample_penumbra - EPSILON <= 0) {
								break;
							}
						}
					}

					power += sample_penumbra;
					light_color += sample_penumbra_color;
					power_accm++;
				}

			} else { // No soft shadows (size == 0).
				float sample_penumbra = 0.0;
				vec3 sample_penumbra_color = light_data.color.rgb * light_texture_color;
				bool sample_did_hit = false;
				for (uint iter = 0; iter < bake_params.transparency_rays; iter++) {
					vec4 hit_albedo = vec4(1.0);
					vec3 hit_position;
					// Offset the ray origin for AA, offset the light position for soft shadows.
					uint ret = trace_ray_closest_hit_triangle_albedo_alpha(origin + light_dir * bake_params.bias, light_pos, hit_albedo, hit_position);
					if (ret == RAY_MISS) {
						if (!sample_did_hit) {
							sample_penumbra = 1.0;
						}
						break;
					} else if (ret == RAY_FRONT || ret == RAY_BACK) {
						bool contribute = ret == RAY_FRONT || !sample_did_hit;
						if (!sample_did_hit) {
							sample_penumbra = 1.0;
							sample_did_hit = true;
						}

						if (contribute) {
							sample_penumbra_color = mix(sample_penumbra_color, sample_penumbra_color * hit_albedo.rgb, hit_albedo.a);
							sample_penumbra *= 1.0 - hit_albedo.a;
						}
						origin = hit_position + shadow_dir * bake_params.bias;

						if (sample_penumbra - EPSILON <= 0) {
							break;
						}
					}
				}
				power = sample_penumbra;
				light_color = sample_penumbra_color;
				power_accm = 1;
			}
			if (power_accm == 0u) {
				// Every area light sample was below the horizon, LTC already makes this contribution ~0.
				power = 1.0;
				light_color = light_data.color.rgb * light_texture_color;
				power_accm = 1;
			}
			aa_power += power / float(power_accm);
			penumbra_color += light_color / float(power_accm);
		}
		penumbra = aa_power / ray_count;
		penumbra_color /= ray_count;
	} else { // No soft shadows and anti-aliasing (disabled via parameter).
#ifdef USE_BINARY_SHADOWS
		// No partial transmission is possible, so the nearest blocker and its color are irrelevant.
		penumbra = 0.0;
		penumbra_color = light_data.color.rgb * light_texture_color;
		if (bake_params.transparency_rays > 0 && trace_ray_any_hit(p_position + shadow_dir * bake_params.bias, light_pos) == RAY_MISS) {
			penumbra = 1.0;
		}
#else
		bool did_hit = false;
		penumbra = 0.0;
		penumbra_color = light_data.color.rgb * light_texture_color;
		for (uint iter = 0; iter < bake_params.transparency_rays; iter++) {
			vec4 hit_albedo = vec4(1.0);
			vec3 hit_position;
			uint ret = trace_ray_closest_hit_triangle_albedo_alpha(p_position + shadow_dir * bake_params.bias, light_pos, hit_albedo, hit_position);
			if (ret == RAY_MISS) {
				if (!did_hit) {
					penumbra = 1.0;
				}
				break;
			} else if (ret == RAY_FRONT || ret == RAY_BACK) {
				bool contribute = (ret == RAY_FRONT || !did_hit);
				if (!did_hit) {
					penumbra = 1.0;
					did_hit = true;
				}

				if (contribute) {
					penumbra_color = mix(penumbra_color, penumbra_color * hit_albedo.rgb, hit_albedo.a);
					penumbra *= 1.0 - hit_albedo.a;
				}

				p_position = hit_position + shadow_dir * bake_params.bias;

				if (penumbra - EPSILON <= 0) {
					break;
				}
			}
		}

		penumbra = clamp(penumbra, 0.0, 1.0);
#endif // USE_BINARY_SHADOWS
	}

	r_shadow = penumbra;
	r_light = light_data.energy * attenuation * penumbra * penumbra_color;
}

#endif

#if defined(MODE_BOUNCE_LIGHT) || defined(MODE_LIGHT_PROBES)

vec2 environment_dir_to_uv(vec3 ray_dir) {
	vec3 sky_dir = normalize(mat3(bake_params.env_transform) * ray_dir);
	vec2 st = vec2(atan(sky_dir.x, sky_dir.z) - PI, acos(clamp(sky_dir.y, -1.0, 1.0)));
	if (st.x < 0.0) {
		st.x += PI * 2.0;
	}
	return st / vec2(PI * 2.0, PI);
}

vec3 trace_environment_color(vec3 ray_dir) {
	return textureLod(sampler2D(environment, linear_sampler), environment_dir_to_uv(ray_dir), 0.0).rgb * bake_params.environment_exposure_multiplier;
}

// Solid angle pdf of sample_environment() for a direction.
float environment_pdf(vec3 ray_dir) {
	if (bake_params.env_cdf_width == 0u) {
		return 0.0;
	}
	uvec2 size = uvec2(bake_params.env_cdf_width, bake_params.env_cdf_height);
	vec2 uv = environment_dir_to_uv(ray_dir);
	uvec2 cell = min(uvec2(uv * vec2(size)), size - 1u);
	float sin_theta = sin(uv.y * PI);
	return sin_theta > 0.0 ? env_cdf.data[cell.y * size.x + cell.x] / (2.0 * PI * PI * sin_theta) : 0.0;
}

// Returns the first index in [p_offset, p_offset + p_count) whose CDF value exceeds p_value.
uint environment_cdf_search(uint p_offset, uint p_count, float p_value) {
	uint lo = 0u;
	uint hi = p_count - 1u;
	while (lo < hi) {
		uint mid = (lo + hi) / 2u;
		if (env_cdf.data[p_offset + mid] > p_value) {
			hi = mid;
		} else {
			lo = mid + 1u;
		}
	}
	return lo;
}

// Picks a direction proportionally to environment luminance. Inverse of environment_dir_to_uv().
vec3 sample_environment(vec2 p_rand, out float r_pdf) {
	uint w = bake_params.env_cdf_width;
	uint h = bake_params.env_cdf_height;
	uint marginal_ofs = w * h;
	uint row = environment_cdf_search(marginal_ofs, h, p_rand.y);
	float row_lo = row > 0u ? env_cdf.data[marginal_ofs + row - 1u] : 0.0;
	float dv = (p_rand.y - row_lo) / max(env_cdf.data[marginal_ofs + row] - row_lo, 1e-8);

	uint row_ofs = marginal_ofs + h + row * w;
	uint col = environment_cdf_search(row_ofs, w, p_rand.x);
	float col_lo = col > 0u ? env_cdf.data[row_ofs + col - 1u] : 0.0;
	float du = (p_rand.x - col_lo) / max(env_cdf.data[row_ofs + col] - col_lo, 1e-8);

	vec2 uv = (vec2(col, row) + clamp(vec2(du, dv), 0.0, 1.0)) / vec2(w, h);
	float theta = uv.y * PI;
	float phi = uv.x * 2.0 * PI + PI;
	float sin_theta = sin(theta);
	r_pdf = sin_theta > 0.0 ? env_cdf.data[row * w + col] / (2.0 * PI * PI * sin_theta) : 0.0;
	vec3 sky_dir = vec3(sin_theta * sin(phi), cos(theta), sin_theta * cos(phi));
	// shortcut: transpose assumes env_transform is a pure rotation, use inverse() if scaled skies are ever needed.
	return normalize(transpose(mat3(bake_params.env_transform)) * sky_dir);
}

float mis_power_heuristic(float p_pdf, float p_other_pdf) {
	float a = p_pdf * p_pdf;
	return a / max(a + p_other_pdf * p_other_pdf, 1e-20);
}

// Next event estimation towards the environment from a surface. The result is MIS weighted against
// cosine sampling and already divided by the cosine pdf, so it adds directly to cosine-sampled radiance.
vec3 sample_environment_light(vec3 p_position, vec3 p_normal, vec3 p_geometry_normal, vec2 p_rand, out vec3 r_dir) {
	r_dir = p_normal;
	if (bake_params.env_cdf_width == 0u) {
		return vec3(0.0);
	}
	float env_pdf;
	r_dir = sample_environment(p_rand, env_pdf);
	float cos_theta = dot(p_normal, r_dir);
	if (env_pdf <= 0.0 || cos_theta <= 0.0 || dot(p_geometry_normal, r_dir) <= 0.0) {
		return vec3(0.0);
	}
	if (trace_ray_any_hit(p_position + r_dir * bake_params.bias, p_position + r_dir * length(bake_params.world_size)) != RAY_MISS) {
		return vec3(0.0);
	}
	float cosine_pdf = cos_theta / PI;
	return trace_environment_color(r_dir) * mis_power_heuristic(env_pdf, cosine_pdf) * cosine_pdf / env_pdf;
}

vec3 clamp_indirect_sample(vec3 p_light) {
	if (bake_params.indirect_sample_clamp > 0.0) {
		float luminance = dot(p_light, vec3(0.2126, 0.7152, 0.0722));
		if (luminance > bake_params.indirect_sample_clamp) {
			p_light *= bake_params.indirect_sample_clamp / luminance;
		}
	}
	return p_light;
}

// p_origin_nee: the caller already sampled the environment from p_position with normal p_origin_normal,
// so environment hits of the first ray must be MIS weighted.
// p_sample_index and p_path_seed pick the stratified samples for every path vertex, so paths of the same texel stay stratified past the first ray.
vec3 trace_indirect_light(vec3 p_position, vec3 p_ray_dir, inout uint r_noise, float p_texel_size, bool p_origin_nee, vec3 p_origin_normal, uint p_sample_index, uint p_path_seed, out bool r_near_backface) {
	// The lower limit considers the case where the lightmapper might have bounces disabled but light probes are requested.
	vec3 position = p_position;
	vec3 ray_dir = p_ray_dir;
	uint max_depth = max(bake_params.bounces, 1);
	uint transparency_rays_left = bake_params.transparency_rays;
	vec3 throughput = vec3(1.0);
	vec3 light = vec3(0.0);
	// Whether the environment was explicitly sampled from the vertex the current ray left, and that vertex's normal.
	bool vertex_nee = p_origin_nee;
	vec3 vertex_normal = p_origin_normal;
	r_near_backface = false;
	uint path_vertex = 0u; // Unlike depth, also advances on transparent hits so every decision gets its own dimensions.
	for (uint depth = 0; depth < max_depth; depth++) {
		path_vertex++;
		uint vertex_seed = hash(p_path_seed ^ hash(path_vertex));
		uint tidx;
		vec3 barycentric;
		float hit_distance;
		vec3 hit_normal;
		vec4 albedo_alpha;
		uint trace_result = trace_ray(position + ray_dir * bake_params.bias, position + ray_dir * length(bake_params.world_size), false, true, hit_distance, hit_normal, tidx, barycentric, albedo_alpha);
		if (depth == 0 && trace_result == RAY_BACK && p_texel_size > 0.0 && hit_distance <= max(p_texel_size * 2.0, bake_params.bias * 4.0)) {
			r_near_backface = true;
		}
		if (trace_result == RAY_FRONT) {
			Vertex vert0 = vertices.data[triangles.data[tidx].indices.x];
			Vertex vert1 = vertices.data[triangles.data[tidx].indices.y];
			Vertex vert2 = vertices.data[triangles.data[tidx].indices.z];
			barycentric = inset_barycentric(barycentric, vert0.position, vert1.position, vert2.position, bake_params.bias);
			vec3 uvw = vec3(barycentric.x * vert0.uv + barycentric.y * vert1.uv + barycentric.z * vert2.uv, float(triangles.data[tidx].slice));
			position = barycentric.x * vert0.position + barycentric.y * vert1.position + barycentric.z * vert2.position;

			vec3 prev_normal = ray_dir;

			vec3 norm0 = vec3(vert0.normal_xy, vert0.normal_z);
			vec3 norm1 = vec3(vert1.normal_xy, vert1.normal_z);
			vec3 norm2 = vec3(vert2.normal_xy, vert2.normal_z);
			vec3 normal = normalize(barycentric.x * norm0 + barycentric.y * norm1 + barycentric.z * norm2);
			vec3 geometry_normal = -normalize(cross(vert1.position - vert0.position, vert2.position - vert0.position));

			vec3 direct_light = vec3(0.0f);
#ifdef USE_LIGHT_TEXTURE_FOR_BOUNCES
			direct_light += textureLod(sampler2DArray(source_light, linear_sampler), uvw, 0.0).rgb;
#else
			// Trace the lights directly. Significantly more expensive but more accurate in scenarios
			// where the lightmap texture isn't reliable.
			for (uint i = 0; i < bake_params.light_count; i++) {
				vec3 light;
				vec3 light_dir;
				float shadow;
				trace_direct_light(position, normal, geometry_normal, i, false, light, light_dir, r_noise, p_texel_size, shadow);
				direct_light += light * lights.data[i].indirect_energy;
			}

			direct_light *= bake_params.exposure_normalization;
#endif

			vec3 emissive = textureLod(sampler2DArray(emission_tex, linear_sampler), uvw, 0).rgb;

			light += throughput * emissive * albedo_alpha.a;
			throughput = mix(throughput, throughput * albedo_alpha.rgb, albedo_alpha.a);
			light += throughput * direct_light * bake_params.bounce_indirect_energy * albedo_alpha.a;

			// Only the texel samples the environment explicitly. Each bounce vertex would cost another full-length
			// shadow ray for little gain, so rays leaving it find the sky by cosine sampling alone, at full weight.
			vertex_nee = false;

			if (albedo_alpha.a < 1.0) {
				transparency_rays_left -= 1;
				depth -= 1;
				if (transparency_rays_left <= 0) {
					break;
				}

				// Either bounce off the transparent surface or keep going forward.
				float pa = albedo_alpha.a * albedo_alpha.a;
				if (randomize(r_noise) > pa) {
					normal = prev_normal;
				}

				position += normal * bake_params.bias;
			}

			// Use Russian Roulette to determine a probability to terminate the bounce earlier as an optimization.
			// <https://computergraphics.stackexchange.com/questions/2316/is-russian-roulette-really-the-answer>
			float p = max(max(throughput.x, throughput.y), throughput.z);
			if (sobol_owen_2d(p_sample_index, vertex_seed ^ 0x9e3779b9u).x > p) {
				break;
			}

			// Boost the throughput from the probability of the ray being terminated early.
			throughput *= 1.0 / p;

			// Generate a new ray direction for the next bounce from this surface's normal.
			ray_dir = generate_ray_dir_from_normal(normal, sobol_owen_2d(p_sample_index, vertex_seed ^ 0x51ed270bu));
			if (dot(geometry_normal, ray_dir) <= 0.0 && albedo_alpha.a >= 1.0) {
				break;
			}
		} else if (trace_result == RAY_MISS) {
			// Look for the environment color and stop bouncing.
			float miss_weight = vertex_nee ? mis_power_heuristic(max(dot(vertex_normal, ray_dir), 0.0) / PI, environment_pdf(ray_dir)) : 1.0;
			light += throughput * trace_environment_color(ray_dir) * miss_weight;
			break;
		} else if (trace_result == RAY_BACK) {
			Vertex vert0 = vertices.data[triangles.data[tidx].indices.x];
			Vertex vert1 = vertices.data[triangles.data[tidx].indices.y];
			Vertex vert2 = vertices.data[triangles.data[tidx].indices.z];
			vec3 uvw = vec3(barycentric.x * vert0.uv + barycentric.y * vert1.uv + barycentric.z * vert2.uv, float(triangles.data[tidx].slice));
			position = barycentric.x * vert0.position + barycentric.y * vert1.position + barycentric.z * vert2.position;

			if (albedo_alpha.a >= 1.0) {
				break;
			}

			// The environment shadow ray from the previous vertex was blocked by this surface, so a later miss gets full weight.
			vertex_nee = false;
			transparency_rays_left -= 1;
			depth -= 1;
			if (transparency_rays_left <= 0) {
				break;
			}

			vec3 norm0 = vec3(vert0.normal_xy, vert0.normal_z);
			vec3 norm1 = vec3(vert1.normal_xy, vert1.normal_z);
			vec3 norm2 = vec3(vert2.normal_xy, vert2.normal_z);
			vec3 normal = normalize(barycentric.x * norm0 + barycentric.y * norm1 + barycentric.z * norm2);
			vec3 geometry_normal = -normalize(cross(vert1.position - vert0.position, vert2.position - vert0.position));

			vec3 direct_light = vec3(0.0f);
#ifdef USE_LIGHT_TEXTURE_FOR_BOUNCES
			direct_light += textureLod(sampler2DArray(source_light, linear_sampler), uvw, 0.0).rgb;
#else
			// Trace the lights directly. Significantly more expensive but more accurate in scenarios
			// where the lightmap texture isn't reliable.
			for (uint i = 0; i < bake_params.light_count; i++) {
				vec3 light;
				vec3 light_dir;
				float shadow;
				trace_direct_light(position, normal, geometry_normal, i, false, light, light_dir, r_noise, p_texel_size, shadow);
				direct_light += light * lights.data[i].indirect_energy;
			}

			direct_light *= bake_params.exposure_normalization;
#endif

			vec3 emissive = textureLod(sampler2DArray(emission_tex, linear_sampler), uvw, 0).rgb;

			light += throughput * emissive * albedo_alpha.a;
			throughput = mix(mix(throughput, throughput * albedo_alpha.rgb, albedo_alpha.a), vec3(0.0), albedo_alpha.a);
			light += throughput * direct_light * bake_params.bounce_indirect_energy * albedo_alpha.a;

			position += ray_dir * bake_params.bias;
		}
	}

	return light;
}

#endif

#endif // LM_RAY_TRACING_PASS

void main() {
	// Check if invocation is out of bounds.
#ifdef MODE_LIGHT_PROBES
	int probe_index = int(gl_GlobalInvocationID.x);
	if (probe_index >= params.probe_count) {
		return;
	}

#else
	ivec2 atlas_pos = ivec2(gl_GlobalInvocationID.xy) + params.region_ofs;
	if (any(greaterThanEqual(atlas_pos, bake_params.atlas_size))) {
		return;
	}
#endif

#ifdef MODE_PAD_OIDN
	const uint coefficient_count = params.ray_to;
	const ivec3 guide_pos = ivec3(atlas_pos, params.geometry_slice);
	vec4 result = texelFetch(sampler2DArray(source_light, linear_sampler), ivec3(atlas_pos, params.atlas_slice), 0);
	if (texelFetch(usampler2DArray(oidn_mesh_tex, linear_sampler), guide_pos, 0).r == 0u) {
		// The margin is reused per destination; mesh and lighting stay global.
		vec4 margin = texelFetch(sampler2DArray(oidn_margin_tex, linear_sampler), ivec3(atlas_pos, 0), 0);
		// Only extend validated seams; leave other empty texels unchanged.
		if (margin.w > 0.0) {
			// The margin provides a validated bilinear footprint or a valid texel center.
			float source_slice = margin.z * float(coefficient_count) + float(params.material_slice);
			result.rgb = textureLod(sampler2DArray(source_light, linear_sampler), vec3(margin.xy, source_slice), 0.0).rgb;
		}
	}
	// Preserve alpha: padding is denoising context, not valid lightmap coverage.
	imageStore(dest_light, ivec3(atlas_pos, params.output_slice), result);
#endif

#ifdef MODE_DIRECT_LIGHT
	vec3 normal = texelFetch(sampler2DArray(source_normal, linear_sampler), ivec3(atlas_pos, params.geometry_slice), 0).xyz;
	if (length(normal) < 0.5) {
		return; //empty texel, no process
	}
	vec3 position = texelFetch(sampler2DArray(source_position, linear_sampler), ivec3(atlas_pos, params.geometry_slice), 0).xyz;
	vec3 geometry_normal = texelFetch(sampler2DArray(source_geometry, linear_sampler), ivec3(atlas_pos, params.geometry_slice), 0).yzw;
	// Validate both coverage and bounds before deriving the shadow footprint.
	const ivec2 footprint_directions[4] = ivec2[4](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));
	float texel_size_world_space = 1e20;
	for (int direction = 0; direction < 4; direction++) {
		ivec2 neighbor_pos = atlas_pos + footprint_directions[direction];
		if (any(lessThan(neighbor_pos, ivec2(0))) || any(greaterThanEqual(neighbor_pos, bake_params.atlas_size))) {
			continue;
		}
		vec4 neighbor_position = texelFetch(sampler2DArray(source_position, linear_sampler), ivec3(neighbor_pos, params.geometry_slice), 0);
		float neighbor_distance = distance(position, neighbor_position.xyz);
		if (neighbor_position.a > 0.5 && neighbor_distance > 0.0) {
			texel_size_world_space = min(texel_size_world_space, neighbor_distance);
		}
	}
	if (texel_size_world_space == 1e20) {
		texel_size_world_space = bake_params.bias * 10.0;
	}
	texel_size_world_space *= bake_params.supersampling_factor;

	vec3 light_for_texture = vec3(0.0);
	vec3 light_for_bounces = vec3(0.0);

#ifdef USE_SHADOWMASK
	float shadowmask_value = 0.0f;
#endif

#ifdef USE_SH_LIGHTMAPS
	vec4 sh_accum[4] = vec4[](
			vec4(0.0, 0.0, 0.0, 1.0),
			vec4(0.0, 0.0, 0.0, 1.0),
			vec4(0.0, 0.0, 0.0, 1.0),
			vec4(0.0, 0.0, 0.0, 1.0));
#endif

	// Use atlas position and a prime number as the seed.
	uint noise = random_seed(ivec3(atlas_pos, 43573547));
	for (uint i = 0; i < bake_params.light_count; i++) {
		vec3 light;
		vec3 light_dir;
		float shadow;
		trace_direct_light(position, normal, geometry_normal, i, true, light, light_dir, noise, texel_size_world_space, shadow);

		if (lights.data[i].static_bake) {
			light_for_texture += light;

#ifdef USE_SH_LIGHTMAPS
			// For L0, light needs to be attenuated by dot(normal, light_dir) else it is oversaturated when sampled later.
			// For L1, light can't be attenuated by dot(normal, light_dir) since when sampling later, the dot product is done.
			// The output of trace_direct_light() is already attenuated by dot(normal, light_dir).
			// So L0 and L1 has the following relationship: L1 = L0 / dot(normal, light_dir).

			// For L1 packing to work, there needs to be a defined ratio (4) between L0 and L1 values.
			// This ratio is achieved with two coefficients c_l0 and c_l1, and ensuring that
			// 4 = (c_l0 * LO) / (c_l1 * L1)

			// For direct lights to look right, its effective "energy" needs to be 1 since it is not being integrated
			// unlike indirect lighting.
			// This binds c_l0 and c_l1 to the following relationship: 1 = c_l0 + c_l1

			float attenuation = dot(normal, light_dir);

			if (attenuation <= 0.0001) {
				continue;
			}

			float c_l0 = 1 / (1 + 4 * attenuation);
			float c_l1 = 1 - c_l0;
			float c[4] = float[](
					c_l0, //l0
					c_l1 / attenuation * light_dir.y, //l1n1
					c_l1 / attenuation * light_dir.z, //l1n0
					c_l1 / attenuation * light_dir.x //l1p1
			);

			for (uint j = 0; j < 4; j++) {
				sh_accum[j].rgb += light * c[j] * bake_params.exposure_normalization;
			}
#endif
		}

		light_for_bounces += light * lights.data[i].indirect_energy;

#ifdef USE_SHADOWMASK
		if (lights.data[i].type == LIGHT_TYPE_DIRECTIONAL && i == bake_params.shadowmask_light_idx) {
			shadowmask_value = max(shadowmask_value, shadow);
		}
#endif
	}

	light_for_bounces *= bake_params.exposure_normalization;
	imageStore(dest_light, ivec3(atlas_pos, params.output_slice), vec4(light_for_bounces, 1.0));

#ifdef USE_SH_LIGHTMAPS
	// Keep for adding at the end.
	imageStore(accum_light, ivec3(atlas_pos, params.output_slice * 4 + 0), sh_accum[0]);
	imageStore(accum_light, ivec3(atlas_pos, params.output_slice * 4 + 1), sh_accum[1]);
	imageStore(accum_light, ivec3(atlas_pos, params.output_slice * 4 + 2), sh_accum[2]);
	imageStore(accum_light, ivec3(atlas_pos, params.output_slice * 4 + 3), sh_accum[3]);
	imageStore(direct_light, ivec3(atlas_pos, params.output_slice * 4 + 0), sh_accum[0]);
	imageStore(direct_light, ivec3(atlas_pos, params.output_slice * 4 + 1), sh_accum[1]);
	imageStore(direct_light, ivec3(atlas_pos, params.output_slice * 4 + 2), sh_accum[2]);
	imageStore(direct_light, ivec3(atlas_pos, params.output_slice * 4 + 3), sh_accum[3]);
#else
	light_for_texture *= bake_params.exposure_normalization;
	imageStore(accum_light, ivec3(atlas_pos, params.output_slice), vec4(light_for_texture, 1.0));
	imageStore(direct_light, ivec3(atlas_pos, params.output_slice), vec4(light_for_texture, 1.0));
#endif

#ifdef USE_SHADOWMASK
	imageStore(shadowmask, ivec3(atlas_pos, params.atlas_slice), vec4(shadowmask_value, shadowmask_value, shadowmask_value, 1.0));
#endif

#endif // MODE_DIRECT_LIGHT

#ifdef MODE_AMBIENT_OCCLUSION
	vec3 normal = texelFetch(sampler2DArray(ao_normal, linear_sampler), ivec3(atlas_pos, params.geometry_slice), 0).xyz;
	if (length(normal) < 0.5) {
		return;
	}
	vec3 position = texelFetch(sampler2DArray(ao_position, linear_sampler), ivec3(atlas_pos, params.geometry_slice), 0).xyz;
	vec3 geometry_normal = texelFetch(sampler2DArray(ao_geometry, linear_sampler), ivec3(atlas_pos, params.geometry_slice), 0).yzw;
	uint noise = random_seed(ivec3(atlas_pos, 92821));
	vec2 ray_rotation = vec2(randomize(noise), randomize(noise));
	float occluded = 0.0;
	// Contact occlusion darkens direct light with its own short radius, grounding lit objects.
	float occluded_contact = 0.0;
	bool use_contact = bake_params.ao_contact_strength > 0.0;
	float ray_distance = use_contact ? max(bake_params.ao_distance, bake_params.ao_contact_distance) : bake_params.ao_distance;
	uint valid_samples = 0;
	for (uint i = 0; i < params.ray_count; i++) {
		// Most texels see nothing within ao_distance. Once a well-spread prefix of
		// the R2 sequence finds no occluder, the remaining rays won't change AO.
		if (i == 16u && occluded == 0.0 && occluded_contact == 0.0) {
			break;
		}
		vec3 ray_dir = generate_ray_dir_from_normal(normal, r2_sequence(i, ray_rotation));
		// Smooth normals can point rays below the actual surface. Mirror them
		// above it instead of dropping the sample, which left smooth edges noisier.
		float below = dot(geometry_normal, ray_dir);
		if (below < 0.0) {
			ray_dir -= 2.0 * below * geometry_normal;
		}
		if (dot(geometry_normal, ray_dir) <= 0.0) {
			continue;
		}
		valid_samples++;
		vec3 ray_end = position + ray_dir * ray_distance;
		// Lift along the face too: grazing rays offset only along themselves stay
		// on the surface and hit neighboring faces of curved meshes (AO acne).
		vec3 ray_origin = position + (ray_dir + geometry_normal) * bake_params.bias;
		float transmission = 1.0;
		bool did_hit = false;
		for (uint iter = 0; iter < max(bake_params.transparency_rays, 1u); iter++) {
			if (dot(ray_end - ray_origin, ray_dir) <= bake_params.bias) {
				break;
			}
			float opacity;
			vec3 hit_position;
#ifdef USE_HARDWARE_RAYTRACING
			uint hit = trace_ray_ao(ray_origin, ray_end, opacity, hit_position);
#else
			vec4 hit_albedo;
			uint hit = trace_ray_closest_hit_triangle_albedo_alpha(ray_origin, ray_end, hit_albedo, hit_position);
			opacity = hit_albedo.a;
#endif
			if (hit == RAY_MISS) {
				break;
			}
			// Match direct lighting: don't count the exit of a closed transparent mesh twice.
			if (hit == RAY_FRONT || !did_hit) {
				opacity = clamp(opacity, 0.0, 1.0);
				float hit_distance = distance(position, hit_position);
				occluded += transmission * opacity * (1.0 - smoothstep(0.0, bake_params.ao_distance, hit_distance));
				if (use_contact) {
					occluded_contact += transmission * opacity * (1.0 - smoothstep(0.0, bake_params.ao_contact_distance, hit_distance));
				}
				transmission *= 1.0 - opacity;
			}
			did_hit = true;
			if (transmission <= 0.00001) {
				break;
			}
			ray_origin = hit_position + ray_dir * bake_params.bias;
		}
	}
	float sample_count = float(max(valid_samples, 1u));
	float ao = pow(max(0.0, 1.0 - occluded / sample_count * bake_params.ao_strength), bake_params.ao_power);
	float ao_direct = pow(max(0.0, 1.0 - occluded_contact / sample_count * bake_params.ao_contact_strength), bake_params.ao_power);
#ifdef USE_SH_LIGHTMAPS
	for (int i = 0; i < 4; i++) {
		vec4 light = imageLoad(ao_light, ivec3(atlas_pos, params.output_slice * 4 + i));
		vec3 direct = imageLoad(ao_direct_light, ivec3(atlas_pos, params.output_slice * 4 + i)).rgb;
		vec3 indirect = light.rgb - direct;
		vec3 result = indirect * ao + direct * ao_direct;
		imageStore(ao_light, ivec3(atlas_pos, params.output_slice * 4 + i), vec4(result, light.a));
	}
#else
	vec4 light = imageLoad(ao_light, ivec3(atlas_pos, params.output_slice));
	vec3 direct = imageLoad(ao_direct_light, ivec3(atlas_pos, params.output_slice)).rgb;
	vec3 indirect = light.rgb - direct;
	vec3 result = indirect * ao + direct * ao_direct;
	imageStore(ao_light, ivec3(atlas_pos, params.output_slice), vec4(result, light.a));
#endif
#endif

#ifdef MODE_BOUNCE_LIGHT

	// Retrieve starting normal and position.
	vec3 normal = texelFetch(sampler2DArray(source_normal, linear_sampler), ivec3(atlas_pos, params.geometry_slice), 0).xyz;
	if (length(normal) < 0.5) {
		// The pixel is empty, skip processing it.
		return;
	}

	vec3 position = texelFetch(sampler2DArray(source_position, linear_sampler), ivec3(atlas_pos, params.geometry_slice), 0).xyz;
	vec3 geometry_normal = texelFetch(sampler2DArray(source_geometry, linear_sampler), ivec3(atlas_pos, params.geometry_slice), 0).yzw;
	const ivec2 footprint_directions[4] = ivec2[4](ivec2(-1, 0), ivec2(0, 1), ivec2(1, 0), ivec2(0, -1));
	float texel_size_world_space = 1e20;
	for (uint direction = 0; direction < 4; direction++) {
		ivec2 neighbor_pos = atlas_pos + footprint_directions[direction];
		if (any(lessThan(neighbor_pos, ivec2(0))) || any(greaterThanEqual(neighbor_pos, bake_params.atlas_size))) {
			continue;
		}
		vec4 neighbor_position = texelFetch(sampler2DArray(source_position, linear_sampler), ivec3(neighbor_pos, params.geometry_slice), 0);
		if (neighbor_position.a > 0.5) {
			texel_size_world_space = min(texel_size_world_space, distance(position, neighbor_position.xyz));
		}
	}
	if (texel_size_world_space == 1e20) {
		texel_size_world_space = bake_params.bias * 10.0;
	}
	// Seeded per texel only (not per pass), so the full ray budget stays one stratified sequence.
	const uint texel_seed = random_seed(ivec3(atlas_pos, int(params.atlas_slice) + 7919));
	uint rotation_noise = texel_seed;
	vec2 ray_rotation = vec2(randomize(rotation_noise), randomize(rotation_noise));
	vec2 env_rotation = vec2(randomize(rotation_noise), randomize(rotation_noise));
	// Preserve each original pass's RNG stream and accumulation boundary while
	// sharing texel geometry preparation across several passes on hardware RT.
#ifdef USE_HARDWARE_RAYTRACING
	uint pass_size = max(params.ray_pass_size, 1u);
#else
	uint pass_size = max(params.ray_to - params.ray_from, 1u);
#endif
	for (uint pass_from = params.ray_from; pass_from < params.ray_to; pass_from += pass_size) {
		vec2 stats = imageLoad(adaptive_stats, ivec3(atlas_pos, params.output_slice)).xy;
		if (stats.y < 0.0) {
			break; // Converged in an earlier pass, its estimate was already rescaled.
		}
		float pass_luminance = 0.0;
		float pass_luminance_sq = 0.0;
#ifdef USE_SH_LIGHTMAPS
		vec4 sh_accum[4] = vec4[](
				vec4(0.0, 0.0, 0.0, 1.0),
				vec4(0.0, 0.0, 0.0, 1.0),
				vec4(0.0, 0.0, 0.0, 1.0),
				vec4(0.0, 0.0, 0.0, 1.0));
#else
		vec3 light_accum = vec3(0.0);
#endif

		uint noise = random_seed(ivec3(pass_from, atlas_pos));
		uint near_backface_count = 0u;
		for (uint i = pass_from; i < min(pass_from + pass_size, params.ray_to); i++) {
			vec3 ray_dir = generate_ray_dir_from_normal(normal, r2_sequence(i, ray_rotation));
			vec3 light = vec3(0.0);
			if (dot(geometry_normal, ray_dir) > 0.0) {
				bool near_backface;
				light = clamp_indirect_sample(trace_indirect_light(position, ray_dir, noise, texel_size_world_space, true, normal, i, texel_seed, near_backface));
				near_backface_count += near_backface ? 1u : 0u;
			}

			vec3 env_dir;
			vec3 env_light = clamp_indirect_sample(sample_environment_light(position, normal, geometry_normal, r2_sequence(i, env_rotation), env_dir));
			float sample_luminance = dot(light + env_light, vec3(0.2126, 0.7152, 0.0722));
			pass_luminance += sample_luminance;
			pass_luminance_sq += sample_luminance * sample_luminance;

#ifdef USE_SH_LIGHTMAPS
			// These coefficients include the factored out SH evaluation, diffuse convolution, and final application, as well as the BRDF 1/PI and the spherical monte carlo factor.
			// LO: 1/(2*sqrtPI) * 1/(2*sqrtPI) * PI * PI * 1/PI = 0.25
			// L1: sqrt(3/(4*pi)) * sqrt(3/(4*pi)) * (PI*2/3) * (2 * PI) * 1/PI = 1.0
			// Note: This only works because we aren't scaling, rotating, or combing harmonics, we are just directing applying them in the shader.
			// env_light is already divided by the cosine pdf, so it projects with its own direction using the same coefficients.

			float c[4] = float[](
					0.25, //l0
					ray_dir.y, //l1n1
					ray_dir.z, //l1n0
					ray_dir.x //l1p1
			);
			float env_c[4] = float[](
					0.25, //l0
					env_dir.y, //l1n1
					env_dir.z, //l1n0
					env_dir.x //l1p1
			);

			for (uint j = 0; j < 4; j++) {
				sh_accum[j].rgb += light * c[j] + env_light * env_c[j];
			}
#else
			light_accum += light + env_light;
#endif
		}
		if (near_backface_count > 0u) {
			imageAtomicAdd(invalid_samples, ivec3(atlas_pos, params.atlas_slice), near_backface_count);
		}

		// Adaptive sampling: stop once the standard error of the mean is small relative to the mean.
		// Accumulation always divides by ray_count, so a texel stopping after rays_done rays is rescaled by ray_count / rays_done.
		uint rays_done = min(pass_from + pass_size, params.ray_to);
		stats += vec2(pass_luminance, pass_luminance_sq);
		float accum_scale = 1.0;
		if (bake_params.adaptive_threshold > 0.0 && rays_done < params.ray_count && rays_done >= max(params.ray_count / 4u, 16u)) {
			float n = float(rays_done);
			float mean = stats.x / n;
			float std_error = sqrt(max(stats.y / n - mean * mean, 0.0) / n);
			if (std_error <= bake_params.adaptive_threshold * max(mean, 1e-3)) {
				accum_scale = float(params.ray_count) / n;
				stats.y = -1.0;
			}
		}
		imageStore(adaptive_stats, ivec3(atlas_pos, params.output_slice), vec4(stats, 0.0, 0.0));

		// Add the averaged result to the accumulated light texture.
#ifdef USE_SH_LIGHTMAPS
		for (int i = 0; i < 4; i++) {
			vec4 accum = imageLoad(accum_light, ivec3(atlas_pos, params.output_slice * 4 + i));
			accum.rgb = (accum.rgb + sh_accum[i].rgb / float(params.ray_count)) * accum_scale;
			imageStore(accum_light, ivec3(atlas_pos, params.output_slice * 4 + i), accum);
		}
#else
		vec4 accum = imageLoad(accum_light, ivec3(atlas_pos, params.output_slice));
		accum.rgb = (accum.rgb + light_accum / float(params.ray_count)) * accum_scale;
		imageStore(accum_light, ivec3(atlas_pos, params.output_slice), accum);
#endif
		if (stats.y < 0.0) {
			break;
		}

		if (pass_from + pass_size < params.ray_to) {
			// The next logical pass reads this invocation's rgba16f result, retaining
			// the same quantization as separate dispatches. No workgroup barrier:
			// uncovered texels may already have returned above.
			memoryBarrierImage();
		}
	}

#endif

#ifdef MODE_UNOCCLUDE

	//texel_size = 0.5;
	//compute tangents

	vec4 position_alpha = imageLoad(position, ivec3(atlas_pos, params.geometry_slice));
	if (position_alpha.a < 0.5) {
		return;
	}

	vec3 vertex_pos = position_alpha.xyz;
	vec4 normal_tsize = imageLoad(unocclude, ivec3(atlas_pos, params.geometry_slice));

	vec3 face_normal = normal_tsize.xyz;
	float texel_size = normal_tsize.w;

	vec3 v0 = abs(face_normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
	vec3 tangent = normalize(cross(v0, face_normal));
	vec3 bitangent = normalize(cross(tangent, face_normal));
	vec3 base_pos = vertex_pos + face_normal * bake_params.bias; // Raise a bit.

	vec3 rays[4] = vec3[](tangent, bitangent, -tangent, -bitangent);
	float min_d = 1e20;
	float unocclude_mask = 0.0;

	for (int i = 0; i < 4; i++) {
		vec3 ray_to = base_pos + rays[i] * texel_size * params.denoiser_range;
		float d;
		vec3 norm;

		if (trace_ray_closest_hit_distance(base_pos, ray_to, d, norm) == RAY_BACK) {
			unocclude_mask = 1.0;
			if (d <= texel_size && d < min_d) {
				// This bias needs to be greater than the regular bias, because otherwise later, rays will go the other side when pointing back.
				vertex_pos = base_pos + rays[i] * d + norm * bake_params.bias * 10.0;
				min_d = d;
			}
		}
	}

	position_alpha.xyz = vertex_pos;

	imageStore(position, ivec3(atlas_pos, params.geometry_slice), position_alpha);
	imageStore(unocclude, ivec3(atlas_pos, params.geometry_slice), vec4(unocclude_mask, face_normal));

#endif

#ifdef MODE_LIGHT_PROBES

	vec3 position = probe_positions.data[probe_index].xyz;

	vec4 probe_sh_accum[9] = vec4[](
			vec4(0.0),
			vec4(0.0),
			vec4(0.0),
			vec4(0.0),
			vec4(0.0),
			vec4(0.0),
			vec4(0.0),
			vec4(0.0),
			vec4(0.0));

	// Position-based seeding keeps surviving probes stable when others are added
	// or removed. Global sample indices keep directions and paths independent of
	// how samples are split across dispatches.
	uint probe_seed = random_seed(floatBitsToInt(position));
	uint rotation_noise = hash(probe_seed ^ 49502741u);
	vec4 probe_rotation = generate_probe_rotation(rotation_noise);
	for (uint i = params.ray_from; i < params.ray_to; i++) {
		vec3 ray_dir = generate_probe_direction(i, params.ray_count, probe_rotation);
		uint noise = hash(probe_seed ^ hash(i) ^ 92821u);
		bool unused_near_backface;
		vec3 light = clamp_indirect_sample(trace_indirect_light(position, ray_dir, noise, 0.0, false, ray_dir, i, probe_seed, unused_near_backface));

		float c[9] = float[](
				0.282095, //l0
				0.488603 * ray_dir.y, //l1n1
				0.488603 * ray_dir.z, //l1n0
				0.488603 * ray_dir.x, //l1p1
				1.092548 * ray_dir.x * ray_dir.y, //l2n2
				1.092548 * ray_dir.y * ray_dir.z, //l2n1
				0.315392 * (3.0 * ray_dir.z * ray_dir.z - 1.0), //l20
				1.092548 * ray_dir.x * ray_dir.z, //l2p1
				0.546274 * (ray_dir.x * ray_dir.x - ray_dir.y * ray_dir.y) //l2p2
		);

		for (uint j = 0; j < 9; j++) {
			probe_sh_accum[j].rgb += light * c[j];
		}
	}

	if (params.ray_from > 0) {
		for (uint j = 0; j < 9; j++) { //accum from existing
			probe_sh_accum[j] += light_probes.data[probe_index * 9 + j];
		}
	}

	if (params.ray_to == params.ray_count) {
		for (uint j = 0; j < 9; j++) { //accum from existing
			probe_sh_accum[j] *= 4.0 / float(params.ray_count);
		}
	}

	for (uint j = 0; j < 9; j++) { //accum from existing
		light_probes.data[probe_index * 9 + j] = probe_sh_accum[j];
	}

#endif

#ifdef MODE_DILATE

	const int max_radius = int(4.0 * bake_params.supersampling_factor);
	const ivec2 directions[8] = ivec2[8](ivec2(-1, 0), ivec2(0, 1), ivec2(1, 0), ivec2(0, -1), ivec2(-1, -1), ivec2(-1, 1), ivec2(1, -1), ivec2(1, 1));

	vec4 texel_color = texelFetch(sampler2DArray(source_light, linear_sampler), ivec3(atlas_pos, params.atlas_slice), 0);

	// Dilation only fills empty texels; preserve already baked and denoised coverage.
	if (texel_color.a > 0.5) {
		imageStore(dest_light, ivec3(atlas_pos, params.atlas_slice), texel_color);
		return;
	}

	for (int radius = 1; radius <= max_radius; radius++) {
		for (uint i = 0; i < 8; i++) {
			const ivec2 sample_pos = atlas_pos + directions[i] * radius;
			// Texture bounds check for robustness.
			if (any(lessThan(sample_pos, ivec2(0))) ||
					any(greaterThanEqual(sample_pos, textureSize(source_light, 0).xy))) {
				continue;
			}

			vec4 neighbor_color = texelFetch(sampler2DArray(source_light, linear_sampler), ivec3(sample_pos, params.atlas_slice), 0);
			if (neighbor_color.a > 0.5) {
				texel_color = neighbor_color;
				break;
			}
		}

		if (texel_color.a > 0.5) {
			break;
		}
	}

	imageStore(dest_light, ivec3(atlas_pos, params.atlas_slice), texel_color);

#endif

#ifdef MODE_DENOISE
	// Joint Non-local means (JNLM) denoiser.
	//
	// Based on YoctoImageDenoiser's JNLM implementation with corrections from "Nonlinearly Weighted First-order Regression for Denoising Monte Carlo Renderings".
	//
	// <https://github.com/ManuelPrandini/YoctoImageDenoiser/blob/06e19489dd64e47792acffde536393802ba48607/libs/yocto_extension/yocto_extension.cpp#L207>
	// <https://benedikt-bitterli.me/nfor/nfor.pdf>
	//
	// MIT License
	//
	// Copyright (c) 2020 ManuelPrandini
	//
	// Permission is hereby granted, free of charge, to any person obtaining a copy
	// of this software and associated documentation files (the "Software"), to deal
	// in the Software without restriction, including without limitation the rights
	// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	// copies of the Software, and to permit persons to whom the Software is
	// furnished to do so, subject to the following conditions:
	//
	// The above copyright notice and this permission notice shall be included in all
	// copies or substantial portions of the Software.
	//
	// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
	// SOFTWARE.
	//
	// Most of the constants below have been hand-picked to fit the common scenarios lightmaps
	// are generated with, but they can be altered freely to experiment and achieve better results.

	// Half the size of the patch window around each pixel that is weighted to compute the denoised pixel.
	// A value of 1 represents a 3x3 window, a value of 2 a 5x5 window, etc.
	const int HALF_PATCH_WINDOW = 3;

	// Half the size of the search window around each pixel that is denoised and weighted to compute the denoised pixel.
	const int HALF_SEARCH_WINDOW = denoise_params.half_search_window;

	// For all of the following sigma values, smaller values will give less weight to pixels that have a bigger distance
	// in the feature being evaluated. Therefore, smaller values are likely to cause more noise to appear, but will also
	// cause less features to be erased in the process.

	// Controls how much the spatial distance of the pixels influences the denoising weight.
	const float SIGMA_SPATIAL = denoise_params.spatial_bandwidth;

	// Controls how much the light color distance of the pixels influences the denoising weight.
	const float SIGMA_LIGHT = denoise_params.light_bandwidth;

	// Controls how much the albedo color distance of the pixels influences the denoising weight.
	const float SIGMA_ALBEDO = denoise_params.albedo_bandwidth;

	// Controls how much the normal vector distance of the pixels influences the denoising weight.
	const float SIGMA_NORMAL = denoise_params.normal_bandwidth;

	// Strength of the filter. The original paper recommends values around 10 to 15 times the Sigma parameter.
	const float FILTER_VALUE = denoise_params.filter_strength * SIGMA_LIGHT;

	// Formula constants.
	const int PATCH_WINDOW_DIMENSION = (HALF_PATCH_WINDOW * 2 + 1);
	const int PATCH_WINDOW_DIMENSION_SQUARE = (PATCH_WINDOW_DIMENSION * PATCH_WINDOW_DIMENSION);
	const float TWO_SIGMA_SPATIAL_SQUARE = 2.0f * SIGMA_SPATIAL * SIGMA_SPATIAL;
	const float TWO_SIGMA_LIGHT_SQUARE = 2.0f * SIGMA_LIGHT * SIGMA_LIGHT;
	const float TWO_SIGMA_ALBEDO_SQUARE = 2.0f * SIGMA_ALBEDO * SIGMA_ALBEDO;
	const float TWO_SIGMA_NORMAL_SQUARE = 2.0f * SIGMA_NORMAL * SIGMA_NORMAL;
	const float FILTER_SQUARE_TWO_SIGMA_LIGHT_SQUARE = FILTER_VALUE * FILTER_VALUE * TWO_SIGMA_LIGHT_SQUARE;
	const float EPSILON = 1e-6f;

	const uint slice_count = denoise_params.slice_count;
	const uint slice_base = params.atlas_slice * slice_count;
	const bool is_directional = (slice_count == 4);

	for (uint i = 0; i < slice_count; i++) {
		uint lightmap_slice = slice_base + i;
		vec3 denoised_rgb = vec3(0.0f);
		vec4 input_light = texelFetch(sampler2DArray(source_light, linear_sampler), ivec3(atlas_pos, lightmap_slice), 0);
		vec3 input_albedo = texelFetch(sampler2DArray(albedo_tex, linear_sampler), ivec3(atlas_pos, params.material_slice), 0).rgb;
		vec3 input_normal = texelFetch(sampler2DArray(source_normal, linear_sampler), ivec3(atlas_pos, params.geometry_slice), 0).xyz;
		if (length(input_normal) > EPSILON) {
			// Compute the denoised pixel if the normal is valid.
			float sum_weights = 0.0f;
			vec3 input_rgb = input_light.rgb;
			for (int search_y = -HALF_SEARCH_WINDOW; search_y <= HALF_SEARCH_WINDOW; search_y++) {
				for (int search_x = -HALF_SEARCH_WINDOW; search_x <= HALF_SEARCH_WINDOW; search_x++) {
					ivec2 search_pos = atlas_pos + ivec2(search_x, search_y);
					vec3 search_rgb = texelFetch(sampler2DArray(source_light, linear_sampler), ivec3(search_pos, lightmap_slice), 0).rgb;
					vec3 search_albedo = texelFetch(sampler2DArray(albedo_tex, linear_sampler), ivec3(search_pos, params.material_slice), 0).rgb;
					vec3 search_normal = texelFetch(sampler2DArray(source_normal, linear_sampler), ivec3(search_pos, params.geometry_slice), 0).xyz;
					float search_occlusion = texelFetch(sampler2DArray(unocclude_mask, linear_sampler), ivec3(search_pos, params.geometry_slice), 0).r;
					float patch_square_dist = 0.0f;
					for (int offset_y = -HALF_PATCH_WINDOW; offset_y <= HALF_PATCH_WINDOW; offset_y++) {
						for (int offset_x = -HALF_PATCH_WINDOW; offset_x <= HALF_PATCH_WINDOW; offset_x++) {
							ivec2 offset_input_pos = atlas_pos + ivec2(offset_x, offset_y);
							ivec2 offset_search_pos = search_pos + ivec2(offset_x, offset_y);
							vec3 offset_input_rgb = texelFetch(sampler2DArray(source_light, linear_sampler), ivec3(offset_input_pos, slice_base), 0).rgb;
							vec3 offset_search_rgb = texelFetch(sampler2DArray(source_light, linear_sampler), ivec3(offset_search_pos, slice_base), 0).rgb;
							vec3 offset_delta_rgb = offset_input_rgb - offset_search_rgb;

							if (is_directional) {
								// Since L0 data is 1/4 the value of a regular lightmap,
								// we have to multiply it by 4.
								offset_delta_rgb *= 4.0;
							}

							patch_square_dist += dot(offset_delta_rgb, offset_delta_rgb) - TWO_SIGMA_LIGHT_SQUARE;
						}
					}

					patch_square_dist = max(0.0f, patch_square_dist / (3.0f * PATCH_WINDOW_DIMENSION_SQUARE));

					float weight = 1.0f;

					// Ignore weight if search position is out of bounds.
					weight *= step(0, search_pos.x) * step(search_pos.x, bake_params.atlas_size.x - 1);
					weight *= step(0, search_pos.y) * step(search_pos.y, bake_params.atlas_size.y - 1);

					// Ignore weight if normal is zero length.
					weight *= step(EPSILON, length(search_normal));

					// Weight with pixel distance.
					vec2 pixel_delta = vec2(search_x, search_y);
					float pixel_square_dist = dot(pixel_delta, pixel_delta);
					weight *= exp(-pixel_square_dist / TWO_SIGMA_SPATIAL_SQUARE);

					// Weight with patch.
					weight *= exp(-patch_square_dist / FILTER_SQUARE_TWO_SIGMA_LIGHT_SQUARE);

					// Weight with albedo.
					vec3 albedo_delta = input_albedo - search_albedo;
					float albedo_square_dist = dot(albedo_delta, albedo_delta);
					weight *= exp(-albedo_square_dist / TWO_SIGMA_ALBEDO_SQUARE);

					// Weight with normal.
					vec3 normal_delta = input_normal - search_normal;
					float normal_square_dist = dot(normal_delta, normal_delta);
					weight *= exp(-normal_square_dist / TWO_SIGMA_NORMAL_SQUARE);

					// Weight with occlusion.
					weight *= 1.0 - search_occlusion;

					denoised_rgb += weight * search_rgb;
					sum_weights += weight;
				}
			}

			// Avoid division by zero if no weights were accumulated.
			denoised_rgb = sum_weights > EPSILON ? denoised_rgb / sum_weights : input_rgb;
		} else {
			// Ignore pixels where the normal is empty, just copy the light color.
			denoised_rgb = input_light.rgb;
		}

		imageStore(dest_light, ivec3(atlas_pos, lightmap_slice), vec4(denoised_rgb, input_light.a));
	}
#endif

#ifdef MODE_PACK_L1_COEFFS
	vec4 base_coeff = texelFetch(sampler2DArray(source_light, linear_sampler), ivec3(atlas_pos, params.atlas_slice * 4), 0);
	imageStore(dest_light, ivec3(atlas_pos, params.atlas_slice * 4), base_coeff);

	for (int i = 1; i < 4; i++) {
		vec4 c = texelFetch(sampler2DArray(source_light, linear_sampler), ivec3(atlas_pos, params.atlas_slice * 4 + i), 0);
		c.rgb /= (base_coeff.rgb * 8.0 + vec3(1e-6f));
		c.rgb = clamp(c.rgb + vec3(0.5), vec3(0.0), vec3(1.0));

		imageStore(dest_light, ivec3(atlas_pos, params.atlas_slice * 4 + i), c);
	}
#endif
}

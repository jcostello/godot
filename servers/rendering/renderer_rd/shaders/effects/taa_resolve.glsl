///////////////////////////////////////////////////////////////////////////////////
// Copyright(c) 2016-2022 Panos Karabelas
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is furnished
// to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
///////////////////////////////////////////////////////////////////////////////////
// File changes (yyyy-mm-dd)
// 2025-11-05: Jakub Brzyski: Added dynamic variance, base variance value adjusted to reduce ghosting
// 2022-05-06: Panos Karabelas: first commit
// 2020-12-05: Joan Fons: convert to Vulkan and Godot
///////////////////////////////////////////////////////////////////////////////////

#[compute]

#version 450

#VERSION_DEFINES

// Based on Spartan Engine's TAA implementation (without TAA upscale).
// <https://github.com/PanosK92/SpartanEngine/blob/a8338d0609b85dc32f3732a5c27fb4463816a3b9/Data/shaders/temporal_antialiasing.hlsl>

#define GROUP_SIZE 8
#define FLT_MIN 0.00000001
#define FLT_MAX 32767.0
#define RPC_9 0.11111111111
#define RPC_16 0.0625

#define DISOCCLUSION_SCALE 0.01 // Scale the weight of this pixel calculated as (change in velocity - threshold) * scale.

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

vec3 reinhard(vec3 hdr) {
	return hdr / (hdr + 1.0);
}
vec3 reinhard_inverse(vec3 sdr) {
	return sdr / (1.0 - sdr);
}

#define SDR_MAX (FLT_MAX / (FLT_MAX + 1.0)) // reinhard(FLT_MAX)

vec3 rgb_to_ycocg(vec3 c) {
	return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25)));
}

vec3 ycocg_to_rgb(vec3 c) {
	return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

#ifdef MODE_TAA_RESOLVE

layout(rgba16f, set = 0, binding = 0) uniform restrict readonly image2D color_buffer;
layout(set = 0, binding = 1) uniform sampler2D depth_buffer;
layout(rg16f, set = 0, binding = 2) uniform restrict readonly image2D velocity_buffer;
layout(rg16f, set = 0, binding = 3) uniform restrict readonly image2D last_velocity_buffer;
layout(set = 0, binding = 4) uniform sampler2D history_buffer;
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D output_buffer;

layout(push_constant, std430) uniform Params {
	vec2 resolution;
	float disocclusion_threshold; // 0.1 / max(params.resolution.x, params.resolution.y)
	float variance_dynamic;
}
params;

const ivec2 kOffsets3x3[9] = {
	ivec2(-1, -1),
	ivec2(0, -1),
	ivec2(1, -1),
	ivec2(-1, 0),
	ivec2(0, 0),
	ivec2(1, 0),
	ivec2(-1, 1),
	ivec2(0, 1),
	ivec2(1, 1),
};

/*------------------------------------------------------------------------------
						THREAD GROUP SHARED MEMORY (LDS)
------------------------------------------------------------------------------*/

const int kBorderSize = 1;
const int kGroupSize = GROUP_SIZE;
const int kTileDimension = kGroupSize + kBorderSize * 2;
const int kTileDimension2 = kTileDimension * kTileDimension;

float get_depth(ivec2 thread_id) {
	return texelFetch(depth_buffer, thread_id, 0).r;
}

shared vec3 tile_color[kTileDimension][kTileDimension];
shared float tile_depth[kTileDimension][kTileDimension];

vec3 load_color(uvec2 group_thread_id) {
	group_thread_id += kBorderSize;
	return tile_color[group_thread_id.x][group_thread_id.y];
}

void store_color(uvec2 group_thread_id, vec3 color) {
	tile_color[group_thread_id.x][group_thread_id.y] = color;
}

float load_depth(uvec2 group_thread_id) {
	group_thread_id += kBorderSize;
	return tile_depth[group_thread_id.x][group_thread_id.y];
}

void store_depth(uvec2 group_thread_id, float depth) {
	tile_depth[group_thread_id.x][group_thread_id.y] = depth;
}

void store_color_depth(uvec2 group_thread_id, ivec2 thread_id) {
	// out of bounds clamp
	thread_id = clamp(thread_id, ivec2(0, 0), ivec2(params.resolution) - ivec2(1, 1));

	// Tonemap and convert once here, so neighbourhood clipping and blending happen in the same space.
	store_color(group_thread_id, rgb_to_ycocg(reinhard(imageLoad(color_buffer, thread_id).rgb)));
	store_depth(group_thread_id, get_depth(thread_id));
}

void populate_group_shared_memory(uvec2 group_id, uint group_index) {
	// Populate group shared memory
	ivec2 group_top_left = ivec2(group_id) * kGroupSize - kBorderSize;
	if (group_index < (kTileDimension2 >> 2)) {
		ivec2 group_thread_id_1 = ivec2(group_index % kTileDimension, group_index / kTileDimension);
		ivec2 group_thread_id_2 = ivec2((group_index + (kTileDimension2 >> 2)) % kTileDimension, (group_index + (kTileDimension2 >> 2)) / kTileDimension);
		ivec2 group_thread_id_3 = ivec2((group_index + (kTileDimension2 >> 1)) % kTileDimension, (group_index + (kTileDimension2 >> 1)) / kTileDimension);
		ivec2 group_thread_id_4 = ivec2((group_index + kTileDimension2 * 3 / 4) % kTileDimension, (group_index + kTileDimension2 * 3 / 4) / kTileDimension);

		store_color_depth(group_thread_id_1, group_top_left + group_thread_id_1);
		store_color_depth(group_thread_id_2, group_top_left + group_thread_id_2);
		store_color_depth(group_thread_id_3, group_top_left + group_thread_id_3);
		store_color_depth(group_thread_id_4, group_top_left + group_thread_id_4);
	}

	// Wait for group threads to load store data.
	groupMemoryBarrier();
	barrier();
}

/*------------------------------------------------------------------------------
								VELOCITY
------------------------------------------------------------------------------*/

// Depth is reverse-Z, so the closest pixel has the largest depth.
void depth_test_closest(uvec2 pos, inout float closest_depth, inout uvec2 closest_pos) {
	float depth = load_depth(pos);

	if (depth > closest_depth) {
		closest_depth = depth;
		closest_pos = pos;
	}
}

// Returns velocity with closest depth (3x3 neighborhood)
void get_closest_pixel_velocity_3x3(in uvec2 group_pos, uvec2 group_origin, out vec2 velocity) {
	float closest_depth = 0.0;
	uvec2 closest_pos = group_pos;

	depth_test_closest(group_pos + kOffsets3x3[0], closest_depth, closest_pos);
	depth_test_closest(group_pos + kOffsets3x3[1], closest_depth, closest_pos);
	depth_test_closest(group_pos + kOffsets3x3[2], closest_depth, closest_pos);
	depth_test_closest(group_pos + kOffsets3x3[3], closest_depth, closest_pos);
	depth_test_closest(group_pos + kOffsets3x3[4], closest_depth, closest_pos);
	depth_test_closest(group_pos + kOffsets3x3[5], closest_depth, closest_pos);
	depth_test_closest(group_pos + kOffsets3x3[6], closest_depth, closest_pos);
	depth_test_closest(group_pos + kOffsets3x3[7], closest_depth, closest_pos);
	depth_test_closest(group_pos + kOffsets3x3[8], closest_depth, closest_pos);

	// Velocity out
	velocity = imageLoad(velocity_buffer, ivec2(group_origin + closest_pos)).xy;
}

/*------------------------------------------------------------------------------
							  HISTORY SAMPLING
------------------------------------------------------------------------------*/

vec3 sample_catmull_rom_5(sampler2D stex, vec2 uv, vec2 resolution) {
	// Source: https://gist.github.com/TheRealMJP/c83b8c0f46b63f3a88a5986f4fa982b1
	// License: https://gist.github.com/TheRealMJP/bc503b0b87b643d3505d41eab8b332ae

	// We're going to sample a 4x4 grid of texels surrounding the target UV coordinate. We'll do this by rounding
	// down the sample location to get the exact center of our "starting" texel. The starting texel will be at
	// location [1, 1] in the grid, where [0, 0] is the top left corner.
	vec2 sample_pos = uv * resolution;
	vec2 texPos1 = floor(sample_pos - 0.5f) + 0.5f;

	// Compute the fractional offset from our starting texel to our original sample location, which we'll
	// feed into the Catmull-Rom spline function to get our filter weights.
	vec2 f = sample_pos - texPos1;

	// Compute the Catmull-Rom weights using the fractional offset that we calculated earlier.
	// These equations are pre-expanded based on our knowledge of where the texels will be located,
	// which lets us avoid having to evaluate a piece-wise function.
	vec2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
	vec2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
	vec2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
	vec2 w3 = f * f * (-0.5f + 0.5f * f);

	// Work out weighting factors and sampling offsets that will let us use bilinear filtering to
	// simultaneously evaluate the middle 2 samples from the 4x4 grid.
	vec2 w12 = w1 + w2;
	vec2 offset12 = w2 / (w1 + w2);

	// Compute the final UV coordinates we'll use for sampling the texture
	vec2 texPos0 = texPos1 - 1.0f;
	vec2 texPos3 = texPos1 + 2.0f;
	vec2 texPos12 = texPos1 + offset12;

	texPos0 /= resolution;
	texPos3 /= resolution;
	texPos12 /= resolution;

	// The 4 corner taps have negligible weight, skip them and renormalize (5 taps instead of 9).
	float w_top = w12.x * w0.y;
	float w_left = w0.x * w12.y;
	float w_center = w12.x * w12.y;
	float w_right = w3.x * w12.y;
	float w_bottom = w12.x * w3.y;

	vec3 result = textureLod(stex, vec2(texPos12.x, texPos0.y), 0.0).xyz * w_top;
	result += textureLod(stex, vec2(texPos0.x, texPos12.y), 0.0).xyz * w_left;
	result += textureLod(stex, texPos12, 0.0).xyz * w_center;
	result += textureLod(stex, vec2(texPos3.x, texPos12.y), 0.0).xyz * w_right;
	result += textureLod(stex, vec2(texPos12.x, texPos3.y), 0.0).xyz * w_bottom;

	return max(result / (w_top + w_left + w_center + w_right + w_bottom), 0.0f);
}

/*------------------------------------------------------------------------------
							  HISTORY CLIPPING
------------------------------------------------------------------------------*/

// Based on "Temporal Reprojection Anti-Aliasing" - https://github.com/playdeadgames/temporal
vec3 clip_aabb(vec3 aabb_min, vec3 aabb_max, vec3 p, vec3 q) {
	vec3 r = q - p;
	vec3 rmax = (aabb_max - p.xyz);
	vec3 rmin = (aabb_min - p.xyz);

	if (r.x > rmax.x + FLT_MIN) {
		r *= (rmax.x / r.x);
	}
	if (r.y > rmax.y + FLT_MIN) {
		r *= (rmax.y / r.y);
	}
	if (r.z > rmax.z + FLT_MIN) {
		r *= (rmax.z / r.z);
	}

	if (r.x < rmin.x - FLT_MIN) {
		r *= (rmin.x / r.x);
	}
	if (r.y < rmin.y - FLT_MIN) {
		r *= (rmin.y / r.y);
	}
	if (r.z < rmin.z - FLT_MIN) {
		r *= (rmin.z / r.z);
	}

	return p + r;
}

// Clip history to the neighbourhood of the current sample (all colors in tonemapped YCoCg)
vec3 clip_history_3x3(uvec2 group_pos, vec3 color_history, vec2 velocity_closest) {
	// Sample a 3x3 neighbourhood
	vec3 s1 = load_color(group_pos + kOffsets3x3[0]);
	vec3 s2 = load_color(group_pos + kOffsets3x3[1]);
	vec3 s3 = load_color(group_pos + kOffsets3x3[2]);
	vec3 s4 = load_color(group_pos + kOffsets3x3[3]);
	vec3 s5 = load_color(group_pos + kOffsets3x3[4]);
	vec3 s6 = load_color(group_pos + kOffsets3x3[5]);
	vec3 s7 = load_color(group_pos + kOffsets3x3[6]);
	vec3 s8 = load_color(group_pos + kOffsets3x3[7]);
	vec3 s9 = load_color(group_pos + kOffsets3x3[8]);

	// Compute min and max (with an adaptive box size, which greatly reduces ghosting)
	vec3 color_avg = (s1 + s2 + s3 + s4 + s5 + s6 + s7 + s8 + s9) * RPC_9;
	vec3 color_avg2 = ((s1 * s1) + (s2 * s2) + (s3 * s3) + (s4 * s4) + (s5 * s5) + (s6 * s6) + (s7 * s7) + (s8 * s8) + (s9 * s9)) * RPC_9;
	// Use variance clipping as described in https://developer.download.nvidia.com/gameworks/events/GDC2016/msalvi_temporal_supersampling.pdf
	float box_size = mix(0.0f, params.variance_dynamic, smoothstep(0.02f, 0.0f, length(velocity_closest)));
	vec3 dev = sqrt(abs(color_avg2 - (color_avg * color_avg))) * box_size;
	vec3 color_min = color_avg - dev;
	vec3 color_max = color_avg + dev;

	// Variance clipping
	return clip_aabb(color_min, color_max, clamp(color_avg, color_min, color_max), color_history);
}

/*------------------------------------------------------------------------------
									TAA
------------------------------------------------------------------------------*/

// This is "velocity disocclusion" as described by https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/.
// We use texel space, so our scale and threshold differ.
float get_factor_disocclusion(vec2 uv_reprojected, vec2 velocity) {
	vec2 velocity_previous = imageLoad(last_velocity_buffer, ivec2(uv_reprojected * params.resolution)).xy;
	vec2 velocity_texels = velocity * params.resolution;
	vec2 prev_velocity_texels = velocity_previous * params.resolution;
	float disocclusion = length(prev_velocity_texels - velocity_texels) - params.disocclusion_threshold;
	return clamp(disocclusion * DISOCCLUSION_SCALE, 0.0, 1.0);
}

vec3 temporal_antialiasing(uvec2 pos_group_origin, uvec2 pos_group, vec2 uv, sampler2D tex_history) {
	// Reproject using the velocity with the closest depth, which keeps edges of moving objects sharp.
	vec2 velocity_closest = vec2(0.0);
	get_closest_pixel_velocity_3x3(pos_group, pos_group_origin, velocity_closest);
	vec2 uv_reprojected = uv + velocity_closest;

	// Get input color (already tonemapped YCoCg)
	vec3 color_input = load_color(pos_group);

	// Get history color (catmull-rom reduces a lot of the blurring that you get under motion)
	vec3 color_history = rgb_to_ycocg(reinhard(sample_catmull_rom_5(tex_history, uv_reprojected, params.resolution)));

	// Clip history to the neighbourhood of the current sample (fixes a lot of the ghosting).
	color_history = clip_history_3x3(pos_group, color_history, velocity_closest);

	// Compute blend factor
	float blend_factor = RPC_16; // We want to be able to accumulate as many jitter samples as we generated, that is, 16.
	{
		// Reduce flickering
		float lum_color = color_input.x;
		float lum_history = color_history.x;
		float diff = abs(lum_color - lum_history) / max(lum_color, max(lum_history, 0.2)); // Relative difference, the floor avoids over-reacting in the dark.
		diff = 1.0 - diff;
		blend_factor *= diff * diff;

		// If re-projected UV is out of screen, converge to current color immediately.
		float factor_screen = any(lessThan(uv_reprojected, vec2(0.0))) || any(greaterThan(uv_reprojected, vec2(1.0))) ? 1.0 : 0.0;

		// Increase blend factor when there is disocclusion (fixes a lot of the remaining ghosting).
		float factor_disocclusion = get_factor_disocclusion(uv_reprojected, velocity_closest);

		// Added after flicker reduction so these always win.
		blend_factor = clamp(blend_factor + factor_screen + factor_disocclusion, 0.0, 1.0);
	}

	// Lerp/blend, then back to linear RGB (clamped to prevent NaNs and division by zero)
	vec3 color_resolved = ycocg_to_rgb(mix(color_history, color_input, blend_factor));
	return reinhard_inverse(clamp(color_resolved, 0.0, SDR_MAX));
}

void main() {
	populate_group_shared_memory(gl_WorkGroupID.xy, gl_LocalInvocationIndex);

	// Out of bounds check
	if (any(greaterThanEqual(vec2(gl_GlobalInvocationID.xy), params.resolution))) {
		return;
	}

	const uvec2 pos_group = gl_LocalInvocationID.xy;
	const uvec2 pos_group_origin = gl_WorkGroupID.xy * kGroupSize;
	const vec2 uv = (gl_GlobalInvocationID.xy + 0.5f) / params.resolution;

	vec3 result = temporal_antialiasing(pos_group_origin, pos_group, uv, history_buffer);
	imageStore(output_buffer, ivec2(gl_GlobalInvocationID.xy), vec4(result, 1.0));
}

#endif // MODE_TAA_RESOLVE

#ifdef MODE_TAA_SHARPEN

layout(rgba16f, set = 0, binding = 0) uniform restrict readonly image2D source_buffer;
layout(rgba16f, set = 0, binding = 1) uniform restrict writeonly image2D output_buffer;

layout(push_constant, std430) uniform Params {
	ivec2 resolution;
	float sharpness; // 0.0 to 1.0
	float pad;
}
params;

vec3 load_tonemapped(ivec2 pos) {
	return reinhard(max(imageLoad(source_buffer, clamp(pos, ivec2(0), params.resolution - 1)).rgb, 0.0));
}

// Contrast adaptive sharpening (AMD FidelityFX CAS, cross pattern) on the resolved output only,
// so the sharpened result never feeds back into the history.
void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.resolution))) {
		return;
	}

	vec3 c = load_tonemapped(pos);
	vec3 n = load_tonemapped(pos + ivec2(0, -1));
	vec3 s = load_tonemapped(pos + ivec2(0, 1));
	vec3 e = load_tonemapped(pos + ivec2(1, 0));
	vec3 w = load_tonemapped(pos + ivec2(-1, 0));

	vec3 color_min = min(c, min(min(n, s), min(e, w)));
	vec3 color_max = max(c, max(max(n, s), max(e, w)));

	// Sharpen less where local contrast is already high, to avoid ringing and amplifying aliasing.
	vec3 amp = sqrt(clamp(min(color_min, 1.0 - color_max) / max(color_max, FLT_MIN), 0.0, 1.0));
	vec3 lobe = amp * mix(-0.125, -0.2, params.sharpness);
	vec3 result = (c + lobe * (n + s + e + w)) / (1.0 + 4.0 * lobe);

	imageStore(output_buffer, pos, vec4(reinhard_inverse(clamp(result, 0.0, SDR_MAX)), 1.0));
}

#endif // MODE_TAA_SHARPEN

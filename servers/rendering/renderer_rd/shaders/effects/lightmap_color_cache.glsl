#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform sampler2DArray source_texture;
layout(set = 0, binding = 1, r11f_g11f_b10f) uniform writeonly image2DArray target_texture;
layout(set = 0, binding = 2, std140) uniform ColorParameters {
	vec4 data[14];
} grading;
layout(push_constant, std430) uniform Params {
	uint directional;
	int mip;
	uint pad0;
	uint pad1;
} params;

#include "../lightmap_color_adjustments_inc.glsl"

void main() {
	ivec3 position = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(position, imageSize(target_texture)))) {
		return;
	}
	vec3 color = texelFetch(source_texture, position, params.mip).rgb;
	// Preserve layer addressing: only L0 is graded, direction coefficients are copied.
	if (params.directional == 0u || position.z % 4 == 0) {
		color = lightmap_grade_color(color, grading.data);
	}
	// B10 has the lowest maximum finite value of the packed RGB channels.
	imageStore(target_texture, position, vec4(clamp(color, vec3(0.0), vec3(64512.0)), 1.0));
}

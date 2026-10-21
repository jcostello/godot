// Hardware traversal replaces only triangle intersection queries. Materials,
// transparency, light integration and all bake outputs use the existing code.
layout(set = 0, binding = 16) uniform accelerationStructureEXT lightmap_acceleration_structure;

layout(set = 0, binding = 17, std430) restrict readonly buffer RaytracingTriangles {
	// Instance -> geometry offset table -> primitive -> original bake triangle.
	uint data[];
}
raytracing_triangles;

uint ray_query_triangle_id(uint p_instance, uint p_geometry, uint p_primitive) {
	uint base = raytracing_triangles.data[p_instance];
	return raytracing_triangles.data[raytracing_triangles.data[base + p_geometry] + p_primitive];
}

vec3 ray_query_barycentric(vec2 p_uv) {
	return vec3(1.0 - p_uv.x - p_uv.y, p_uv.x, p_uv.y);
}

vec3 ray_query_normal(uint p_triangle) {
	uvec3 indices = triangles.data[p_triangle].indices;
	vec3 a = vertices.data[indices.x].position;
	vec3 b = vertices.data[indices.y].position;
	vec3 c = vertices.data[indices.z].position;
	return -cross(a - b, a - c);
}

uint trace_ray(vec3 p_from, vec3 p_to, bool p_any_hit, bool p_sample_albedo, out float r_distance, out vec3 r_normal, out uint r_triangle, out vec3 r_barycentric, out vec4 r_albedo_alpha) {
	r_albedo_alpha = vec4(0.0, 0.0, 0.0, -1.0);
	vec3 delta = p_to - p_from;
	float ray_length = length(delta);
	if (ray_length <= bake_params.bias) {
		return RAY_MISS;
	}
	vec3 direction = delta / ray_length;
	float query_min = uintBitsToFloat(floatBitsToUint(bake_params.bias) + 1u);
	float query_max = uintBitsToFloat(floatBitsToUint(ray_length) - 1u);
	if (query_min > query_max) {
		return RAY_MISS;
	}

	// Hardware commits non-cutout geometry directly. Only cutouts are returned
	// as candidates, where the existing alpha-scissor mask can reject them.
	uint flags = p_any_hit ? gl_RayFlagsTerminateOnFirstHitEXT : gl_RayFlagsNoneEXT;
	rayQueryEXT query;
	rayQueryInitializeEXT(query, lightmap_acceleration_structure, flags, 0xFF, p_from, query_min, direction, query_max);
	while (rayQueryProceedEXT(query)) {
		if (rayQueryGetIntersectionTypeEXT(query, false) != gl_RayQueryCandidateIntersectionTriangleEXT) {
			continue;
		}
		float distance = rayQueryGetIntersectionTEXT(query, false);
		if (distance <= bake_params.bias || distance >= ray_length) {
			continue;
		}
		uint triangle = ray_query_triangle_id(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(query, false)), uint(rayQueryGetIntersectionGeometryIndexEXT(query, false)), uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, false)));
		vec3 barycentric = ray_query_barycentric(rayQueryGetIntersectionBarycentricsEXT(query, false));
		if (triangles.data[triangle].alpha_scissor_threshold >= 0.0 && alpha_scissor_rejects(triangle, barycentric)) {
			continue;
		}
		if (p_any_hit) {
			rayQueryTerminateEXT(query);
			return RAY_ANY;
		}
		rayQueryConfirmIntersectionEXT(query);
	}
	if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
		return RAY_MISS;
	}

	if (p_any_hit) {
		return RAY_ANY;
	}
	r_triangle = ray_query_triangle_id(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(query, true)), uint(rayQueryGetIntersectionGeometryIndexEXT(query, true)), uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, true)));
	r_barycentric = ray_query_barycentric(rayQueryGetIntersectionBarycentricsEXT(query, true));
	r_distance = rayQueryGetIntersectionTEXT(query, true);
	vec3 normal = ray_query_normal(r_triangle);
	bool geometric_backface = dot(normal, direction) >= 0.0;
	if (geometric_backface) {
		// Compute prefers a geometric front face up to one bias unit farther
		// away. A nearest-hit query alone would lose that behavior for meshes
		// with coincident front/back faces. Search only this short tie interval.
		rayQueryEXT facing_query;
		float tie_end = min(ray_length, r_distance + bake_params.bias);
		rayQueryInitializeEXT(facing_query, lightmap_acceleration_structure, gl_RayFlagsNoOpaqueEXT, 0xFF, p_from, query_min, direction, min(query_max, tie_end));
		while (rayQueryProceedEXT(facing_query)) {
			if (rayQueryGetIntersectionTypeEXT(facing_query, false) != gl_RayQueryCandidateIntersectionTriangleEXT) {
				continue;
			}
			float distance = rayQueryGetIntersectionTEXT(facing_query, false);
			if (distance <= bake_params.bias || distance >= ray_length) {
				continue;
			}
			uint triangle = ray_query_triangle_id(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(facing_query, false)), uint(rayQueryGetIntersectionGeometryIndexEXT(facing_query, false)), uint(rayQueryGetIntersectionPrimitiveIndexEXT(facing_query, false)));
			vec3 candidate_normal = ray_query_normal(triangle);
			if (dot(candidate_normal, direction) >= 0.0) {
				continue;
			}
			float biased_distance = max(bake_params.bias, distance - bake_params.bias);
			if (biased_distance >= r_distance) {
				continue;
			}
			vec3 barycentric = ray_query_barycentric(rayQueryGetIntersectionBarycentricsEXT(facing_query, false));
			if (triangles.data[triangle].alpha_scissor_threshold >= 0.0 && alpha_scissor_rejects(triangle, barycentric)) {
				continue;
			}
			r_triangle = triangle;
			r_barycentric = barycentric;
			r_distance = biased_distance;
			normal = candidate_normal;
			geometric_backface = false;
			// Do not confirm: a farther front face can have a smaller biased
			// distance than the current back face. Inspect the whole tie range.
		}
	} else {
		r_distance = max(bake_params.bias, r_distance - bake_params.bias);
	}

	r_normal = normalize(normal);
	if (p_sample_albedo) {
		r_albedo_alpha = apply_triangle_alpha_scissor(r_triangle, sample_triangle_albedo_alpha(r_triangle, r_barycentric));
	}
	bool backface = geometric_backface;
	switch (triangles.data[r_triangle].cull_mode) {
		case CULL_DISABLED:
			backface = false;
			break;
		case CULL_FRONT:
			backface = !backface;
			break;
	}
	return backface ? RAY_BACK : RAY_FRONT;
}

// AO only needs opacity and the geometric hit position. In particular, accepted
// cutouts already have binary alpha = 1: do not fetch their material a second time.
uint trace_ray_ao(vec3 p_from, vec3 p_to, out float r_opacity, out vec3 r_position) {
	float distance;
	vec3 normal;
	uint triangle;
	vec3 barycentric;
	vec4 unused_albedo;
	uint hit = trace_ray(p_from, p_to, false, false, distance, normal, triangle, barycentric, unused_albedo);
	if (hit == RAY_MISS) {
		return hit;
	}
	Triangle t = triangles.data[triangle];
	r_opacity = t.alpha_scissor_threshold >= 0.0 ? 1.0 : sample_triangle_albedo_alpha(triangle, barycentric).a;
	r_position = vertices.data[t.indices.x].position * barycentric.x + vertices.data[t.indices.y].position * barycentric.y + vertices.data[t.indices.z].position * barycentric.z;
	return hit;
}

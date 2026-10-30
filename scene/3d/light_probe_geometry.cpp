/**************************************************************************/
/*  light_probe_geometry.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "light_probe_geometry.h"

#include "core/math/face3.h"
#include "core/math/geometry_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

Error LightProbeGeometry::_append_mesh(const Ref<Mesh> &p_mesh, const Transform3D &p_transform, const Vector<Ref<Material>> &p_materials) {
	if (p_mesh.is_null()) {
		return OK;
	}
	ERR_FAIL_COND_V(!p_transform.is_finite(), ERR_INVALID_DATA);
	Vector<Vector3> faces;
	for (int surface = 0; surface < p_mesh->get_surface_count(); surface++) {
		if (p_mesh->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		const Ref<Material> material = surface < p_materials.size() ? p_materials[surface] : p_mesh->surface_get_material(surface);
		const Ref<BaseMaterial3D> base_material = material;
		// Without sampling alpha textures, transparent/cutout surfaces cannot
		// safely define a closed solid. Shader materials remain conservative.
		if (base_material.is_valid() && base_material->get_transparency() != BaseMaterial3D::TRANSPARENCY_DISABLED) {
			continue;
		}
		const Array arrays = p_mesh->surface_get_arrays(surface);
		ERR_FAIL_COND_V(arrays.size() != Mesh::ARRAY_MAX, ERR_INVALID_DATA);
		const PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
		const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
		const int count = indices.is_empty() ? vertices.size() : indices.size();
		ERR_FAIL_COND_V(count % 3 != 0, ERR_INVALID_DATA);
		for (int i = 0; i < count; i += 3) {
			Vector3 triangle[3];
			for (int corner = 0; corner < 3; corner++) {
				const int index = indices.is_empty() ? i + corner : indices[i + corner];
				ERR_FAIL_INDEX_V(index, vertices.size(), ERR_INVALID_DATA);
				triangle[corner] = p_transform.xform(vertices[index]);
				ERR_FAIL_COND_V(!triangle[corner].is_finite(), ERR_INVALID_DATA);
			}
			if (!Face3(triangle[0], triangle[1], triangle[2]).is_degenerate()) {
				for (const Vector3 &point : triangle) {
					faces.push_back(point);
				}
			}
		}
	}
	if (faces.is_empty()) {
		return OK;
	}
	Geometry geometry;
	geometry.mesh.instantiate();
	geometry.mesh->create(faces);
	ERR_FAIL_COND_V(!geometry.mesh->is_valid(), ERR_INVALID_DATA);
	geometry.bounds = geometry.mesh->get_aabb();
	const Vector<TriangleMesh::Triangle> &triangles = geometry.mesh->get_triangles();
	const Vector<Vector3> &vertices = geometry.mesh->get_vertices();
	struct Edge {
		int face = -1;
		int count = 0;
		int orientation = 0;
	};
	HashMap<uint64_t, Edge> edges;
	LocalVector<int> parents;
	parents.resize(triangles.size());
	for (int i = 0; i < triangles.size(); i++) {
		parents[i] = i;
	}
	auto root = [&](int p_face) {
		while (parents[p_face] != p_face) {
			parents[p_face] = parents[parents[p_face]];
			p_face = parents[p_face];
		}
		return p_face;
	};
	for (int i = 0; i < triangles.size(); i++) {
		for (int edge = 0; edge < 3; edge++) {
			const uint32_t a = triangles[i].indices[edge];
			const uint32_t b = triangles[i].indices[(edge + 1) % 3];
			const uint64_t key = (uint64_t(MIN(a, b)) << 32) | MAX(a, b);
			Edge *entry = edges.getptr(key);
			if (!entry) {
				Edge value;
				value.face = i;
				value.count = 1;
				value.orientation = a < b ? 1 : -1;
				edges.insert(key, value);
			} else {
				entry->count++;
				entry->orientation += a < b ? 1 : -1;
				parents[root(i)] = root(entry->face);
			}
		}
	}
	LocalVector<bool> closed;
	closed.resize(triangles.size());
	for (bool &value : closed) {
		value = true;
	}
	for (const KeyValue<uint64_t, Edge> &entry : edges) {
		if (entry.value.count != 2 || entry.value.orientation != 0) {
			closed[root(entry.value.face)] = false;
		}
	}
	for (int i = 0; i < triangles.size(); i++) {
		const TriangleMesh::Triangle &triangle = triangles[i];
		if (Face3(vertices[triangle.indices[0]], vertices[triangle.indices[1]], vertices[triangle.indices[2]]).is_degenerate()) {
			closed[root(i)] = false;
		}
	}
	geometry.closed_faces.resize(triangles.size());
	for (int i = 0; i < triangles.size(); i++) {
		geometry.closed_faces.write[i] = closed[root(i)];
		geometry.has_closed_faces |= closed[root(i)];
	}
	geometries.push_back(geometry);
	return OK;
}

Error LightProbeGeometry::_collect(Node *p_node, Lightmapper::BakeStepFunc p_step, void *p_userdata) {
	MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(p_node);
	Node3D *spatial = Object::cast_to<Node3D>(p_node);
	if (spatial && spatial->is_visible_in_tree()) {
		if (p_step && p_step(0.14, RTR("Preparing probe validation geometry"), p_userdata, false)) {
			return ERR_SKIP;
		}
		if (instance && instance->get_gi_mode() == GeometryInstance3D::GI_MODE_STATIC) {
			const Ref<Mesh> mesh = instance->get_mesh();
			Vector<Ref<Material>> materials;
			for (int i = 0; mesh.is_valid() && i < mesh->get_surface_count(); i++) {
				materials.push_back(instance->get_active_material(i));
			}
			const Error error = _append_mesh(mesh, instance->get_global_transform(), materials);
			if (error != OK) {
				return error;
			}
		} else if (!instance && spatial->has_method(SNAME("get_bake_meshes"))) {
			const Array meshes = spatial->call("get_bake_meshes");
			for (int i = 0; i + 1 < meshes.size(); i += 2) {
				const Ref<Mesh> mesh = meshes[i];
				const Transform3D transform = meshes[i + 1];
				const Error error = _append_mesh(mesh, spatial->get_global_transform() * transform, Vector<Ref<Material>>());
				if (error != OK) {
					return error;
				}
			}
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *child = p_node->get_child(i);
		if (child->get_owner()) {
			const Error error = _collect(child, p_step, p_userdata);
			if (error != OK) {
				return error;
			}
		}
	}
	return OK;
}

Error LightProbeGeometry::build(Node *p_root, Lightmapper::BakeStepFunc p_step, void *p_userdata) {
	ERR_FAIL_NULL_V(p_root, ERR_INVALID_PARAMETER);
	broadphase.clear();
	geometries.clear();
	const Error error = _collect(p_root, p_step, p_userdata);
	if (error != OK) {
		return error;
	}
	// Addresses become stable only after scene collection is finished.
	for (Geometry &geometry : geometries) {
		broadphase.insert(geometry.bounds.grow(0.0002), &geometry);
	}
	return OK;
}

bool LightProbeGeometry::_near_surface(const Geometry &p_geometry, const Vector3 &p_position, real_t p_clearance) {
	const Vector<TriangleMesh::BVH> &nodes = p_geometry.mesh->get_bvh();
	const Vector<TriangleMesh::Triangle> &triangles = p_geometry.mesh->get_triangles();
	const Vector<Vector3> &vertices = p_geometry.mesh->get_vertices();
	LocalVector<int> stack;
	stack.push_back(nodes.size() - 1);
	const real_t limit = p_clearance * p_clearance;
	while (!stack.is_empty()) {
		const int index = stack[stack.size() - 1];
		stack.remove_at(stack.size() - 1);
		const TriangleMesh::BVH &node = nodes[index];
		if (p_position.distance_squared_to(p_position.clamp(node.aabb.position, node.aabb.get_end())) > limit) {
			continue;
		}
		if (node.face_index < 0) {
			stack.push_back(node.left);
			stack.push_back(node.right);
			continue;
		}
		const TriangleMesh::Triangle &triangle = triangles[node.face_index];
		const Face3 face(vertices[triangle.indices[0]], vertices[triangle.indices[1]], vertices[triangle.indices[2]]);
		if (face.is_degenerate()) {
			for (int edge = 0; edge < 3; edge++) {
				if (p_position.distance_squared_to(Geometry3D::get_closest_point_to_segment(p_position, face.vertex[edge], face.vertex[(edge + 1) % 3])) <= limit) {
					return true;
				}
			}
		} else if (p_position.distance_squared_to(face.get_closest_point_to(p_position)) <= limit) {
			return true;
		}
	}
	return false;
}

LightProbeVolume::ValidationStatus LightProbeGeometry::_inside(const Geometry &p_geometry, const Vector3 &p_position) {
	if (!p_geometry.has_closed_faces || !p_geometry.bounds.has_point(p_position)) {
		return LightProbeVolume::VALIDATION_VALID;
	}
	const Vector<TriangleMesh::BVH> &nodes = p_geometry.mesh->get_bvh();
	const Vector<TriangleMesh::Triangle> &triangles = p_geometry.mesh->get_triangles();
	const Vector<Vector3> &vertices = p_geometry.mesh->get_vertices();
	static const Vector3 directions[] = { Vector3(1, 0.371, 0.529).normalized(), Vector3(-0.413, 1, 0.193).normalized(), Vector3(0.239, -0.617, 1).normalized(), Vector3(-1, -0.283, 0.743).normalized(), Vector3(0.683, 1, -0.397).normalized() };
	int valid_rays = 0;
	bool inside = false;
	LocalVector<int> stack;
	for (const Vector3 &direction : directions) {
		int winding = 0;
		bool ambiguous = false;
		stack.clear();
		stack.push_back(nodes.size() - 1);
		while (!stack.is_empty()) {
			const int index = stack[stack.size() - 1];
			stack.remove_at(stack.size() - 1);
			const TriangleMesh::BVH &node = nodes[index];
			if (!node.aabb.grow(0.000001).intersects_ray(p_position, direction)) {
				continue;
			}
			if (node.face_index < 0) {
				stack.push_back(node.left);
				stack.push_back(node.right);
				continue;
			}
			if (!p_geometry.closed_faces[node.face_index]) {
				continue;
			}
			const TriangleMesh::Triangle &triangle = triangles[node.face_index];
			const Vector3 a = vertices[triangle.indices[0]];
			const Vector3 e1 = vertices[triangle.indices[1]] - a;
			const Vector3 e2 = vertices[triangle.indices[2]] - a;
			const Vector3 h = direction.cross(e2);
			const real_t determinant = e1.dot(h);
			if (Math::abs(determinant) < 1e-12) {
				continue;
			}
			const Vector3 offset = p_position - a;
			const real_t u = offset.dot(h) / determinant;
			const Vector3 q = offset.cross(e1);
			const real_t v = direction.dot(q) / determinant;
			const real_t distance = e2.dot(q) / determinant;
			if (u < 0 || v < 0 || u + v > 1 || distance <= 0) {
				continue;
			}
			if (MIN(u, MIN(v, 1 - u - v)) < 0.000001) {
				ambiguous = true; // Retry a direction that avoids a shared edge.
				break;
			}
			winding += e1.cross(e2).dot(direction) > 0 ? 1 : -1;
		}
		if (!ambiguous) {
			const bool ray_inside = winding != 0;
			if (valid_rays > 0 && inside != ray_inside) {
				return LightProbeVolume::VALIDATION_AMBIGUOUS;
			}
			inside = ray_inside;
			if (++valid_rays == 3) {
				return inside ? LightProbeVolume::VALIDATION_INSIDE_GEOMETRY : LightProbeVolume::VALIDATION_VALID;
			}
		}
	}
	return LightProbeVolume::VALIDATION_AMBIGUOUS;
}

LightProbeVolume::ValidationStatus LightProbeGeometry::validate(const Vector3 &p_position, real_t p_clearance) {
	// TriangleMesh welds positions at 0.0001 units. Keep surface rejection
	// conservative at that precision, even when requested clearance is zero.
	const real_t clearance = MAX(p_clearance, real_t(0.0002));
	const Vector3 extent(clearance, clearance, clearance);
	LightProbeVolume::ValidationStatus result = LightProbeVolume::VALIDATION_VALID;
	auto query = [&](void *p_data) {
		const Geometry &geometry = *static_cast<Geometry *>(p_data);
		if (_near_surface(geometry, p_position, clearance)) {
			result = LightProbeVolume::VALIDATION_TOO_CLOSE;
			return true;
		}
		const LightProbeVolume::ValidationStatus status = _inside(geometry, p_position);
		if (status == LightProbeVolume::VALIDATION_INSIDE_GEOMETRY) {
			result = status;
			return true;
		}
		if (status == LightProbeVolume::VALIDATION_AMBIGUOUS) {
			result = status;
		}
		return false;
	};
	broadphase.aabb_query(AABB(p_position - extent, extent * 2.0), query);
	return result;
}

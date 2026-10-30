/**************************************************************************/
/*  delaunay_3d_incremental.cpp                                           */
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

#include "delaunay_3d_incremental.h"

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

namespace {
using Tet = Delaunay3D::OutputSimplex;
using Faces = HashMap<Vector3i, int>;

Vector3i face_key(uint32_t p_a, uint32_t p_b, uint32_t p_c) {
	if (p_a > p_b) {
		SWAP(p_a, p_b);
	}
	if (p_b > p_c) {
		SWAP(p_b, p_c);
	}
	if (p_a > p_b) {
		SWAP(p_a, p_b);
	}
	return Vector3i(p_a, p_b, p_c);
}

bool boundary(const Vector<Tet> &p_tets, Faces &r_faces) {
	r_faces.clear();
	for (const Tet &tet : p_tets) {
		for (int opposite = 0; opposite < 4; opposite++) {
			uint32_t vertices[3];
			int count = 0;
			for (int j = 0; j < 4; j++) {
				if (j != opposite) {
					vertices[count++] = tet.points[j];
				}
			}
			const Vector3i key = face_key(vertices[0], vertices[1], vertices[2]);
			int &uses = r_faces[key];
			if (++uses > 2) {
				return false;
			}
		}
	}
	Vector<Vector3i> internal;
	for (const KeyValue<Vector3i, int> &face : r_faces) {
		if (face.value == 2) {
			internal.push_back(face.key);
		}
	}
	for (const Vector3i &key : internal) {
		r_faces.erase(key);
	}
	return !r_faces.is_empty();
}

bool same_boundary(const Vector<Tet> &p_before, const Vector<Tet> &p_after) {
	Faces before;
	Faces after;
	if (!boundary(p_before, before) || !boundary(p_after, after) || before.size() != after.size()) {
		return false;
	}
	for (const KeyValue<Vector3i, int> &face : before) {
		if (!after.has(face.key)) {
			return false;
		}
	}
	return true;
}

double volume6(const Vector<Vector3> &p_points, const Tet &p_tet) {
	const Vector3 a = p_points[p_tet.points[0]];
	return Math::abs(double((p_points[p_tet.points[1]] - a).dot((p_points[p_tet.points[2]] - a).cross(p_points[p_tet.points[3]] - a))));
}

bool contains(const Vector<Vector3> &p_points, const Tet &p_tet, const Vector3 &p_position) {
	const Vector3 a = p_points[p_tet.points[0]];
	const Vector3 b = p_points[p_tet.points[1]] - a;
	const Vector3 c = p_points[p_tet.points[2]] - a;
	const Vector3 d = p_points[p_tet.points[3]] - a;
	const double determinant = b.dot(c.cross(d));
	if (Math::abs(determinant) < 1e-20) {
		return false;
	}
	const Vector3 offset = p_position - a;
	const double x = offset.dot(c.cross(d)) / determinant;
	const double y = b.dot(offset.cross(d)) / determinant;
	const double z = b.dot(c.cross(offset)) / determinant;
	return MIN(x, MIN(y, z)) >= -1e-6 && x + y + z <= 1.000001;
}

bool same_volume(const Vector<Vector3> &p_points, const Vector<Tet> &p_before, const Vector<Tet> &p_after) {
	double before = 0;
	double after = 0;
	for (const Tet &tet : p_before) {
		before += volume6(p_points, tet);
	}
	for (const Tet &tet : p_after) {
		after += volume6(p_points, tet);
	}
	return before > 0 && Math::abs(before - after) <= before * 1e-5;
}
} // namespace

bool Delaunay3DIncremental::update(const Vector<Vector3> &p_previous_points, const PackedInt32Array &p_previous_tetrahedra, const Vector<Vector3> &p_points, Vector<Delaunay3D::OutputSimplex> &r_simplices, Statistics &r_statistics) {
	r_statistics = Statistics();
	auto reject = [&](const char *p_reason) {
		r_statistics.fallback_reason = p_reason;
		return false;
	};
	if (p_previous_points.size() < 4 || p_points.size() < 4 || p_previous_tetrahedra.is_empty() || p_previous_tetrahedra.size() % 4 != 0) {
		return false;
	}
	HashMap<Vector3, int> previous_indices;
	HashMap<Vector3, int> new_indices;
	AABB bounds(p_previous_points[0], Vector3());
	for (int i = 0; i < p_previous_points.size(); i++) {
		if (!p_previous_points[i].is_finite() || previous_indices.has(p_previous_points[i])) {
			return reject("duplicate_or_non_finite_positions");
		}
		previous_indices.insert(p_previous_points[i], i);
		bounds.expand_to(p_previous_points[i]);
	}
	const AABB previous_bounds = bounds;
	AABB next_bounds(p_points[0], Vector3());
	for (int i = 0; i < p_points.size(); i++) {
		next_bounds.expand_to(p_points[i]);
		if (!p_points[i].is_finite() || new_indices.has(p_points[i])) {
			return reject("duplicate_or_non_finite_positions");
		}
		new_indices.insert(p_points[i], i);
		bounds.expand_to(p_points[i]);
	}
	// Retain the exact quantization frame used by the previous full bake.
	// A changed extent, like a moved hull vertex, requires a fresh solve.
	if (next_bounds != previous_bounds) {
		return reject("changed_bounds");
	}
	Vector<int> removed;
	Vector<int> inserted;
	for (int i = 0; i < p_previous_points.size(); i++) {
		if (!new_indices.has(p_previous_points[i])) {
			removed.push_back(i);
		}
	}
	for (int i = 0; i < p_points.size(); i++) {
		if (!previous_indices.has(p_points[i])) {
			inserted.push_back(i);
		}
	}
	// Large grid edits are cheaper and safer as a single full reconstruction.
	if (removed.size() + inserted.size() > 32) {
		return reject("large_edit");
	}
	const real_t scale = bounds.size[bounds.get_longest_axis_index()];
	if (scale <= 0) {
		return reject("degenerate_bounds");
	}
	Vector<Vector3> world_points = p_previous_points;
	for (int i : inserted) {
		world_points.push_back(p_points[i]);
	}
	Vector<Vector3> points;
	Vector<uint8_t> active;
	points.resize(world_points.size());
	active.resize(world_points.size());
	HashSet<Vector3> quantized;
	for (int i = 0; i < world_points.size(); i++) {
		points.write[i] = Vector3(Vector3i((world_points[i] - bounds.position) / scale * 65536)) / 65536;
		if (quantized.has(points[i])) {
			return reject("quantized_duplicate");
		}
		quantized.insert(points[i]);
		active.write[i] = i < p_previous_points.size();
	}
	Vector<Tet> tets;
	tets.resize(p_previous_tetrahedra.size() / 4);
	for (int i = 0; i < tets.size(); i++) {
		for (int j = 0; j < 4; j++) {
			const int index = p_previous_tetrahedra[i * 4 + j];
			if (index < 0 || index >= p_previous_points.size()) {
				return reject("invalid_previous_tetrahedra");
			}
			tets.write[i].points[j] = index;
		}
	}
	Delaunay3D::Statistics predicate_statistics;
	auto empty_sphere = [&](const Tet &p_tet) {
		Delaunay3D::Simplex sphere(p_tet.points[0], p_tet.points[1], p_tet.points[2], p_tet.points[3]);
		if (Delaunay3D::simplex_is_coplanar(world_points.ptr(), sphere)) {
			return false;
		}
		Delaunay3D::circum_sphere_compute(points.ptr(), &sphere);
		for (int i = 0; i < points.size(); i++) {
			if (!active[i] || uint32_t(i) == p_tet.points[0] || uint32_t(i) == p_tet.points[1] || uint32_t(i) == p_tet.points[2] || uint32_t(i) == p_tet.points[3]) {
				continue;
			}
			if (Delaunay3D::simplex_contains(points.ptr(), sphere, i, predicate_statistics)) {
				return false;
			}
		}
		return true;
	};
	for (int vertex : removed) {
		Vector<Tet> cavity;
		Vector<Tet> retained;
		HashSet<uint32_t> neighbors;
		for (const Tet &tet : tets) {
			bool incident = false;
			for (uint32_t point : tet.points) {
				incident |= point == uint32_t(vertex);
			}
			if (!incident) {
				retained.push_back(tet);
				continue;
			}
			cavity.push_back(tet);
			for (uint32_t point : tet.points) {
				if (point != uint32_t(vertex)) {
					neighbors.insert(point);
				}
			}
		}
		if (cavity.is_empty() || neighbors.size() > 128) {
			return reject("large_or_missing_neighborhood");
		}
		Vector<Vector3> local_points;
		Vector<uint32_t> local_indices;
		for (uint32_t point : neighbors) {
			local_indices.push_back(point);
		}
		local_indices.sort();
		for (uint32_t point : local_indices) {
			local_points.push_back(world_points[point]);
		}
		const Vector<Tet> local_tets = Delaunay3D::tetrahedralize(local_points);
		Vector<Tet> replacement;
		active.write[vertex] = false;
		for (const Tet &local : local_tets) {
			Tet tet;
			Vector3 center;
			for (int j = 0; j < 4; j++) {
				tet.points[j] = local_indices[local.points[j]];
				center += points[tet.points[j]] * 0.25;
			}
			bool inside = false;
			for (const Tet &old : cavity) {
				if (contains(points, old, center)) {
					inside = true;
					break;
				}
			}
			if (!inside) {
				continue;
			}
			if (!empty_sphere(tet)) {
				return reject("non_delaunay_or_degenerate_cavity");
			}
			replacement.push_back(tet);
		}
		if (!same_boundary(cavity, replacement) || !same_volume(points, cavity, replacement)) {
			return reject("cavity_boundary_or_volume_mismatch");
		}
		retained.append_array(replacement);
		tets = retained;
		r_statistics.removed_points++;
		r_statistics.removed_tetrahedra += cavity.size();
		r_statistics.inserted_tetrahedra += replacement.size();
	}
	for (int offset = 0; offset < inserted.size(); offset++) {
		const uint32_t vertex = p_previous_points.size() + offset;
		Vector<Tet> cavity;
		Vector<Tet> retained;
		bool inside = false;
		for (const Tet &tet : tets) {
			inside |= contains(points, tet, points[vertex]);
			Delaunay3D::Simplex sphere(tet.points[0], tet.points[1], tet.points[2], tet.points[3]);
			Delaunay3D::circum_sphere_compute(points.ptr(), &sphere);
			if (Delaunay3D::simplex_contains(points.ptr(), sphere, vertex, predicate_statistics)) {
				cavity.push_back(tet);
			} else {
				retained.push_back(tet);
			}
		}
		if (!inside || cavity.is_empty()) {
			return reject("outside_or_missing_cavity");
		}
		Faces faces;
		if (!boundary(cavity, faces)) {
			return reject("non_manifold_cavity");
		}
		Vector<Vector3i> ordered_faces;
		for (const KeyValue<Vector3i, int> &face : faces) {
			ordered_faces.push_back(face.key);
		}
		ordered_faces.sort();
		Vector<Tet> replacement;
		active.write[vertex] = true;
		for (const Vector3i &face : ordered_faces) {
			Tet tet = { { uint32_t(face.x), uint32_t(face.y), uint32_t(face.z), vertex } };
			if (!empty_sphere(tet)) {
				return reject("non_delaunay_or_degenerate_cavity");
			}
			replacement.push_back(tet);
		}
		if (!same_boundary(cavity, replacement) || !same_volume(points, cavity, replacement)) {
			return reject("cavity_boundary_or_volume_mismatch");
		}
		retained.append_array(replacement);
		tets = retained;
		r_statistics.inserted_points++;
		r_statistics.removed_tetrahedra += cavity.size();
		r_statistics.inserted_tetrahedra += replacement.size();
	}
	for (Tet &tet : tets) {
		for (uint32_t &point : tet.points) {
			const int *index = new_indices.getptr(world_points[point]);
			if (!index) {
				return reject("point_mapping_changed");
			}
			point = *index;
		}
	}
	r_statistics.fallback_reason = "none";
	r_simplices = tets;
	return true;
}

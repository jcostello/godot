/**************************************************************************/
/*  lightmap_probe_capture.cpp                                            */
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

#include "lightmap_probe_capture.h"

#include "core/math/face3.h"

#include <limits>

Color LightmapProbeCapture::_weights(const Cell &p_cell, const Vector3 &p_point) {
	const Vector3 barycentric = p_cell.inverse.xform(p_point - p_cell.points[0]);
	return Color(1 - barycentric.x - barycentric.y - barycentric.z, barycentric.x, barycentric.y, barycentric.z);
}

void LightmapProbeCapture::clear() {
	index.clear();
	point_index.clear();
	probe_points.clear();
	point_bounds = AABB();
	for (const KeyValue<Key, Cell *> &entry : cells) {
		memdelete(entry.value);
	}
	cells.clear();
	bounds = AABB();
}

void LightmapProbeCapture::update(const PackedVector3Array &p_points, const PackedInt32Array &p_tetrahedra) {
	if (probe_points != p_points) {
		point_index.clear();
		probe_points = p_points;
		bool first_point = true;
		for (int i = 0; i < probe_points.size(); i++) {
			ERR_CONTINUE(!probe_points[i].is_finite());
			point_index.insert(AABB(probe_points[i], Vector3()), reinterpret_cast<void *>(uintptr_t(i + 1)));
			if (first_point) {
				point_bounds = AABB(probe_points[i], Vector3());
				first_point = false;
			} else {
				point_bounds.expand_to(probe_points[i]);
			}
		}
		if (first_point) {
			point_bounds = AABB();
		}
	}
	HashMap<Key, Cell *, KeyHasher> next;
	next.reserve(p_tetrahedra.size() / 4);
	bool first = true;
	for (int i = 0; i + 3 < p_tetrahedra.size(); i += 4) {
		int vertices[4];
		bool valid = true;
		for (int j = 0; j < 4; j++) {
			vertices[j] = p_tetrahedra[i + j];
			valid &= vertices[j] >= 0 && vertices[j] < p_points.size();
		}
		ERR_CONTINUE(!valid);
		for (int j = 1; j < 4; j++) {
			for (int k = j; k > 0 && p_points[vertices[k]] < p_points[vertices[k - 1]]; k--) {
				SWAP(vertices[k], vertices[k - 1]);
			}
		}
		Key key;
		for (int j = 0; j < 4; j++) {
			key.points[j] = p_points[vertices[j]];
			valid &= key.points[j].is_finite();
		}
		ERR_CONTINUE(!valid);
		if (next.has(key)) {
			continue;
		}
		Cell *cell = nullptr;
		Cell **previous = cells.getptr(key);
		if (previous) {
			cell = *previous;
			cells.erase(key);
		} else {
			const Basis basis(key.points[1] - key.points[0], key.points[2] - key.points[0], key.points[3] - key.points[0]);
			if (basis.determinant() == 0) {
				continue;
			}
			cell = memnew(Cell);
			cell->inverse = basis.inverse();
			AABB box(key.points[0], Vector3());
			for (int j = 0; j < 4; j++) {
				cell->points[j] = key.points[j];
				box.expand_to(key.points[j]);
			}
			cell->id = index.insert(box.grow(MAX(real_t(0.00001), box.size.length() * real_t(0.00001))), cell);
		}
		for (int j = 0; j < 4; j++) {
			cell->indices[j] = vertices[j];
		}
		cell->order = i / 4;
		next.insert(key, cell);
		for (const Vector3 &point : key.points) {
			if (first) {
				bounds = AABB(point, Vector3());
				first = false;
			} else {
				bounds.expand_to(point);
			}
		}
	}
	for (const KeyValue<Key, Cell *> &entry : cells) {
		index.remove(entry.value->id);
		memdelete(entry.value);
	}
	cells = std::move(next);
	if (first) {
		bounds = AABB();
	}
}

bool LightmapProbeCapture::_sample_nearest_probes(const Vector3 &p_point, const PackedColorArray &p_sh, Color *r_sh) const {
	if (probe_points.is_empty() || point_index.is_empty()) {
		return false;
	}
	struct Candidate {
		int id;
		real_t distance_squared;
	};
	struct CandidateOrder {
		bool operator()(const Candidate &p_a, const Candidate &p_b) const {
			return p_a.distance_squared == p_b.distance_squared ? p_a.id < p_b.id : p_a.distance_squared < p_b.distance_squared;
		}
	};
	int selected[4];
	double weights[4];
	int selected_count = 0;
	Candidate candidates[4];
	const real_t maximum_radius = p_point.distance_to(point_bounds.get_center()) + point_bounds.size.length() + 0.0001;
	real_t radius = MAX(real_t(0.0001), point_bounds.size.length() / 64);
	radius = MAX(radius, p_point.distance_to(p_point.clamp(point_bounds.position, point_bounds.get_end())));
	for (int attempt = 0; attempt < 32; attempt++) {
		radius = MIN(radius, maximum_radius);
		int candidate_count = 0;
		auto collect = [&](void *p_data) {
			const int id = int(reinterpret_cast<uintptr_t>(p_data)) - 1;
			if (int64_t(id) * 9 + 8 >= p_sh.size()) {
				return false;
			}
			for (int i = 0; i < selected_count; i++) {
				if (selected[i] == id) {
					return false;
				}
			}
			const Candidate candidate = { id, probe_points[id].distance_squared_to(p_point) };
			if (candidate.distance_squared <= radius * radius) {
				// Only four neighbors are used. Keep a bounded, sorted selection
				// instead of allocating and sorting every point in the query.
				int position = candidate_count;
				while (position > 0 && CandidateOrder()(candidate, candidates[position - 1])) {
					position--;
				}
				if (position < 4) {
					for (int i = MIN(candidate_count, 3); i > position; i--) {
						candidates[i] = candidates[i - 1];
					}
					candidates[position] = candidate;
					candidate_count = MIN(candidate_count + 1, 4);
				}
			}
			return false;
		};
		const Vector3 extent(radius, radius, radius);
		point_index.aabb_query(AABB(p_point - extent, extent * 2), collect);
		for (int i = 0; i < candidate_count; i++) {
			const Candidate &candidate = candidates[i];
			if (candidate.distance_squared == 0) {
				for (int j = 0; j < 9; j++) {
					r_sh[j] = p_sh[candidate.id * 9 + j];
				}
				return true;
			}
			selected[selected_count] = candidate.id;
			weights[selected_count++] = 1.0 / MAX(double(candidate.distance_squared), 1e-12);
			if (selected_count == 4) {
				break;
			}
		}
		// All points closer than this radius were visited. Expand until four
		// neighbors are found or the complete point set has been searched.
		if (selected_count == 4 || radius >= maximum_radius) {
			break;
		}
		radius *= 2;
	}
	if (selected_count == 0) {
		return false;
	}
	double sum = 0;
	for (int i = 0; i < selected_count; i++) {
		sum += weights[i];
	}
	for (int j = 0; j < 9; j++) {
		r_sh[j] = Color(0, 0, 0, 0);
		for (int i = 0; i < selected_count; i++) {
			r_sh[j] += p_sh[selected[i] * 9 + j] * float(weights[i] / sum);
		}
	}
	return true;
}

bool LightmapProbeCapture::sample(const Vector3 &p_point, const PackedColorArray &p_sh, Color *r_sh) const {
	if (!p_point.is_finite()) {
		return false;
	}
	if (cells.is_empty()) {
		return _sample_nearest_probes(p_point, p_sh, r_sh);
	}
	const Cell *selected = nullptr;
	Color weights;
	auto containing = [&](void *p_data) {
		const Cell *cell = static_cast<Cell *>(p_data);
		const Color barycentric = _weights(*cell, p_point);
		if (MIN(MIN(barycentric.r, barycentric.g), MIN(barycentric.b, barycentric.a)) >= -0.00001 && (!selected || cell->order < selected->order)) {
			selected = cell;
			weights = barycentric;
		}
		return false;
	};
	index.aabb_query(AABB(p_point, Vector3()), containing);
	if (!selected) {
		// Outside the convex hull, extend the closest tetrahedron's boundary
		// lighting. Branch queries grow until the best surface is within the
		// search radius, so cells outside that box cannot improve the result.
		real_t distance_squared = std::numeric_limits<real_t>::infinity();
		real_t radius = MAX(real_t(0.00001), bounds.size.length() / 64);
		auto closest = [&](void *p_data) {
			const Cell *cell = static_cast<Cell *>(p_data);
			for (int opposite = 0; opposite < 4; opposite++) {
				Vector3 face[3];
				int count = 0;
				for (int j = 0; j < 4; j++) {
					if (j != opposite) {
						face[count++] = cell->points[j];
					}
				}
				const Vector3 position = Face3(face[0], face[1], face[2]).get_closest_point_to(p_point);
				const real_t distance = position.distance_squared_to(p_point);
				if (distance < distance_squared || (distance == distance_squared && (!selected || cell->order < selected->order))) {
					distance_squared = distance;
					selected = cell;
					weights = _weights(*cell, position);
				}
			}
			return false;
		};
		// Start near the capture bounds even for objects far outside them.
		radius = MAX(radius, p_point.distance_to(p_point.clamp(bounds.position, bounds.get_end())));
		for (int attempt = 0; attempt < 32; attempt++) {
			const Vector3 extent(radius, radius, radius);
			index.aabb_query(AABB(p_point - extent, extent * 2), closest);
			if (selected && distance_squared <= radius * radius) {
				break;
			}
			radius *= 2;
		}
	}
	if (!selected) {
		return false;
	}
	float sum = 0;
	for (int j = 0; j < 4; j++) {
		weights[j] = CLAMP(weights[j], 0.0f, 1.0f);
		sum += weights[j];
	}
	if (sum <= 0) {
		return false;
	}
	for (int i = 0; i < 4; i++) {
		if (selected->indices[i] * 9 + 8 >= p_sh.size()) {
			return false;
		}
	}
	for (int j = 0; j < 9; j++) {
		r_sh[j] = Color(0, 0, 0, 0);
		for (int i = 0; i < 4; i++) {
			r_sh[j] += p_sh[selected->indices[i] * 9 + j] * (weights[i] / sum);
		}
	}
	return true;
}

LightmapProbeCapture::~LightmapProbeCapture() {
	clear();
}

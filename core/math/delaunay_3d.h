/**************************************************************************/
/*  delaunay_3d.h                                                         */
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

#pragma once

#include "core/math/aabb.h"
#include "core/math/dynamic_bvh.h"
#include "core/math/projection.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/templates/a_hash_map.h"
#include "core/templates/list.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"

#include <thirdparty/misc/r128.h>

#include <limits>

class Delaunay3D {
	friend class Delaunay3DIncremental;

public:
	struct Statistics {
		uint64_t sphere_tests = 0;
		uint64_t precise_sphere_tests = 0;
		uint64_t simplices_created = 0;
		uint64_t index_entries = 0;
		uint64_t peak_index_entries = 0;
	};

private:
	struct Simplex;

	enum {
		QUANTIZATION_MAX = 1 << 16 // A power of two smaller than the 23 bit significand of a float.
	};

	struct Simplex {
		uint32_t points[4];
		R128 circum_center_x;
		R128 circum_center_y;
		R128 circum_center_z;
		R128 circum_r2;
		// Conservative double filter of the existing fixed-point predicate.
		double filter_center[3];
		double filter_r2 = 0;
		double filter_error = 0;
		bool filter_safe = false;
		DynamicBVH::ID spatial_id;
		uint64_t insertion_order = 0;
		List<Simplex *>::Element *SE = nullptr;

		_FORCE_INLINE_ Simplex() {}
		_FORCE_INLINE_ Simplex(uint32_t p_a, uint32_t p_b, uint32_t p_c, uint32_t p_d) {
			points[0] = p_a;
			points[1] = p_b;
			points[2] = p_c;
			points[3] = p_d;
		}
	};

	struct SimplexInsertionOrder {
		_FORCE_INLINE_ bool operator()(const Simplex *p_a, const Simplex *p_b) const {
			return p_a->insertion_order < p_b->insertion_order;
		}
	};

	static AABB simplex_bounds(const Simplex &p_simplex) {
		// Only normalized input points are queried. Clip large spheres to the
		// unit cube so root/boundary simplices do not dominate BVH bounds.
		// Unsafe fixed-point spheres keep full coverage and use R128 queries.
		if (!p_simplex.filter_safe) {
			return AABB(Vector3(), Vector3(1, 1, 1));
		}
		const double radius = Math::sqrt(p_simplex.filter_r2 + p_simplex.filter_error);
		Vector3 lower;
		Vector3 upper;
		for (int axis = 0; axis < 3; axis++) {
			// Pad before converting to real_t, covering cancellation and float
			// rounding of both the stored position and its reconstructed end.
			lower[axis] = CLAMP(p_simplex.filter_center[axis] - radius - 0.000001, 0.0, 1.0);
			upper[axis] = CLAMP(p_simplex.filter_center[axis] + radius + 0.000001, 0.0, 1.0);
		}
		return AABB(lower, upper - lower);
	}

	struct Triangle {
		uint32_t triangle[3];
		bool bad = false;
		_FORCE_INLINE_ bool operator==(const Triangle &p_triangle) const {
			return triangle[0] == p_triangle.triangle[0] && triangle[1] == p_triangle.triangle[1] && triangle[2] == p_triangle.triangle[2];
		}

		_FORCE_INLINE_ Triangle() {}
		_FORCE_INLINE_ Triangle(uint32_t p_a, uint32_t p_b, uint32_t p_c) {
			if (p_a > p_b) {
				SWAP(p_a, p_b);
			}
			if (p_b > p_c) {
				SWAP(p_b, p_c);
			}
			if (p_a > p_b) {
				SWAP(p_a, p_b);
			}

			triangle[0] = p_a;
			triangle[1] = p_b;
			triangle[2] = p_c;
		}
	};

	struct TriangleHasher {
		_FORCE_INLINE_ static uint32_t hash(const Triangle &p_triangle) {
			uint32_t h = hash_djb2_one_32(p_triangle.triangle[0]);
			h = hash_djb2_one_32(p_triangle.triangle[1], h);
			return hash_fmix32(hash_djb2_one_32(p_triangle.triangle[2], h));
		}
	};

	_FORCE_INLINE_ static void circum_sphere_compute(const Vector3 *p_points, Simplex *p_simplex) {
		// The only part in the algorithm where there may be precision errors is this one,
		// so ensure that we do it with the maximum precision possible.

		R128 v0_x = p_points[p_simplex->points[0]].x;
		R128 v0_y = p_points[p_simplex->points[0]].y;
		R128 v0_z = p_points[p_simplex->points[0]].z;
		R128 v1_x = p_points[p_simplex->points[1]].x;
		R128 v1_y = p_points[p_simplex->points[1]].y;
		R128 v1_z = p_points[p_simplex->points[1]].z;
		R128 v2_x = p_points[p_simplex->points[2]].x;
		R128 v2_y = p_points[p_simplex->points[2]].y;
		R128 v2_z = p_points[p_simplex->points[2]].z;
		R128 v3_x = p_points[p_simplex->points[3]].x;
		R128 v3_y = p_points[p_simplex->points[3]].y;
		R128 v3_z = p_points[p_simplex->points[3]].z;

		// Create the rows of our "unrolled" 3x3 matrix.
		R128 row1_x = v1_x - v0_x;
		R128 row1_y = v1_y - v0_y;
		R128 row1_z = v1_z - v0_z;

		R128 row2_x = v2_x - v0_x;
		R128 row2_y = v2_y - v0_y;
		R128 row2_z = v2_z - v0_z;

		R128 row3_x = v3_x - v0_x;
		R128 row3_y = v3_y - v0_y;
		R128 row3_z = v3_z - v0_z;

		R128 sq_length1 = row1_x * row1_x + row1_y * row1_y + row1_z * row1_z;
		R128 sq_length2 = row2_x * row2_x + row2_y * row2_y + row2_z * row2_z;
		R128 sq_length3 = row3_x * row3_x + row3_y * row3_y + row3_z * row3_z;

		// Compute the determinant of said matrix.
		R128 determinant = row1_x * (row2_y * row3_z - row3_y * row2_z) - row2_x * (row1_y * row3_z - row3_y * row1_z) + row3_x * (row1_y * row2_z - row2_y * row1_z);

		// Compute the volume of the tetrahedron, and precompute a scalar quantity for reuse in the formula.
		R128 volume = determinant / R128(6.f);
		R128 i12volume = R128(1.f) / (volume * R128(12.f));

		R128 center_x = v0_x + i12volume * ((row2_y * row3_z - row3_y * row2_z) * sq_length1 - (row1_y * row3_z - row3_y * row1_z) * sq_length2 + (row1_y * row2_z - row2_y * row1_z) * sq_length3);
		R128 center_y = v0_y + i12volume * (-(row2_x * row3_z - row3_x * row2_z) * sq_length1 + (row1_x * row3_z - row3_x * row1_z) * sq_length2 - (row1_x * row2_z - row2_x * row1_z) * sq_length3);
		R128 center_z = v0_z + i12volume * ((row2_x * row3_y - row3_x * row2_y) * sq_length1 - (row1_x * row3_y - row3_x * row1_y) * sq_length2 + (row1_x * row2_y - row2_x * row1_y) * sq_length3);

		// Once we know the center, the radius is clearly the distance to any vertex.
		R128 rel1_x = center_x - v0_x;
		R128 rel1_y = center_y - v0_y;
		R128 rel1_z = center_z - v0_z;

		R128 radius1 = rel1_x * rel1_x + rel1_y * rel1_y + rel1_z * rel1_z;

		p_simplex->circum_center_x = center_x;
		p_simplex->circum_center_y = center_y;
		p_simplex->circum_center_z = center_z;
		p_simplex->circum_r2 = radius1;

		p_simplex->filter_center[0] = double(center_x);
		p_simplex->filter_center[1] = double(center_y);
		p_simplex->filter_center[2] = double(center_z);
		p_simplex->filter_r2 = double(radius1 - R128(0.0000000001));
		double center_squared = 0;
		for (double coordinate : p_simplex->filter_center) {
			center_squared += coordinate * coordinate;
		}
		// Queries are quantized points in [0, 1]^3. This deliberately loose
		// bound covers conversion/subtraction/product/sum rounding and the
		// sub-2^-64 rounding of each original R128 squared distance.
		p_simplex->filter_error = 128 * std::numeric_limits<double>::epsilon() * (center_squared + Math::abs(p_simplex->filter_r2) + 4);
		// Keep the original path for extreme spheres, including cases where
		// the fixed-point squared distance could overflow its integer part.
		p_simplex->filter_safe = center_squared < 1e16 && p_simplex->filter_r2 >= 0 && p_simplex->filter_r2 < 1e18;
	}

	_FORCE_INLINE_ static bool simplex_contains(const Vector3 *p_points, const Simplex &p_simplex, uint32_t p_vertex, Statistics &r_statistics) {
		r_statistics.sphere_tests++;
		if (p_simplex.filter_safe) {
			const double dx = p_simplex.filter_center[0] - double(p_points[p_vertex].x);
			const double dy = p_simplex.filter_center[1] - double(p_points[p_vertex].y);
			const double dz = p_simplex.filter_center[2] - double(p_points[p_vertex].z);
			const double difference = dx * dx + dy * dy + dz * dz - p_simplex.filter_r2;
			if (difference > p_simplex.filter_error) {
				return false;
			}
			if (difference < -p_simplex.filter_error) {
				return true;
			}
		}
		r_statistics.precise_sphere_tests++;
		R128 v_x = p_points[p_vertex].x;
		R128 v_y = p_points[p_vertex].y;
		R128 v_z = p_points[p_vertex].z;

		R128 rel2_x = p_simplex.circum_center_x - v_x;
		R128 rel2_y = p_simplex.circum_center_y - v_y;
		R128 rel2_z = p_simplex.circum_center_z - v_z;

		R128 radius2 = rel2_x * rel2_x + rel2_y * rel2_y + rel2_z * rel2_z;

		return radius2 < (p_simplex.circum_r2 - R128(0.0000000001));
		// When this tolerance is too big, it can result in overlapping simplices.
		// When it's too small, large amounts of planar simplices are created.
	}

	static bool simplex_is_coplanar(const Vector3 *p_points, const Simplex &p_simplex) {
		// Checking every possible distance like this is overkill, but only checking
		// one is not enough. If the simplex is almost planar then the vectors p1-p2
		// and p1-p3 can be practically collinear, which makes Plane unreliable.
		for (uint32_t i = 0; i < 4; i++) {
			Plane p(p_points[p_simplex.points[i]], p_points[p_simplex.points[(i + 1) % 4]], p_points[p_simplex.points[(i + 2) % 4]]);
			// This tolerance should not be smaller than the one used with
			// Plane::has_point() when creating the LightmapGI probe BSP tree.
			if (Math::abs(p.distance_to(p_points[p_simplex.points[(i + 3) % 4]])) < 0.001) {
				return true;
			}
		}

		return false;
	}

public:
	struct OutputSimplex {
		uint32_t points[4];
	};

	static Vector<OutputSimplex> tetrahedralize(const Vector<Vector3> &p_points, Statistics *r_statistics = nullptr) {
		Statistics statistics;
		if (r_statistics) {
			*r_statistics = statistics;
		}
		if (p_points.size() < 4) {
			return Vector<OutputSimplex>();
		}
		uint32_t point_count = p_points.size();
		Vector3 *points = (Vector3 *)memalloc(sizeof(Vector3) * (point_count + 4));
		const Vector3 *src_points = p_points.ptr();

		{
			AABB rect;
			for (uint32_t i = 0; i < point_count; i++) {
				Vector3 point = src_points[i];
				if (i == 0) {
					rect.position = point;
				} else {
					rect.expand_to(point);
				}
			}

			// Axis-aligned planar sets cannot produce a volume. Avoid dividing
			// by zero when normalizing points.
			if (rect.size.x == 0 || rect.size.y == 0 || rect.size.z == 0) {
				memfree(points);
				return Vector<OutputSimplex>();
			}
			real_t longest_axis = rect.size[rect.get_longest_axis_index()];

			for (uint32_t i = 0; i < point_count; i++) {
				// Scale points to the unit cube to better utilize R128 precision
				// and quantize to stabilize triangulation over a wide range of
				// distances.
				points[i] = Vector3(Vector3i((src_points[i] - rect.position) / longest_axis * QUANTIZATION_MAX)) / QUANTIZATION_MAX;
			}

			const real_t delta_max = Math::sqrt(2.0) * 100.0;
			Vector3 center = Vector3(0.5, 0.5, 0.5);

			// The larger the root simplex is, the more likely it is that the
			// triangulation is convex. If it's not absolutely huge, there can
			// be missing simplices that are not created for the outermost faces
			// of the point cloud if the point density is very low there.
			points[point_count + 0] = center + Vector3(0, 1, 0) * delta_max;
			points[point_count + 1] = center + Vector3(0, -1, 1) * delta_max;
			points[point_count + 2] = center + Vector3(1, -1, -1) * delta_max;
			points[point_count + 3] = center + Vector3(-1, -1, -1) * delta_max;
		}

		DynamicBVH spatial_index;
		List<Simplex *> simplex_list;
		{
			Simplex *root = memnew(Simplex(point_count + 0, point_count + 1, point_count + 2, point_count + 3));
			root->SE = simplex_list.push_back(root);
			root->insertion_order = statistics.simplices_created++;
			circum_sphere_compute(points, root);
			root->spatial_id = spatial_index.insert(simplex_bounds(*root), root);
			statistics.index_entries++;
			statistics.peak_index_entries = 1;
		}
		LocalVector<Simplex *> cavity;

		AHashMap<Triangle, uint32_t, TriangleHasher> triangles_inserted;
		LocalVector<Triangle> triangles;

		// Keep the last occurrence of each quantized point, exactly as the former
		// forward duplicate scan did. Input/output indices and insertion order stay
		// unchanged. Drop the hash table before constructing the tetrahedra.
		LocalVector<uint32_t> unique_point_indices;
		{
			AHashMap<Vector3, uint32_t> last_point_indices;
			last_point_indices.reserve(point_count);
			for (uint32_t i = 0; i < point_count; i++) {
				last_point_indices[points[i]] = i;
			}
			unique_point_indices.reserve(last_point_indices.size());
			for (uint32_t i = 0; i < point_count; i++) {
				if (*last_point_indices.getptr(points[i]) == i) {
					unique_point_indices.push_back(i);
				}
			}
		}

		for (const uint32_t i : unique_point_indices) {
			cavity.clear();
			auto collect = [&](void *p_data) {
				Simplex *simplex = static_cast<Simplex *>(p_data);
				if (simplex_contains(points, *simplex, i, statistics)) {
					cavity.push_back(simplex);
				}
				return false;
			};
			spatial_index.aabb_query(AABB(points[i], Vector3()), collect);
			// BVH traversal order is arbitrary. Preserve the former list order
			// for boundary-face creation, especially for cospherical grid points.
			cavity.sort_custom<SimplexInsertionOrder>();
			// Finish querying before deleting any BVH nodes or simplex data.
			for (Simplex *simplex : cavity) {
				static const uint32_t triangle_order[4][3] = {
					{ 0, 1, 2 },
					{ 0, 1, 3 },
					{ 0, 2, 3 },
					{ 1, 2, 3 },
				};

				for (uint32_t k = 0; k < 4; k++) {
					Triangle t = Triangle(simplex->points[triangle_order[k][0]], simplex->points[triangle_order[k][1]], simplex->points[triangle_order[k][2]]);
					uint32_t *p = triangles_inserted.getptr(t);
					if (p) {
						// This Delaunay implementation uses the Bowyer-Watson algorithm.
						// The rule is that you don't reuse any triangles that were
						// shared by any of the retriangulated simplices.
						triangles[*p].bad = true;
					} else {
						triangles_inserted.insert(t, triangles.size());
						triangles.push_back(t);
					}
				}

				simplex_list.erase(simplex->SE);

				spatial_index.remove(simplex->spatial_id);
				memdelete(simplex);
			}

			for (const Triangle &triangle : triangles) {
				if (triangle.bad) {
					continue;
				}
				Simplex *new_simplex = memnew(Simplex(triangle.triangle[0], triangle.triangle[1], triangle.triangle[2], i));
				circum_sphere_compute(points, new_simplex);
				new_simplex->insertion_order = statistics.simplices_created++;
				new_simplex->SE = simplex_list.push_back(new_simplex);
				new_simplex->spatial_id = spatial_index.insert(simplex_bounds(*new_simplex), new_simplex);
				statistics.index_entries++;
				statistics.peak_index_entries = MAX(statistics.peak_index_entries, uint64_t(spatial_index.get_leaf_count()));
			}

			triangles.clear();
			triangles_inserted.clear();
		}

		// Release index nodes before freeing their simplex userdata.
		spatial_index.clear();
		Vector<OutputSimplex> ret_simplices;
		ret_simplices.resize(simplex_list.size());
		OutputSimplex *ret_simplicesw = ret_simplices.ptrw();
		uint32_t simplices_written = 0;

		for (Simplex *simplex : simplex_list) {
			bool invalid = false;
			for (int j = 0; j < 4; j++) {
				if (simplex->points[j] >= point_count) {
					invalid = true;
					break;
				}
			}
			if (invalid || simplex_is_coplanar(src_points, *simplex)) {
				memdelete(simplex);
				continue;
			}

			ret_simplicesw[simplices_written].points[0] = simplex->points[0];
			ret_simplicesw[simplices_written].points[1] = simplex->points[1];
			ret_simplicesw[simplices_written].points[2] = simplex->points[2];
			ret_simplicesw[simplices_written].points[3] = simplex->points[3];
			simplices_written++;
			memdelete(simplex);
		}

		ret_simplices.resize(simplices_written);

		memfree(points);

		if (r_statistics) {
			*r_statistics = statistics;
		}
		return ret_simplices;
	}
};

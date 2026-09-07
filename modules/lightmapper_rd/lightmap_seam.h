/**************************************************************************/
/*  lightmap_seam.h                                                       */
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
#include "core/math/transform_2d.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"

// Geometry and atlas coordinates of an unmatched mesh edge. Positions are in bake space.
struct LightmapSeamEdge {
	// Allow small placement gaps without discarding short edges or overlaps.
	// Both thresholds are in bake space, independent of the scene extent.
	static constexpr real_t POSITION_TOLERANCE = 0.0015;
	static constexpr real_t MIN_SEGMENT_LENGTH = 0.0001;
	static constexpr real_t NORMAL_DOT_MIN = 0.999;

	Vector3 position[2];
	Vector3 normal[2];
	Vector2 uv[2];
	Vector3 opposite_vertex;
	Vector3 opposite_normal;
	Vector2 opposite_uv;
	uint32_t mesh = 0;
	uint32_t slice = 0;

	// Map UVs across the shared plane, preserving world-space distances even for
	// rotated, mirrored, sheared, or differently scaled lightmap charts.
	bool get_uv_transform(const LightmapSeamEdge &p_other, Transform2D &r_transform, bool p_unfold = false) const {
		if (p_unfold) {
			// Internal UV seams must share their endpoints and smooth normals.
			for (int i = 0; i < 2; i++) {
				if (position[i].distance_to(p_other.position[i]) > POSITION_TOLERANCE || normal[i].dot(p_other.normal[i]) < NORMAL_DOT_MIN) {
					return false;
				}
			}
		}
		const Vector3 tangent = (position[1] - position[0]).normalized();
		Vector3 inward = opposite_vertex - position[0];
		inward = (inward - tangent * inward.dot(tangent)).normalized();
		auto project = [&](const Vector3 &p_point) {
			const Vector3 delta = p_point - position[0];
			return Vector2(delta.dot(tangent), delta.dot(inward));
		};
		const Transform2D dst_uv(uv[1] - uv[0], opposite_uv - uv[0], uv[0]);
		const Transform2D dst_plane(project(position[1]), project(opposite_vertex), Vector2());
		Vector3 source_inward = p_other.opposite_vertex - p_other.position[0];
		source_inward = (source_inward - tangent * source_inward.dot(tangent)).normalized();
		if (p_unfold && inward.dot(source_inward) > NORMAL_DOT_MIN) {
			return false; // Overlapping faces extending to the same side.
		}
		auto project_source = [&](const Vector3 &p_point) {
			if (!p_unfold) {
				return project(p_point);
			}
			// Unfold the source onto the opposite side of the common edge,
			// preserving its intrinsic distances instead of foreshortening it.
			const Vector3 delta = p_point - p_other.position[0];
			return Vector2(delta.dot(tangent), -delta.dot(source_inward));
		};
		const Vector2 src_origin = project_source(p_other.position[0]);
		const Transform2D src_plane(project_source(p_other.position[1]) - src_origin, project_source(p_other.opposite_vertex) - src_origin, src_origin);
		const Transform2D src_uv(p_other.uv[1] - p_other.uv[0], p_other.opposite_uv - p_other.uv[0], p_other.uv[0]);
		// UV determinants can be very small for charts packed into large atlases.
		if (dst_uv.determinant() == 0 || src_uv.determinant() == 0 || dst_plane.determinant() == 0 || src_plane.determinant() == 0) {
			return false;
		}
		r_transform = src_uv * src_plane.affine_inverse() * dst_plane * dst_uv.affine_inverse();
		return true;
	}

	AABB get_aabb() const {
		AABB box(position[0], Vector3());
		box.expand_to(position[1]);
		return box.grow(POSITION_TOLERANCE);
	}

	// Return corresponding intervals on both edges, including partial overlaps and
	// reversed endpoints. No endpoint welding or matching tessellation is required.
	bool get_overlap(const LightmapSeamEdge &p_other, Vector2 &r_interval, Vector2 &r_other_interval) const {
		if (mesh == p_other.mesh) {
			return false;
		}
		const Vector3 delta = position[1] - position[0];
		const Vector3 other_delta = p_other.position[1] - p_other.position[0];
		const real_t length = delta.length();
		const real_t other_length = other_delta.length();
		if (length <= MIN_SEGMENT_LENGTH || other_length <= MIN_SEGMENT_LENGTH) {
			return false;
		}
		const Vector3 direction = delta / length;
		const Vector3 other_direction = other_delta / other_length;
		if (Math::abs(direction.dot(other_direction)) < 0.99999) {
			return false;
		}

		const real_t start = direction.dot(p_other.position[0] - position[0]);
		const real_t end = direction.dot(p_other.position[1] - position[0]);
		const real_t overlap_start = MAX(real_t(0.0), MIN(start, end));
		const real_t overlap_end = MIN(length, MAX(start, end));
		if (overlap_end - overlap_start <= MIN_SEGMENT_LENGTH) {
			return false;
		}

		// Faces must extend to opposite sides of the seam on approximately the same
		// plane. This rejects overlapping faces and hard corners even with smooth normals.
		Vector3 interior = opposite_vertex - position[0];
		interior = (interior - direction * interior.dot(direction)).normalized();
		Vector3 other_interior = p_other.opposite_vertex - p_other.position[0];
		other_interior = (other_interior - other_direction * other_interior.dot(other_direction)).normalized();
		if (interior.dot(other_interior) > -NORMAL_DOT_MIN) {
			return false;
		}

		r_interval = Vector2(overlap_start, overlap_end) / length;
		for (int i = 0; i < 2; i++) {
			const Vector3 point = position[0].lerp(position[1], r_interval[i]);
			r_other_interval[i] = CLAMP(other_direction.dot(point - p_other.position[0]) / other_length, real_t(0.0), real_t(1.0));
			const Vector3 other_point = p_other.position[0].lerp(p_other.position[1], r_other_interval[i]);
			if (point.distance_squared_to(other_point) > POSITION_TOLERANCE * POSITION_TOLERANCE) {
				return false;
			}
			const Vector3 n = normal[0].lerp(normal[1], r_interval[i]).normalized();
			const Vector3 other_n = p_other.normal[0].lerp(p_other.normal[1], r_other_interval[i]).normalized();
			if (n.dot(other_n) < NORMAL_DOT_MIN) {
				return false;
			}
		}
		return true;
	}
};

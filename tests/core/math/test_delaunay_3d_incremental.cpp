/**************************************************************************/
/*  test_delaunay_3d_incremental.cpp                                      */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_delaunay_3d_incremental)

#include "core/math/delaunay_3d_incremental.h"
#include "core/math/random_pcg.h"

namespace TestDelaunay3DIncremental {

// Unit cube corners plus jittered interior points, so the hull (and the
// quantization bounds) stay fixed while interior points are edited.
static Vector<Vector3> make_points(int p_interior_count) {
	Vector<Vector3> points;
	for (int i = 0; i < 8; i++) {
		points.push_back(Vector3(i & 1, (i >> 1) & 1, (i >> 2) & 1));
	}
	RandomPCG rng(42);
	for (int i = 0; i < p_interior_count; i++) {
		points.push_back(Vector3(0.05 + rng.randf() * 0.9, 0.05 + rng.randf() * 0.9, 0.05 + rng.randf() * 0.9));
	}
	return points;
}

static PackedInt32Array to_packed(const Vector<Delaunay3D::OutputSimplex> &p_simplices) {
	PackedInt32Array result;
	for (const Delaunay3D::OutputSimplex &simplex : p_simplices) {
		for (int j = 0; j < 4; j++) {
			result.push_back(simplex.points[j]);
		}
	}
	return result;
}

static double det3(double a, double b, double c, double d, double e, double f, double g, double h, double i) {
	return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}

// Checks indices, that the cells tile the unit cube without gaps or overlaps
// (total volume), and the empty circumsphere property against every other point.
static void check_delaunay(const Vector<Vector3> &p_points, const Vector<Delaunay3D::OutputSimplex> &p_simplices) {
	REQUIRE(!p_simplices.is_empty());
	double total_volume = 0;
	for (const Delaunay3D::OutputSimplex &simplex : p_simplices) {
		for (int j = 0; j < 4; j++) {
			REQUIRE(simplex.points[j] < uint32_t(p_points.size()));
		}
		const Vector3 a = p_points[simplex.points[0]];
		double r[3][3];
		double rhs[3];
		for (int k = 0; k < 3; k++) {
			const Vector3 e = p_points[simplex.points[k + 1]] - a;
			r[k][0] = e.x;
			r[k][1] = e.y;
			r[k][2] = e.z;
			rhs[k] = 0.5 * (double(e.x) * e.x + double(e.y) * e.y + double(e.z) * e.z);
		}
		const double det = det3(r[0][0], r[0][1], r[0][2], r[1][0], r[1][1], r[1][2], r[2][0], r[2][1], r[2][2]);
		CHECK(Math::abs(det) > 1e-12);
		total_volume += Math::abs(det) / 6.0;

		// Circumcenter relative to a, by Cramer's rule.
		const double cx = det3(rhs[0], r[0][1], r[0][2], rhs[1], r[1][1], r[1][2], rhs[2], r[2][1], r[2][2]) / det;
		const double cy = det3(r[0][0], rhs[0], r[0][2], r[1][0], rhs[1], r[1][2], r[2][0], rhs[2], r[2][2]) / det;
		const double cz = det3(r[0][0], r[0][1], rhs[0], r[1][0], r[1][1], rhs[1], r[2][0], r[2][1], rhs[2]) / det;
		const double radius_squared = cx * cx + cy * cy + cz * cz;
		for (int i = 0; i < p_points.size(); i++) {
			if (uint32_t(i) == simplex.points[0] || uint32_t(i) == simplex.points[1] || uint32_t(i) == simplex.points[2] || uint32_t(i) == simplex.points[3]) {
				continue;
			}
			const Vector3 p = p_points[i] - a;
			const double dx = p.x - cx;
			const double dy = p.y - cy;
			const double dz = p.z - cz;
			CHECK(dx * dx + dy * dy + dz * dz >= radius_squared * (1.0 - 1e-5));
		}
	}
	CHECK(total_volume == doctest::Approx(1.0).epsilon(1e-4));
}

TEST_CASE("[Delaunay3DIncremental] Unchanged points keep the tetrahedra") {
	const Vector<Vector3> points = make_points(40);
	const Vector<Delaunay3D::OutputSimplex> full = Delaunay3D::tetrahedralize(points);
	check_delaunay(points, full);

	Vector<Delaunay3D::OutputSimplex> result;
	Delaunay3DIncremental::Statistics statistics;
	REQUIRE(Delaunay3DIncremental::update(points, to_packed(full), points, result, statistics));
	CHECK(to_packed(result) == to_packed(full));
	CHECK(statistics.inserted_points == 0);
	CHECK(statistics.removed_points == 0);
}

TEST_CASE("[Delaunay3DIncremental] Inserting interior points stays Delaunay") {
	const Vector<Vector3> previous = make_points(40);
	const Vector<Delaunay3D::OutputSimplex> full = Delaunay3D::tetrahedralize(previous);

	Vector<Vector3> points = previous;
	points.push_back(Vector3(0.31, 0.47, 0.62));
	points.push_back(Vector3(0.73, 0.21, 0.38));

	Vector<Delaunay3D::OutputSimplex> result;
	Delaunay3DIncremental::Statistics statistics;
	REQUIRE_MESSAGE(Delaunay3DIncremental::update(previous, to_packed(full), points, result, statistics), statistics.fallback_reason);
	CHECK(statistics.inserted_points == 2);
	check_delaunay(points, result);
}

TEST_CASE("[Delaunay3DIncremental] Removing interior points stays Delaunay") {
	const Vector<Vector3> previous = make_points(40);
	const Vector<Delaunay3D::OutputSimplex> full = Delaunay3D::tetrahedralize(previous);

	// Removal also reorders the remaining points, which the result must follow.
	Vector<Vector3> points = previous;
	points.remove_at(20);
	points.remove_at(12);

	Vector<Delaunay3D::OutputSimplex> result;
	Delaunay3DIncremental::Statistics statistics;
	REQUIRE_MESSAGE(Delaunay3DIncremental::update(previous, to_packed(full), points, result, statistics), statistics.fallback_reason);
	CHECK(statistics.removed_points == 2);
	check_delaunay(points, result);
}

TEST_CASE("[Delaunay3DIncremental] Moving an interior point stays Delaunay") {
	const Vector<Vector3> previous = make_points(40);
	const Vector<Delaunay3D::OutputSimplex> full = Delaunay3D::tetrahedralize(previous);

	Vector<Vector3> points = previous;
	points.write[15] = Vector3(0.52, 0.44, 0.27);

	Vector<Delaunay3D::OutputSimplex> result;
	Delaunay3DIncremental::Statistics statistics;
	REQUIRE_MESSAGE(Delaunay3DIncremental::update(previous, to_packed(full), points, result, statistics), statistics.fallback_reason);
	CHECK(statistics.removed_points == 1);
	CHECK(statistics.inserted_points == 1);
	check_delaunay(points, result);
}

TEST_CASE("[Delaunay3DIncremental] Edits that need a full rebuild are rejected") {
	const Vector<Vector3> previous = make_points(40);
	const PackedInt32Array tetrahedra = to_packed(Delaunay3D::tetrahedralize(previous));
	Vector<Delaunay3D::OutputSimplex> result;
	Delaunay3DIncremental::Statistics statistics;

	SUBCASE("Changed hull bounds") {
		Vector<Vector3> points = previous;
		points.write[7] = Vector3(1.5, 1.0, 1.0);
		CHECK_FALSE(Delaunay3DIncremental::update(previous, tetrahedra, points, result, statistics));
		CHECK(String(statistics.fallback_reason) == "changed_bounds");
	}

	SUBCASE("Large edit") {
		Vector<Vector3> points = previous;
		RandomPCG rng(7);
		for (int i = 0; i < 33; i++) {
			points.push_back(Vector3(0.05 + rng.randf() * 0.9, 0.05 + rng.randf() * 0.9, 0.05 + rng.randf() * 0.9));
		}
		CHECK_FALSE(Delaunay3DIncremental::update(previous, tetrahedra, points, result, statistics));
		CHECK(String(statistics.fallback_reason) == "large_edit");
	}

	SUBCASE("Duplicate positions") {
		Vector<Vector3> points = previous;
		points.push_back(points[10]);
		CHECK_FALSE(Delaunay3DIncremental::update(previous, tetrahedra, points, result, statistics));
		CHECK(String(statistics.fallback_reason) == "duplicate_or_non_finite_positions");
	}

	SUBCASE("No previous tetrahedra") {
		CHECK_FALSE(Delaunay3DIncremental::update(previous, PackedInt32Array(), previous, result, statistics));
	}
}

} // namespace TestDelaunay3DIncremental

/**************************************************************************/
/*  test_lightmap_probe_capture.cpp                                       */
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

TEST_FORCE_LINK(test_lightmap_probe_capture)

#include "servers/rendering/storage/lightmap_probe_capture.h"

namespace TestLightmapProbeCapture {

// Probe i stores Color(i, j, 0, 0) in SH coefficient j, so the red channel of
// any sample is the weighted probe index and green identifies the coefficient.
static PackedColorArray make_sh(int p_probe_count) {
	PackedColorArray sh;
	for (int i = 0; i < p_probe_count; i++) {
		for (int j = 0; j < 9; j++) {
			sh.push_back(Color(i, j, 0, 0));
		}
	}
	return sh;
}

static void check_sh(const Color *p_sh, float p_probe) {
	for (int j = 0; j < 9; j++) {
		CHECK(p_sh[j].r == doctest::Approx(p_probe));
		CHECK(p_sh[j].g == doctest::Approx(j));
	}
}

static PackedVector3Array unit_tetrahedron() {
	return { Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1) };
}

TEST_CASE("[LightmapProbeCapture] Interpolates inside a tetrahedron") {
	LightmapProbeCapture capture;
	capture.update(unit_tetrahedron(), { 0, 1, 2, 3 });
	const PackedColorArray sh = make_sh(4);
	Color result[9];

	SUBCASE("Centroid weights all probes equally") {
		REQUIRE(capture.sample(Vector3(0.25, 0.25, 0.25), sh, result));
		check_sh(result, (0 + 1 + 2 + 3) / 4.0);
	}

	SUBCASE("A vertex returns its own probe") {
		REQUIRE(capture.sample(Vector3(0, 1, 0), sh, result));
		check_sh(result, 2);
	}

	SUBCASE("A point on an edge mixes only its two probes") {
		REQUIRE(capture.sample(Vector3(0.25, 0, 0), sh, result));
		check_sh(result, 0.75 * 0 + 0.25 * 1);
	}
}

TEST_CASE("[LightmapProbeCapture] Extends the closest boundary outside the hull") {
	LightmapProbeCapture capture;
	capture.update(unit_tetrahedron(), { 0, 1, 2, 3 });
	const PackedColorArray sh = make_sh(4);
	Color result[9];

	// The closest point of the hull to (-1, -1, -1) is probe 0.
	REQUIRE(capture.sample(Vector3(-1, -1, -1), sh, result));
	check_sh(result, 0);

	// Straight out of the x = 0 face, the result matches the face point below it.
	Color face[9];
	REQUIRE(capture.sample(Vector3(0, 0.2, 0.3), sh, face));
	REQUIRE(capture.sample(Vector3(-5, 0.2, 0.3), sh, result));
	for (int j = 0; j < 9; j++) {
		CHECK(result[j].is_equal_approx(face[j]));
	}
}

TEST_CASE("[LightmapProbeCapture] Uses the containing cell of a two-cell capture") {
	// Two tetrahedra sharing the face (1,0,0), (0,1,0), (0,0,1).
	PackedVector3Array points = unit_tetrahedron();
	points.push_back(Vector3(1, 1, 1));
	LightmapProbeCapture capture;
	capture.update(points, { 0, 1, 2, 3, 1, 2, 3, 4 });
	const PackedColorArray sh = make_sh(5);
	Color result[9];

	REQUIRE(capture.sample(Vector3(1, 1, 1), sh, result));
	check_sh(result, 4);
	REQUIRE(capture.sample(Vector3(0, 0, 0), sh, result));
	check_sh(result, 0);
	// The shared face only uses its three probes, from either side.
	REQUIRE(capture.sample(Vector3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0), sh, result));
	check_sh(result, (1 + 2 + 3) / 3.0);
}

TEST_CASE("[LightmapProbeCapture] Falls back to nearest probes without tetrahedra") {
	LightmapProbeCapture capture;
	capture.update(unit_tetrahedron(), PackedInt32Array());
	const PackedColorArray sh = make_sh(4);
	Color result[9];

	REQUIRE(capture.sample(Vector3(0, 0, 1), sh, result));
	check_sh(result, 3);

	// Inverse squared distance weights: 0.1875 to the origin, 0.6875 to the other three probes.
	REQUIRE(capture.sample(Vector3(0.25, 0.25, 0.25), sh, result));
	const double w0 = 1.0 / 0.1875;
	const double w1 = 1.0 / 0.6875;
	check_sh(result, (1 + 2 + 3) * w1 / (w0 + 3 * w1));
}

TEST_CASE("[LightmapProbeCapture] Rejects invalid input") {
	LightmapProbeCapture capture;
	const PackedColorArray sh = make_sh(4);
	Color result[9];

	SUBCASE("Empty capture") {
		CHECK_FALSE(capture.sample(Vector3(), sh, result));
	}

	SUBCASE("Non-finite sample point") {
		capture.update(unit_tetrahedron(), { 0, 1, 2, 3 });
		CHECK_FALSE(capture.sample(Vector3(Math::NaN, 0, 0), sh, result));
	}

	SUBCASE("Out of range and degenerate tetrahedra are skipped") {
		PackedVector3Array points = unit_tetrahedron();
		points.push_back(Vector3(2, 0, 0));
		ERR_PRINT_OFF;
		// Index 9 does not exist, and probes 0, 1, 4 and 2 all lie in the plane z = 0.
		capture.update(points, { 0, 1, 2, 9, 0, 1, 4, 2 });
		ERR_PRINT_ON;
		// With no valid cell left, sampling uses the nearest probes.
		REQUIRE(capture.sample(Vector3(0, 1, 0), sh, result));
		check_sh(result, 2);
	}

	SUBCASE("SH arrays smaller than the probe count") {
		capture.update(unit_tetrahedron(), { 0, 1, 2, 3 });
		CHECK_FALSE(capture.sample(Vector3(0.25, 0.25, 0.25), make_sh(3), result));
	}
}

} // namespace TestLightmapProbeCapture

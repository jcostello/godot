/**************************************************************************/
/*  test_environment_color_grading.cpp                                    */
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

#include "servers/rendering/storage/environment_color_grading.h"
#include "tests/test_macros.h"

TEST_FORCE_LINK(test_environment_color_grading)

namespace TestEnvironmentColorGrading {

TEST_CASE("[Rendering][Environment] Color grading neutral values") {
	const Vector3 neutral_temperature = EnvironmentColorGrading::temperature_balance(6500.0f);
	CHECK(neutral_temperature.is_equal_approx(Vector3(1.0f, 1.0f, 1.0f)));

	const Vector2 default_midtones = EnvironmentColorGrading::clamp_midtones_range(0.45f, 0.55f);
	CHECK(default_midtones.is_equal_approx(Vector2(0.45f, 0.55f)));
}

TEST_CASE("[Rendering][Environment] Color grading midtones stay ordered") {
	const Vector2 low_range = EnvironmentColorGrading::clamp_midtones_range(-1.0f, -1.0f);
	CHECK(low_range.x == doctest::Approx(0.0f));
	CHECK(low_range.y == doctest::Approx(0.0f));

	const Vector2 high_range = EnvironmentColorGrading::clamp_midtones_range(2.0f, 2.0f);
	CHECK(high_range.x == doctest::Approx(1.0f));
	CHECK(high_range.y == doctest::Approx(1.0f));
}

TEST_CASE("[Rendering][Environment] Hard tonal cuts allow empty ranges") {
	CHECK(EnvironmentColorGrading::clamp_midtones_range(0.0f, 0.0f).is_equal_approx(Vector2(0.0f, 0.0f)));
	CHECK(EnvironmentColorGrading::clamp_midtones_range(1.0f, 1.0f).is_equal_approx(Vector2(1.0f, 1.0f)));
	CHECK(EnvironmentColorGrading::clamp_midtones_range(0.5f, 0.5f).is_equal_approx(Vector2(0.5f, 0.5f)));
	CHECK(EnvironmentColorGrading::clamp_midtones_range(0.8f, 0.2f).is_equal_approx(Vector2(0.8f, 0.8f)));
}

TEST_CASE("[Rendering][Environment] Tonal softness is centered and independent") {
	const Vector2 centers(0.3f, 0.7f);
	const Vector2 softness(0.1f, 0.2f);
	CHECK(EnvironmentColorGrading::tonal_weights(0.25f, centers, softness).is_equal_approx(Vector3(1, 0, 0)));
	CHECK(EnvironmentColorGrading::tonal_weights(0.3f, centers, softness).is_equal_approx(Vector3(0.5f, 0.5f, 0)));
	CHECK(EnvironmentColorGrading::tonal_weights(0.35f, centers, softness).is_equal_approx(Vector3(0, 1, 0)));
	CHECK(EnvironmentColorGrading::tonal_weights(0.6f, centers, softness).is_equal_approx(Vector3(0, 1, 0)));
	CHECK(EnvironmentColorGrading::tonal_weights(0.7f, centers, softness).is_equal_approx(Vector3(0, 0.5f, 0.5f)));
	CHECK(EnvironmentColorGrading::tonal_weights(0.8f, centers, softness).is_equal_approx(Vector3(0, 0, 1)));
	// One transition may stay sharp while the other is soft.
	CHECK(EnvironmentColorGrading::tonal_weights(0.3f, centers, Vector2(0, 0.2f)).is_equal_approx(Vector3(0, 1, 0)));
	CHECK(EnvironmentColorGrading::tonal_weights(0.7f, centers, Vector2(0.1f, 0)).is_equal_approx(Vector3(0, 0, 1)));
}

TEST_CASE("[Rendering][Environment] Tonal softness preserves neutral weights when overlapping") {
	const Vector2 centers[] = { Vector2(0, 0), Vector2(0.45f, 0.55f), Vector2(0.5f, 0.5f), Vector2(1, 1) };
	const Vector2 widths[] = { Vector2(0, 0), Vector2(0, 1), Vector2(1, 0), Vector2(0.1f, 0.1f), Vector2(1, 1) };
	for (const Vector2 &center : centers) {
		for (const Vector2 &width : widths) {
			for (int i = -10; i <= 110; i++) {
				const Vector3 weights = EnvironmentColorGrading::tonal_weights(float(i) / 100.0f, center, width);
				CHECK(weights.x >= 0.0f);
				CHECK(weights.y >= 0.0f);
				CHECK(weights.z >= 0.0f);
				CHECK(weights.x + weights.y + weights.z == doctest::Approx(1.0f));
			}
		}
	}
	CHECK(EnvironmentColorGrading::tonal_weights(0.5f, Vector2(0.5f, 0.5f), Vector2()).is_equal_approx(Vector3(0, 0, 1)));
}

TEST_CASE("[Rendering][Environment] Color grading tonal ranges stay integrated") {
	const EnvironmentColorGrading::TonalRanges low_ranges = EnvironmentColorGrading::clamp_tonal_ranges(-1.0f, 1.0f, 0.2f, 0.4f, 0.1f, 2.0f);
	CHECK(low_ranges.shadows_start == doctest::Approx(0.0f));
	CHECK(low_ranges.shadows_end == doctest::Approx(0.2f));
	CHECK(low_ranges.midtones_start == doctest::Approx(0.2f));
	CHECK(low_ranges.midtones_end == doctest::Approx(0.4f));
	CHECK(low_ranges.highlights_start == doctest::Approx(0.4f));
	CHECK(low_ranges.highlights_end == doctest::Approx(1.0f));

	const EnvironmentColorGrading::TonalRanges high_ranges = EnvironmentColorGrading::clamp_tonal_ranges(0.8f, 0.9f, 0.6f, 0.7f, 0.8f, 0.6f);
	CHECK(high_ranges.shadows_start == doctest::Approx(0.6f));
	CHECK(high_ranges.shadows_end == doctest::Approx(0.6f));
	CHECK(high_ranges.midtones_start == doctest::Approx(0.6f));
	CHECK(high_ranges.midtones_end == doctest::Approx(0.7f));
	CHECK(high_ranges.highlights_start == doctest::Approx(0.8f));
	CHECK(high_ranges.highlights_end == doctest::Approx(0.8f));
}

TEST_CASE("[Rendering][Environment] Vignette range stays ordered") {
	const Vector2 low_range = EnvironmentColorGrading::clamp_vignette_range(-1.0f, -1.0f);
	CHECK(low_range.x == doctest::Approx(0.0f));
	CHECK(low_range.y == doctest::Approx(0.001f));

	const Vector2 high_range = EnvironmentColorGrading::clamp_vignette_range(5.0f, 5.0f);
	CHECK(high_range.x == doctest::Approx(0.999f));
	CHECK(high_range.y == doctest::Approx(1.0f));
}

} // namespace TestEnvironmentColorGrading

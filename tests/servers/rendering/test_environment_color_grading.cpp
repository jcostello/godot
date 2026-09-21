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

	const Vector2 default_shadows = EnvironmentColorGrading::clamp_tonal_range(0.0f, 0.3f);
	CHECK(default_shadows.is_equal_approx(Vector2(0.0f, 0.3f)));
	CHECK(EnvironmentColorGrading::prepare_shadows_midtones_highlights(Color(1, 1, 1), 0.0f).is_equal_approx(Color(1, 1, 1)));
	CHECK(EnvironmentColorGrading::prepare_lift(Color(1, 1, 1), 0.0f).is_equal_approx(Color(0, 0, 0)));
	CHECK(EnvironmentColorGrading::prepare_gamma(Color(1, 1, 1), 0.0f).is_equal_approx(Color(1, 1, 1)));
	CHECK(EnvironmentColorGrading::prepare_gain(Color(1, 1, 1), 0.0f).is_equal_approx(Color(1, 1, 1)));
}

TEST_CASE("[Rendering][Environment] Chromatic white balance is neutral and intensity is continuous") {
	const Basis identity;
	const Basis neutral = EnvironmentColorGrading::white_balance_matrix(6500.0f, 0.0f, 1.0f);
	CHECK(neutral.is_equal_approx(identity));
	CHECK(EnvironmentColorGrading::white_balance_matrix(3000.0f, 0.5f, 0.0f).is_equal_approx(identity));

	const Basis full = EnvironmentColorGrading::white_balance_matrix(3000.0f, 0.5f, 1.0f);
	const Basis half = EnvironmentColorGrading::white_balance_matrix(3000.0f, 0.5f, 0.5f);
	for (int row = 0; row < 3; row++) {
		for (int column = 0; column < 3; column++) {
			CHECK(Math::is_finite(full[row][column]));
			CHECK(half[row][column] == doctest::Approx(Math::lerp(row == column ? 1.0f : 0.0f, full[row][column], 0.5f)));
		}
	}
}

TEST_CASE("[Rendering][Environment] Chromatic white balance temperature and tint axes have expected directions") {
	const Vector3 neutral_white(1.0f, 1.0f, 1.0f);
	const Vector3 warm = EnvironmentColorGrading::white_balance_matrix(3000.0f, 0.0f, 1.0f).xform(neutral_white);
	const Vector3 cool = EnvironmentColorGrading::white_balance_matrix(10000.0f, 0.0f, 1.0f).xform(neutral_white);
	CHECK(warm.x > warm.z);
	CHECK(cool.z > cool.x);

	const Vector3 magenta = EnvironmentColorGrading::white_balance_matrix(6500.0f, 0.5f, 1.0f).xform(neutral_white);
	const Vector3 green = EnvironmentColorGrading::white_balance_matrix(6500.0f, -0.5f, 1.0f).xform(neutral_white);
	CHECK(magenta.x + magenta.z > 2.0f * magenta.y);
	CHECK(green.x + green.z < 2.0f * green.y);
}

TEST_CASE("[Rendering][Environment] Lift gamma gain preparation separates color from luminance") {
	const Color lift = EnvironmentColorGrading::prepare_lift(Color(1, 0, 0), 0.25f);
	CHECK(lift.r > lift.g);
	CHECK(lift.g == doctest::Approx(lift.b));
	const Color gamma = EnvironmentColorGrading::prepare_gamma(Color(1, 1, 1), -1.0f);
	CHECK(Math::is_finite(gamma.r));
	CHECK(gamma.r <= 1000.0f);
	const Color gain = EnvironmentColorGrading::prepare_gain(Color(1, 1, 1), -1.0f);
	CHECK(gain.is_equal_approx(Color(0, 0, 0)));
}

TEST_CASE("[Rendering][Environment] Unity shadows midtones highlights preparation") {
	const Color positive = EnvironmentColorGrading::prepare_shadows_midtones_highlights(Color(1, 1, 1), 0.25f);
	CHECK(positive.is_equal_approx(Color(2, 2, 2)));
	const Color negative = EnvironmentColorGrading::prepare_shadows_midtones_highlights(Color(0, 0, 0), -0.25f);
	CHECK(negative.is_equal_approx(Color(0, 0, 0)));
	const Color tint = EnvironmentColorGrading::prepare_shadows_midtones_highlights(Color(1, 0, 0), 0.0f);
	CHECK(tint.is_equal_approx(Color(1, 0, 0)));
}

TEST_CASE("[Rendering][Environment] Color grading midtones stay ordered") {
	const Vector2 low_range = EnvironmentColorGrading::clamp_tonal_range(-1.0f, -1.0f);
	CHECK(low_range.x == doctest::Approx(0.0f));
	CHECK(low_range.y == doctest::Approx(0.0f));

	const Vector2 high_range = EnvironmentColorGrading::clamp_tonal_range(2.0f, 2.0f);
	CHECK(high_range.x == doctest::Approx(1.0f));
	CHECK(high_range.y == doctest::Approx(1.0f));
}

TEST_CASE("[Rendering][Environment] Hard tonal cuts allow empty ranges") {
	CHECK(EnvironmentColorGrading::clamp_tonal_range(0.0f, 0.0f).is_equal_approx(Vector2(0.0f, 0.0f)));
	CHECK(EnvironmentColorGrading::clamp_tonal_range(1.0f, 1.0f).is_equal_approx(Vector2(1.0f, 1.0f)));
	CHECK(EnvironmentColorGrading::clamp_tonal_range(0.5f, 0.5f).is_equal_approx(Vector2(0.5f, 0.5f)));
	CHECK(EnvironmentColorGrading::clamp_tonal_range(0.8f, 0.2f).is_equal_approx(Vector2(0.8f, 0.8f)));
}

TEST_CASE("[Rendering][Environment] Unity tonal weights use four limits") {
	const Vector2 shadows(0.0f, 0.3f);
	const Vector2 highlights(0.55f, 1.0f);
	CHECK(EnvironmentColorGrading::tonal_weights(0.0f, shadows, highlights).is_equal_approx(Vector3(1, 0, 0)));
	CHECK(EnvironmentColorGrading::tonal_weights(0.3f, shadows, highlights).is_equal_approx(Vector3(0, 1, 0)));
	CHECK(EnvironmentColorGrading::tonal_weights(0.55f, shadows, highlights).is_equal_approx(Vector3(0, 1, 0)));
	CHECK(EnvironmentColorGrading::tonal_weights(1.0f, shadows, highlights).is_equal_approx(Vector3(0, 0, 1)));
	const Vector3 overlapping = EnvironmentColorGrading::tonal_weights(0.5f, Vector2(0.8f, 1.0f), Vector2(0.0f, 0.2f));
	CHECK(overlapping.x + overlapping.y + overlapping.z == doctest::Approx(1.0f));
	CHECK(overlapping.y < 0.0f);
}

TEST_CASE("[Rendering][Environment] Color grading tonal ranges stay integrated") {
	const EnvironmentColorGrading::TonalRanges low_ranges = EnvironmentColorGrading::clamp_tonal_ranges(-1.0f, 1.0f, 0.2f, 0.4f, 0.1f, 2.0f);
	CHECK(low_ranges.shadows_start == doctest::Approx(0.0f));
	CHECK(low_ranges.shadows_end == doctest::Approx(1.0f));
	CHECK(low_ranges.midtones_start == doctest::Approx(0.2f));
	CHECK(low_ranges.midtones_end == doctest::Approx(0.4f));
	CHECK(low_ranges.highlights_start == doctest::Approx(0.1f));
	CHECK(low_ranges.highlights_end == doctest::Approx(1.0f));

	const EnvironmentColorGrading::TonalRanges high_ranges = EnvironmentColorGrading::clamp_tonal_ranges(0.8f, 0.9f, 0.6f, 0.7f, 0.8f, 0.6f);
	CHECK(high_ranges.shadows_start == doctest::Approx(0.8f));
	CHECK(high_ranges.shadows_end == doctest::Approx(0.9f));
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

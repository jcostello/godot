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
	CHECK(low_range.y == doctest::Approx(0.01f));

	const Vector2 high_range = EnvironmentColorGrading::clamp_midtones_range(2.0f, 2.0f);
	CHECK(high_range.x == doctest::Approx(0.99f));
	CHECK(high_range.y == doctest::Approx(1.0f));
}

} // namespace TestEnvironmentColorGrading

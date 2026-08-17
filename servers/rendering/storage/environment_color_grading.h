/**************************************************************************/
/*  environment_color_grading.h                                           */
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

#include "core/math/color.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"

namespace EnvironmentColorGrading {

struct TonalRanges {
	float shadows_start = 0.0f;
	float shadows_end = 0.45f;
	float midtones_start = 0.45f;
	float midtones_end = 0.55f;
	float highlights_start = 0.55f;
	float highlights_end = 1.0f;
};

inline Vector2 clamp_midtones_range(float p_start, float p_end) {
	const float start = CLAMP(p_start, 0.0f, 0.99f);
	return Vector2(start, CLAMP(p_end, start + 0.01f, 1.0f));
}

inline TonalRanges clamp_tonal_ranges(float p_shadows_start, float p_shadows_end, float p_midtones_start, float p_midtones_end, float p_highlights_start, float p_highlights_end) {
	TonalRanges ranges;
	const Vector2 midtones_range = clamp_midtones_range(p_midtones_start, p_midtones_end);
	ranges.midtones_start = midtones_range.x;
	ranges.midtones_end = midtones_range.y;

	ranges.shadows_start = CLAMP(p_shadows_start, 0.0f, ranges.midtones_start);
	ranges.shadows_end = CLAMP(p_shadows_end, ranges.shadows_start, ranges.midtones_start);

	ranges.highlights_start = CLAMP(p_highlights_start, ranges.midtones_end, 1.0f);
	ranges.highlights_end = CLAMP(p_highlights_end, ranges.highlights_start, 1.0f);

	return ranges;
}

inline Vector2 clamp_vignette_range(float p_start, float p_end) {
	const float start = CLAMP(p_start, 0.0f, 0.999f);
	return Vector2(start, CLAMP(p_end, start + 0.001f, 1.0f));
}

inline Color color_from_temperature(float p_temperature) {
	const float temperature = CLAMP(p_temperature, 1000.0f, 15000.0f);
	const float temperature_squared = temperature * temperature;
	const float u = (0.860117757f + 1.54118254e-4f * temperature + 1.28641212e-7f * temperature_squared) /
			(1.0f + 8.42420235e-4f * temperature + 7.08145163e-7f * temperature_squared);
	const float v = (0.317398726f + 4.22806245e-5f * temperature + 4.20481691e-8f * temperature_squared) /
			(1.0f - 2.89741816e-5f * temperature + 1.61456053e-7f * temperature_squared);

	const float d = 1.0f / (2.0f * u - 8.0f * v + 4.0f);
	const float x = 3.0f * u * d;
	const float y = 2.0f * v * d;
	const float inverse_y = 1.0f / MAX(y, 1e-5f);
	const Vector3 xyz(x * inverse_y, 1.0f, (1.0f - x - y) * inverse_y);
	Vector3 linear(
			3.2404542f * xyz.x - 1.5371385f * xyz.y - 0.4985314f * xyz.z,
			-0.9692660f * xyz.x + 1.8760108f * xyz.y + 0.0415560f * xyz.z,
			0.0556434f * xyz.x - 0.2040259f * xyz.y + 1.0572252f * xyz.z);
	linear /= MAX(1e-5f, linear[linear.max_axis_index()]);
	return Color(linear.x, linear.y, linear.z).clamp();
}

inline Vector3 temperature_balance(float p_temperature) {
	const Color neutral = color_from_temperature(6500.0f);
	const Color current = color_from_temperature(p_temperature);
	return Vector3(
			current.r / MAX(neutral.r, 1e-5f),
			current.g / MAX(neutral.g, 1e-5f),
			current.b / MAX(neutral.b, 1e-5f));
}

} // namespace EnvironmentColorGrading

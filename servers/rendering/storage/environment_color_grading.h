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

#include "core/math/basis.h"
#include "core/math/color.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"

namespace EnvironmentColorGrading {

struct TonalRanges {
	float shadows_start = 0.0f;
	float shadows_end = 0.3f;
	float midtones_start = 0.3f;
	float midtones_end = 0.55f;
	float highlights_start = 0.55f;
	float highlights_end = 1.0f;
};

inline Vector2 clamp_tonal_range(float p_start, float p_end) {
	const float start = CLAMP(p_start, 0.0f, 1.0f);
	return Vector2(start, CLAMP(p_end, start, 1.0f));
}

inline float tonal_smoothstep(float p_start, float p_end, float p_luminance) {
	if (p_end <= p_start) {
		return p_luminance >= p_start ? 1.0f : 0.0f;
	}
	const float t = CLAMP((p_luminance - p_start) / (p_end - p_start), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// Unity Graphics 6000.0/staging snapshot 48a8a5b, ColorGrading.hlsl and LutBuilderHdr.shader.
// Keep this in sync with grading_tonal_weights() in color_grading_inc.glsl.
inline Vector3 tonal_weights(float p_luminance, const Vector2 &p_shadows_limits, const Vector2 &p_highlights_limits) {
	const Vector2 shadows = clamp_tonal_range(p_shadows_limits.x, p_shadows_limits.y);
	const Vector2 highlights = clamp_tonal_range(p_highlights_limits.x, p_highlights_limits.y);
	const float shadows_weight = 1.0f - tonal_smoothstep(shadows.x, shadows.y, p_luminance);
	const float highlights_weight = tonal_smoothstep(highlights.x, highlights.y, p_luminance);
	return Vector3(shadows_weight, 1.0f - shadows_weight - highlights_weight, highlights_weight);
}

inline float unity_luminance(const Color &p_color) {
	return p_color.r * 0.2126729f + p_color.g * 0.7151522f + p_color.b * 0.0721750f;
}

inline Color prepare_shadows_midtones_highlights(const Color &p_color, float p_intensity) {
	const Color linear = p_color.srgb_to_linear();
	const float intensity = CLAMP(p_intensity, -1.0f, 1.0f);
	const float weight = intensity * (intensity < 0.0f ? 1.0f : 4.0f);
	return Color(MAX(linear.r + weight, 0.0f), MAX(linear.g + weight, 0.0f), MAX(linear.b + weight, 0.0f));
}

// Unity Graphics 6000.0/staging snapshot 48a8a5b, ColorUtils.PrepareLiftGammaGain().
inline Color prepare_lift(const Color &p_color, float p_intensity) {
	const Color linear = p_color.srgb_to_linear() * 0.15f;
	const float luminance = unity_luminance(linear);
	return Color(linear.r - luminance + p_intensity, linear.g - luminance + p_intensity, linear.b - luminance + p_intensity);
}

inline Color prepare_gamma(const Color &p_color, float p_intensity) {
	const Color linear = p_color.srgb_to_linear() * 0.8f;
	const float luminance = unity_luminance(linear);
	const float base = 1.0f + p_intensity - luminance;
	return Color(1.0f / MAX(linear.r + base, 0.001f), 1.0f / MAX(linear.g + base, 0.001f), 1.0f / MAX(linear.b + base, 0.001f));
}

inline Color prepare_gain(const Color &p_color, float p_intensity) {
	const Color linear = p_color.srgb_to_linear() * 0.8f;
	const float luminance = unity_luminance(linear);
	return Color(linear.r - luminance + 1.0f + p_intensity, linear.g - luminance + 1.0f + p_intensity, linear.b - luminance + 1.0f + p_intensity);
}

inline TonalRanges clamp_tonal_ranges(float p_shadows_start, float p_shadows_end, float p_midtones_start, float p_midtones_end, float p_highlights_start, float p_highlights_end) {
	TonalRanges ranges;
	const Vector2 midtones_range = clamp_tonal_range(p_midtones_start, p_midtones_end);
	ranges.midtones_start = midtones_range.x;
	ranges.midtones_end = midtones_range.y;

	const Vector2 shadows_range = clamp_tonal_range(p_shadows_start, p_shadows_end);
	ranges.shadows_start = shadows_range.x;
	ranges.shadows_end = shadows_range.y;

	const Vector2 highlights_range = clamp_tonal_range(p_highlights_start, p_highlights_end);
	ranges.highlights_start = highlights_range.x;
	ranges.highlights_end = highlights_range.y;

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

inline Basis white_balance_matrix(float p_temperature, float p_tint, float p_intensity) {
	const auto white_xyz = [](float p_temperature_value, float p_tint_value) {
		const float temperature = CLAMP(p_temperature_value, 1000.0f, 15000.0f);
		const float temperature_squared = temperature * temperature;
		const float u = (0.860117757f + 1.54118254e-4f * temperature + 1.28641212e-7f * temperature_squared) /
				(1.0f + 8.42420235e-4f * temperature + 7.08145163e-7f * temperature_squared);
		const float v = (0.317398726f + 4.22806245e-5f * temperature + 4.20481691e-8f * temperature_squared) /
						(1.0f - 2.89741816e-5f * temperature + 1.61456053e-7f * temperature_squared) -
				CLAMP(p_tint_value, -1.0f, 1.0f) * 0.05f;
		const float d = 1.0f / MAX(2.0f * u - 8.0f * v + 4.0f, 1e-5f);
		const float x = 3.0f * u * d;
		const float y = MAX(2.0f * v * d, 1e-5f);
		return Vector3(x / y, 1.0f, (1.0f - x - y) / y);
	};

	const Basis rgb_to_xyz(
			0.4124564f, 0.3575761f, 0.1804375f,
			0.2126729f, 0.7151522f, 0.0721750f,
			0.0193339f, 0.1191920f, 0.9503041f);
	const Basis xyz_to_rgb = rgb_to_xyz.inverse();
	const Basis bradford(
			0.8951f, 0.2664f, -0.1614f,
			-0.7502f, 1.7135f, 0.0367f,
			0.0389f, -0.0685f, 1.0296f);
	const Basis inverse_bradford = bradford.inverse();
	const Vector3 source_lms = bradford.xform(white_xyz(6500.0f, 0.0f));
	const Vector3 target_lms = bradford.xform(white_xyz(p_temperature, p_tint));
	const Basis scale(
			target_lms.x / MAX(source_lms.x, 1e-5f), 0.0f, 0.0f,
			0.0f, target_lms.y / MAX(source_lms.y, 1e-5f), 0.0f,
			0.0f, 0.0f, target_lms.z / MAX(source_lms.z, 1e-5f));
	const Basis adapted = xyz_to_rgb * inverse_bradford * scale * bradford * rgb_to_xyz;
	const float intensity = CLAMP(p_intensity, 0.0f, 1.0f);
	Basis result;
	for (int row = 0; row < 3; row++) {
		for (int column = 0; column < 3; column++) {
			result[row][column] = Math::lerp(row == column ? 1.0f : 0.0f, adapted[row][column], intensity);
		}
	}
	return result;
}

} // namespace EnvironmentColorGrading

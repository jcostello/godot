/**************************************************************************/
/*  lightmap_adjustments.h                                                */
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

// LightmapGI color controls. Directionality is retained for serialization only.
struct LightmapAdjustments {
	bool enabled = true;
	float energy = 1.0f;
	Color tint = Color(1, 1, 1);
	float saturation = 1.0f;
	float contrast = 1.0f;
	float shadows = 0.0f;
	float midtones = 0.0f;
	float highlights = 0.0f;
	Color tonal_ranges = Color(0.1f, 0.4f, 0.4f, 0.8f);
	Color shadows_tint = Color(1, 1, 1);
	Color midtones_tint = Color(1, 1, 1);
	Color highlights_tint = Color(1, 1, 1);
	Vector3 tonal_saturation = Vector3(1, 1, 1);
	Vector3 tonal_contrast = Vector3(1, 1, 1);
	Vector3 tonal_black_point;
	Vector3 tonal_white_point = Vector3(1, 1, 1);
	Vector3 tonal_gamma = Vector3(1, 1, 1);
	int debug_mode = 0;
};

// Robust L0 luminance range: 1st and 99th percentiles of a log2 histogram, so a few hot
// texels near lights do not push every other texel into the shadows range.
struct LightmapLuminanceHistogram {
	static constexpr int BIN_COUNT = 1024;
	static constexpr float LOG_MIN = -24.0f;
	static constexpr float LOG_MAX = 24.0f;
	uint32_t bins[BIN_COUNT] = {};
	uint64_t count = 0;

	void add(const Color &p_color) {
		const float luminance = p_color.r * 0.2126f + p_color.g * 0.7152f + p_color.b * 0.0722f;
		// Unused texels are black and carry no lighting.
		if (!(luminance > 0.0f) || !Math::is_finite(luminance)) {
			return;
		}
		const int bin = CLAMP(int((Math::log2(luminance) - LOG_MIN) * (BIN_COUNT / (LOG_MAX - LOG_MIN))), 0, BIN_COUNT - 1);
		bins[bin]++;
		count++;
	}

	float percentile(float p_fraction) const {
		const uint64_t target = uint64_t(p_fraction * double(count - 1));
		uint64_t accumulated = 0;
		for (int i = 0; i < BIN_COUNT; i++) {
			accumulated += bins[i];
			if (accumulated > target) {
				return Math::pow(2.0f, LOG_MIN + (i + 0.5f) * ((LOG_MAX - LOG_MIN) / BIN_COUNT));
			}
		}
		return Math::pow(2.0f, LOG_MAX);
	}

	// (min, max) in linear luminance; max < min means there is no lit texel.
	Vector2 get_range() const {
		return count == 0 ? Vector2(0, -1) : Vector2(percentile(0.01f), percentile(0.99f));
	}
};

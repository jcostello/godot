/**************************************************************************/
/*  test_lightmap_seam.h                                                  */
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

#include "../lightmap_seam.h"

#include "tests/test_macros.h"

namespace TestLightmapSeam {

TEST_CASE("[LightmapSeamStitcher] Matches filtered samples on both sides of a seam") {
	for (int bicubic = 0; bicubic < 2; bicubic++) {
		// Two vertical edges in an 8x8 atlas: x = 2 (left chart) and x = 6 (right chart), at texel boundaries.
		LightmapSeamStitcher stitcher;
		stitcher.atlas_size = Size2i(8, 8);
		stitcher.bicubic = bicubic;
		const Vector2 dst[2] = { Vector2(2.0, 1.0) / 8.0, Vector2(2.0, 7.0) / 8.0 };
		const Vector2 src[2] = { Vector2(6.0, 1.0) / 8.0, Vector2(6.0, 7.0) / 8.0 };
		stitcher.add_seam(dst, 0, src, 0);

		// Left side is dark, right side is bright: the seam is visible before stitching.
		LocalVector<Vector3> values;
		values.resize(stitcher.texels.size());
		for (uint32_t i = 0; i < stitcher.texels.size(); i++) {
			const int x = int(stitcher.texels[i] % 8);
			values[i] = Vector3(1, 1, 1) * (x < 4 ? 0.0f : 1.0f);
		}
		stitcher.solve(values, 0.01f, 200);

		const uint32_t constraint_taps = stitcher.get_constraint_taps();
		for (uint32_t c = 0; c < stitcher.taps.size(); c += constraint_taps) {
			Vector3 difference;
			for (uint32_t k = c; k < c + constraint_taps; k++) {
				difference += values[stitcher.taps[k].index] * stitcher.taps[k].weight;
			}
			CHECK(Math::abs(difference.x) < 0.05);
		}
		if (!bicubic) {
			// Both sides move toward each other instead of one copying the other.
			// The outer texels of the bicubic footprint carry almost no weight, so they barely move.
			for (uint32_t i = 0; i < stitcher.texels.size(); i++) {
				CHECK(values[i].x > 0.2);
				CHECK(values[i].x < 0.8);
			}
		}
	}
}

} // namespace TestLightmapSeam

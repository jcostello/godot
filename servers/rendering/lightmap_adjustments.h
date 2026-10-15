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
	float directionality_strength = 1.0f;
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

/**************************************************************************/
/*  lightmap_probe_capture.h                                              */
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

#include "core/math/dynamic_bvh.h"
#include "core/math/transform_3d.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

// Shared CPU capture lookup for RD and Compatibility. Serialized captures keep
// points/SH/tetrahedra, and this runtime BVH indexes the tetrahedra.
class LightmapProbeCapture {
	struct Cell {
		Vector3 points[4];
		int indices[4];
		Basis inverse;
	};
	// Cells keep the tetrahedra order. On overlap, the lowest index wins.
	LocalVector<Cell> cells;
	mutable DynamicBVH index;
	AABB bounds;
	PackedVector3Array probe_points;
	mutable DynamicBVH point_index;
	AABB point_bounds;
	bool _sample_nearest_probes(const Vector3 &p_point, const PackedColorArray &p_sh, Color *r_sh) const;
	static Color _weights(const Cell &p_cell, const Vector3 &p_point);

public:
	LightmapProbeCapture() = default;
	LightmapProbeCapture(const LightmapProbeCapture &) = delete;
	LightmapProbeCapture &operator=(const LightmapProbeCapture &) = delete;
	void clear();
	void update(const PackedVector3Array &p_points, const PackedInt32Array &p_tetrahedra);
	bool sample(const Vector3 &p_point, const PackedColorArray &p_sh, Color *r_sh) const;
};

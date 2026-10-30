/**************************************************************************/
/*  light_probe_geometry.h                                                */
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
#include "core/math/triangle_mesh.h"
#include "scene/3d/light_probe_volume.h"

class Material;
class Mesh;

// CPU geometry snapshot shared by explicit grid generation and bake validation.
// Interactive probe dragging never queries geometry. Coordinates are world-space.
class LightProbeGeometry {
	struct Geometry {
		Ref<TriangleMesh> mesh;
		Vector<uint8_t> closed_faces;
		bool has_closed_faces = false;
		AABB bounds;
	};
	Vector<Geometry> geometries;
	DynamicBVH broadphase;

	Error _append_mesh(const Ref<Mesh> &p_mesh, const Transform3D &p_transform, const Vector<Ref<Material>> &p_materials);
	Error _collect(Node *p_node, Lightmapper::BakeStepFunc p_step, void *p_userdata);
	static bool _near_surface(const Geometry &p_geometry, const Vector3 &p_position, real_t p_clearance);
	static LightProbeVolume::ValidationStatus _inside(const Geometry &p_geometry, const Vector3 &p_position);

public:
	Error build(Node *p_root, Lightmapper::BakeStepFunc p_step = nullptr, void *p_userdata = nullptr);
	LightProbeVolume::ValidationStatus validate(const Vector3 &p_position, real_t p_clearance);
};

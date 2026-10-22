/**************************************************************************/
/*  light_probe_data.h                                                    */
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

#include "core/io/resource.h"

// Lighting/capture data only. Editable placement belongs to LightProbeVolumeData.
class LightProbeData : public Resource {
	GDCLASS(LightProbeData, Resource);
	// Keep recognizing legacy bakes, but use the standard resource extension when saving.
public:
	static void register_custom_data_to_otdb() {
		Resource::_add_resource_base_extension_to_classdb("probebake", get_class_static());
	}
	String get_base_extension() const override { return "tres"; }

private:
	AABB bounds;
	PackedVector3Array points;
	PackedColorArray coefficients;
	PackedInt32Array tetrahedra;
	float baked_exposure = 1.0f;
	bool interior = false;
	bool stale = false;
	uint32_t position_hash = 0;
	bool cache_invalidated = false;
	mutable bool upload_pending = true;
	mutable bool rebuild_on_upload = false;
	mutable bool bounds_uploaded = false;
	mutable AABB uploaded_bounds;
	RID capture;

	void _upload() const;

protected:
	static void _bind_methods();

public:
	Dictionary get_capture_data() const;
	Error set_capture_data(const Dictionary &p_data);
	PackedVector3Array get_points() const { return points; }
	PackedColorArray get_coefficients() const { return coefficients; }
	PackedInt32Array get_tetrahedra() const { return tetrahedra; }
	AABB get_bounds() const { return bounds; }
	float get_baked_exposure() const { return baked_exposure; }
	uint32_t get_position_hash() const { return position_hash; }
	int get_probe_count() const { return points.size(); }
	bool is_stale() const { return stale; }
	void set_stale(bool p_stale);
	void clear_cache();
	bool is_cache_invalidated() const { return cache_invalidated; }
	void consume_cache_invalidation();
	RID get_rid() const override;
	LightProbeData();
	~LightProbeData();
};

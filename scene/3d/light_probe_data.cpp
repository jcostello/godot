/**************************************************************************/
/*  light_probe_data.cpp                                                  */
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

#include "light_probe_data.h"

#include "core/object/class_db.h"
#include "core/string/print_string.h"
#include "servers/rendering/rendering_server.h"

Dictionary LightProbeData::get_capture_data() const {
	Dictionary result;
	result["bounds"] = bounds;
	result["points"] = points;
	result["sh"] = coefficients;
	result["tetrahedra"] = tetrahedra;
	result["baked_exposure"] = baked_exposure;
	result["interior"] = interior;
	result["stale"] = stale;
	return result;
}

Error LightProbeData::set_capture_data(const Dictionary &p_data) {
	ERR_FAIL_COND_V(!p_data.has("bounds") || !p_data.has("points") || !p_data.has("sh") || !p_data.has("tetrahedra") || !p_data.has("baked_exposure") || !p_data.has("interior"), ERR_INVALID_DATA);
	ERR_FAIL_COND_V(p_data["bounds"].get_type() != Variant::AABB || p_data["points"].get_type() != Variant::PACKED_VECTOR3_ARRAY || p_data["sh"].get_type() != Variant::PACKED_COLOR_ARRAY || p_data["tetrahedra"].get_type() != Variant::PACKED_INT32_ARRAY, ERR_INVALID_DATA);
	const AABB new_bounds = p_data["bounds"];
	const PackedVector3Array new_points = p_data["points"];
	const PackedColorArray new_coefficients = p_data["sh"];
	const PackedInt32Array new_tetrahedra = p_data["tetrahedra"];
	ERR_FAIL_COND_V((p_data["baked_exposure"].get_type() != Variant::FLOAT && p_data["baked_exposure"].get_type() != Variant::INT) || p_data["interior"].get_type() != Variant::BOOL, ERR_INVALID_DATA);
	ERR_FAIL_COND_V(p_data.has("stale") && p_data["stale"].get_type() != Variant::BOOL, ERR_INVALID_DATA);
	const float new_exposure = p_data["baked_exposure"];
	const bool new_interior = p_data["interior"];
	ERR_FAIL_COND_V(!new_bounds.position.is_finite() || !new_bounds.size.is_finite() || !new_bounds.get_end().is_finite() || new_bounds.size.x < 0 || new_bounds.size.y < 0 || new_bounds.size.z < 0, ERR_INVALID_DATA);
	ERR_FAIL_COND_V(!Math::is_finite(new_exposure) || new_exposure < 0 || int64_t(new_points.size()) * 9 != new_coefficients.size() || new_tetrahedra.size() % 4 != 0, ERR_INVALID_DATA);
	for (const Vector3 &point : new_points) {
		ERR_FAIL_COND_V(!point.is_finite(), ERR_INVALID_DATA);
	}
	for (const Color &coefficient : new_coefficients) {
		ERR_FAIL_COND_V(!Math::is_finite(coefficient.r) || !Math::is_finite(coefficient.g) || !Math::is_finite(coefficient.b) || !Math::is_finite(coefficient.a), ERR_INVALID_DATA);
	}
	for (int vertex : new_tetrahedra) {
		ERR_FAIL_COND_V(vertex < 0 || vertex >= new_points.size(), ERR_INVALID_DATA);
	}
	upload_pending = upload_pending || bounds != new_bounds || points != new_points || coefficients != new_coefficients || tetrahedra != new_tetrahedra || baked_exposure != new_exposure || interior != new_interior;
	bounds = new_bounds;
	points = new_points;
	coefficients = new_coefficients;
	tetrahedra = new_tetrahedra;
	baked_exposure = new_exposure;
	interior = new_interior;
	stale = p_data.get("stale", false);
	emit_changed();
	return OK;
}

void LightProbeData::_upload() const {
	if (!upload_pending) {
		return;
	}
	upload_pending = false;
	RenderingServer *server = RenderingServer::get_singleton();
	if (rebuild_on_upload || points.is_empty()) {
		server->lightmap_set_probe_capture_data(capture, PackedVector3Array(), PackedColorArray(), PackedInt32Array(), PackedInt32Array());
		rebuild_on_upload = false;
	}
	const AABB new_bounds = points.is_empty() ? AABB() : bounds;
	if (!bounds_uploaded || uploaded_bounds != new_bounds) {
		server->lightmap_set_probe_bounds(capture, new_bounds);
		uploaded_bounds = new_bounds;
		bounds_uploaded = true;
	}
	server->lightmap_set_probe_interior(capture, interior);
	server->lightmap_set_baked_exposure_normalization(capture, baked_exposure);
	if (!points.is_empty()) {
		server->lightmap_set_probe_capture_data(capture, points, coefficients, tetrahedra, PackedInt32Array());
	}
}

RID LightProbeData::get_rid() const {
	_upload();
	return capture;
}

void LightProbeData::set_stale(bool p_stale) {
	if (stale == p_stale) {
		return;
	}
	stale = p_stale;
	emit_changed();
}

void LightProbeData::clear_cache() {
	cache_invalidated = true;
}

void LightProbeData::consume_cache_invalidation() {
	if (cache_invalidated) {
		rebuild_on_upload = true;
		upload_pending = true;
		cache_invalidated = false;
	}
}

void LightProbeData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_capture_data", "data"), &LightProbeData::set_capture_data);
	ClassDB::bind_method(D_METHOD("get_capture_data"), &LightProbeData::get_capture_data);
	ClassDB::bind_method(D_METHOD("get_points"), &LightProbeData::get_points);
	ClassDB::bind_method(D_METHOD("get_coefficients"), &LightProbeData::get_coefficients);
	ClassDB::bind_method(D_METHOD("get_probe_count"), &LightProbeData::get_probe_count);
	ClassDB::bind_method(D_METHOD("get_baked_exposure"), &LightProbeData::get_baked_exposure);
	ClassDB::bind_method(D_METHOD("is_stale"), &LightProbeData::is_stale);
	ClassDB::bind_method(D_METHOD("clear_cache"), &LightProbeData::clear_cache);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "_capture_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_INTERNAL), "set_capture_data", "get_capture_data");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "probe_count", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_probe_count");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "baked_exposure", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_baked_exposure");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "stale", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "is_stale");
}

LightProbeData::LightProbeData() {
	capture = RenderingServer::get_singleton()->lightmap_create();
}

LightProbeData::~LightProbeData() {
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	RenderingServer::get_singleton()->free_rid(capture);
}

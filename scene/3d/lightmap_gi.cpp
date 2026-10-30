/**************************************************************************/
/*  lightmap_gi.cpp                                                       */
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

#include "lightmap_gi.h"

#include "light_probe_geometry.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/math/delaunay_3d.h"
#include "core/math/delaunay_3d_incremental.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/camera_attributes.h"
#include "scene/resources/environment.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/sky.h"
#include "servers/rendering/light_energy.h"
#include "servers/rendering/rendering_server.h"

#include "modules/modules_enabled.gen.h" // IWYU pragma: keep. For lightmapper_rd.

#ifdef MODULE_LIGHTMAPPER_RD_ENABLED
#include "servers/display/display_server.h"
#endif

namespace {
void collect_probe_systems(Node *p_root, LightmapGI *p_source, Vector<LightProbeSystem *> &r_systems) {
	if (!p_root) {
		return;
	}
	if (Object::cast_to<LightmapGI>(p_root) && p_root != p_source) {
		return;
	}
	if (LightProbeSystem *system = Object::cast_to<LightProbeSystem>(p_root)) {
		r_systems.push_back(system);
	}
	for (int i = 0; i < p_root->get_child_count(); i++) {
		if (p_root->get_child(i)->get_owner()) {
			collect_probe_systems(p_root->get_child(i), p_source, r_systems);
		}
	}
}

LightmapGI::BakeError prepare_probe_capture(Lightmapper *p_lightmapper, LightProbeSystem *p_system, int p_probe_offset, int p_probe_count, const Transform3D &p_capture_transform, float p_exposure, bool p_interior, Lightmapper::BakeStepFunc p_step, void *p_userdata, Ref<LightProbeData> &r_capture) {
	const Ref<LightProbeData> previous_capture = p_system->get_light_data();
	const bool use_probe_cache = previous_capture.is_valid() && !previous_capture->is_cache_invalidated() && p_system->get_capture_global_transform().is_equal_approx(p_capture_transform);
	const PackedVector3Array previous_probe_points = use_probe_cache ? previous_capture->get_points() : PackedVector3Array();
	const PackedInt32Array previous_probe_tetrahedra = use_probe_cache ? previous_capture->get_tetrahedra() : PackedInt32Array();
	Dictionary probe_capture_data;
	const int probe_count = p_probe_count;
	ERR_FAIL_COND_V(p_probe_offset + probe_count > p_lightmapper->get_bake_probe_count(), LightmapGI::BAKE_ERROR_MESHES_INVALID);

	// Probe SH may change between bakes.
	PackedColorArray probe_sh;
	PackedVector3Array probe_points;
	probe_sh.resize(probe_count * 9);
	probe_points.resize(probe_count);

	AABB system_bounds;
	for (int i = 0; i < probe_count; i++) {
		Vector3 point = p_lightmapper->get_bake_probe_point(p_probe_offset + i);
		if (i == 0) {
			system_bounds.position = point;
		} else {
			system_bounds.expand_to(point);
		}
		probe_points.set(i, point);
		Vector<Color> colors = p_lightmapper->get_bake_probe_sh(p_probe_offset + i);
		ERR_FAIL_COND_V(colors.size() != 9, LightmapGI::BAKE_ERROR_MESHES_INVALID);
		for (int j = 0; j < 9; j++) {
			probe_sh.set(i * 9 + j, colors[j]);
		}
	}

	// Measure structure generation separately from probe lighting.
	const uint64_t probe_structure_start = OS::get_singleton()->get_ticks_usec();
	PackedInt32Array tetrahedra = previous_probe_tetrahedra;
	const char *action = "reuse";
	if (probe_points != previous_probe_points || !use_probe_cache) {
		if (p_step && p_step(0.8, RTR("Updating Probe Volumes"), p_userdata, true)) {
			return LightmapGI::BAKE_ERROR_USER_ABORTED;
		}
		Vector<Delaunay3D::OutputSimplex> solved;
		Delaunay3DIncremental::Statistics incremental_statistics;
		const uint64_t local_start = OS::get_singleton()->get_ticks_usec();
		const bool incremental = Delaunay3DIncremental::update(previous_probe_points, previous_probe_tetrahedra, probe_points, solved, incremental_statistics);
		print_verbose(vformat("Lightmap probe incremental: result=%s, reason=%s, time=%.3fs, removed_points=%d, inserted_points=%d.",
				incremental ? "accepted" : "full_rebuild_required", incremental_statistics.fallback_reason, double(OS::get_singleton()->get_ticks_usec() - local_start) / 1e6,
				incremental_statistics.removed_points, incremental_statistics.inserted_points));
		if (incremental) {
			action = "incremental";
		} else {
			action = "rebuild";
			solved = Delaunay3D::tetrahedralize(probe_points);
		}
		tetrahedra.resize(solved.size() * 4);
		for (int i = 0; i < solved.size(); i++) {
			for (int j = 0; j < 4; j++) {
				tetrahedra.set(i * 4 + j, solved[i].points[j]);
			}
		}
	}
	// New captures use the runtime tetrahedron BVH, with no serialized BSP.
	probe_capture_data["tetrahedra"] = tetrahedra;
	print_verbose(vformat("Lightmap probe topology: action=%s, probes=%d, tetrahedra=%d, time=%.3fs.", action, probe_count, tetrahedra.size() / 4, double(OS::get_singleton()->get_ticks_usec() - probe_structure_start) / 1e6));
	if (probe_count > 0) {
		system_bounds.grow_by(MAX(real_t(0.05), system_bounds.size.length() * real_t(0.001)));
	}
	probe_capture_data["bounds"] = system_bounds;
	probe_capture_data["interior"] = p_interior;
	probe_capture_data["points"] = probe_points;
	probe_capture_data["sh"] = probe_sh;
	probe_capture_data["baked_exposure"] = p_exposure;
	probe_capture_data["stale"] = false;
	r_capture.instantiate();
	ERR_FAIL_COND_V(r_capture->set_capture_data(probe_capture_data) != OK, LightmapGI::BAKE_ERROR_MESHES_INVALID);
	return LightmapGI::BAKE_ERROR_OK;
}

Error save_bake_resources(const Vector<Ref<Resource>> &p_resources, const Vector<String> &p_paths) {
	ERR_FAIL_COND_V(p_resources.size() != p_paths.size(), ERR_INVALID_PARAMETER);
	Vector<String> backups;
	backups.resize(p_paths.size());
	const String suffix = vformat(".bake_backup_%d_%d", OS::get_singleton()->get_process_id(), OS::get_singleton()->get_ticks_usec());
	for (int i = 0; i < p_paths.size(); i++) {
		for (int j = 0; j < i; j++) {
			ERR_FAIL_COND_V(p_paths[i] == p_paths[j], ERR_INVALID_PARAMETER);
		}
	}
	for (int i = 0; i < p_paths.size(); i++) {
		if (FileAccess::exists(p_paths[i])) {
			const String backup = p_paths[i] + suffix;
			const Error error = DirAccess::copy_absolute(p_paths[i], backup);
			if (error != OK) {
				if (FileAccess::exists(backup)) {
					DirAccess::remove_absolute(backup);
				}
				for (const String &created : backups) {
					if (!created.is_empty()) {
						DirAccess::remove_absolute(created);
					}
				}
				return error;
			}
			backups.write[i] = backup;
		}
	}
	for (int i = 0; i < p_paths.size(); i++) {
		const Error error = ResourceSaver::save(p_resources[i], p_paths[i]);
		if (error != OK) {
			for (int j = 0; j <= i; j++) {
				if (backups[j].is_empty()) {
					if (FileAccess::exists(p_paths[j])) {
						DirAccess::remove_absolute(p_paths[j]);
					}
				} else if (DirAccess::copy_absolute(backups[j], p_paths[j]) != OK) {
					ERR_PRINT(vformat("Could not restore bake resource '%s'. Recovery copy retained at '%s'.", p_paths[j], backups[j]));
					backups.write[j] = String(); // Keep the recovery file.
				}
			}
			for (const String &backup : backups) {
				if (!backup.is_empty()) {
					DirAccess::remove_absolute(backup);
				}
			}
			return error;
		}
	}
	for (const String &backup : backups) {
		if (!backup.is_empty()) {
			DirAccess::remove_absolute(backup);
		}
	}
	return OK;
}
} // namespace

void LightmapGIData::add_user(const NodePath &p_path, const Rect2 &p_uv_scale, int p_slice_index, int32_t p_sub_instance) {
	User user;
	user.path = p_path;
	user.uv_scale = p_uv_scale;
	user.slice_index = p_slice_index;
	user.sub_instance = p_sub_instance;
	users.push_back(user);
}

int LightmapGIData::get_user_count() const {
	return users.size();
}

NodePath LightmapGIData::get_user_path(int p_user) const {
	ERR_FAIL_INDEX_V(p_user, users.size(), NodePath());
	return users[p_user].path;
}

int32_t LightmapGIData::get_user_sub_instance(int p_user) const {
	ERR_FAIL_INDEX_V(p_user, users.size(), -1);
	return users[p_user].sub_instance;
}

Rect2 LightmapGIData::get_user_lightmap_uv_scale(int p_user) const {
	ERR_FAIL_INDEX_V(p_user, users.size(), Rect2());
	return users[p_user].uv_scale;
}

int LightmapGIData::get_user_lightmap_slice_index(int p_user) const {
	ERR_FAIL_INDEX_V(p_user, users.size(), -1);
	return users[p_user].slice_index;
}

void LightmapGIData::clear_users() {
	users.clear();
}

void LightmapGIData::_set_user_data(const Array &p_data) {
	ERR_FAIL_COND((p_data.size() % 4) != 0);
	users.clear();
	for (int i = 0; i < p_data.size(); i += 4) {
		add_user(p_data[i + 0], p_data[i + 1], p_data[i + 2], p_data[i + 3]);
	}
}

Array LightmapGIData::_get_user_data() const {
	Array ret;
	for (int i = 0; i < users.size(); i++) {
		ret.push_back(users[i].path);
		ret.push_back(users[i].uv_scale);
		ret.push_back(users[i].slice_index);
		ret.push_back(users[i].sub_instance);
	}
	return ret;
}

void LightmapGIData::set_lightmap_textures(const TypedArray<TextureLayered> &p_data) {
	storage_light_textures = p_data;
	if (p_data.is_empty()) {
		combined_light_texture = Ref<TextureLayered>();
		_reset_lightmap_textures();
		return;
	}

	if (p_data.size() == 1) {
		combined_light_texture = p_data[0];
	} else {
		Vector<Ref<Image>> images;
		for (int i = 0; i < p_data.size(); i++) {
			Ref<TextureLayered> texture = p_data[i];
			ERR_FAIL_COND_MSG(texture.is_null(), vformat("Invalid TextureLayered at index %d.", i));
			for (int j = 0; j < texture->get_layers(); j++) {
				images.push_back(texture->get_layer_data(j));
			}
		}

		Ref<Texture2DArray> combined_texture;
		combined_texture.instantiate();

		combined_texture->create_from_images(images);
		combined_light_texture = combined_texture;
	}
	_reset_lightmap_textures();
}

TypedArray<TextureLayered> LightmapGIData::get_lightmap_textures() const {
	return storage_light_textures;
}

void LightmapGIData::set_shadowmask_textures(const TypedArray<TextureLayered> &p_data) {
	storage_shadowmask_textures = p_data;

	if (p_data.is_empty()) {
		combined_shadowmask_texture = Ref<TextureLayered>();
		_reset_shadowmask_textures();
		return;
	}

	if (p_data.size() == 1) {
		combined_shadowmask_texture = p_data[0];

	} else {
		Vector<Ref<Image>> images;
		for (int i = 0; i < p_data.size(); i++) {
			Ref<TextureLayered> texture = p_data[i];
			ERR_FAIL_COND_MSG(texture.is_null(), vformat("Invalid TextureLayered at index %d.", i));
			for (int j = 0; j < texture->get_layers(); j++) {
				images.push_back(texture->get_layer_data(j));
			}
		}

		Ref<Texture2DArray> combined_texture;
		combined_texture.instantiate();

		combined_texture->create_from_images(images);
		combined_shadowmask_texture = combined_texture;
	}

	_reset_shadowmask_textures();
}

TypedArray<TextureLayered> LightmapGIData::get_shadowmask_textures() const {
	return storage_shadowmask_textures;
}

void LightmapGIData::clear_shadowmask_textures() {
	RS::get_singleton()->lightmap_set_shadowmask_textures(lightmap, RID());
	storage_shadowmask_textures.clear();
	combined_shadowmask_texture.unref();
}

bool LightmapGIData::has_shadowmask_textures() {
	return !storage_shadowmask_textures.is_empty() && combined_shadowmask_texture.is_valid();
}

RID LightmapGIData::get_rid() const {
	return lightmap;
}

void LightmapGIData::clear() {
	users.clear();
}

void LightmapGIData::_reset_lightmap_textures() {
	RS::get_singleton()->lightmap_set_textures(lightmap, combined_light_texture.is_valid() ? combined_light_texture->get_rid() : RID(), uses_spherical_harmonics);
}

void LightmapGIData::_reset_shadowmask_textures() {
	RS::get_singleton()->lightmap_set_shadowmask_textures(lightmap, combined_shadowmask_texture.is_valid() ? combined_shadowmask_texture->get_rid() : RID());
}

void LightmapGIData::set_uses_spherical_harmonics(bool p_enable) {
	uses_spherical_harmonics = p_enable;
	_reset_lightmap_textures();
}

bool LightmapGIData::is_using_spherical_harmonics() const {
	return uses_spherical_harmonics;
}

void LightmapGIData::_set_uses_packed_directional(bool p_enable) {
	_uses_packed_directional = p_enable;
}

bool LightmapGIData::_is_using_packed_directional() const {
	return _uses_packed_directional;
}

void LightmapGIData::update_shadowmask_mode(ShadowmaskMode p_mode) {
	RS::get_singleton()->lightmap_set_shadowmask_mode(lightmap, (RSE::ShadowmaskMode)p_mode);
}

LightmapGIData::ShadowmaskMode LightmapGIData::get_shadowmask_mode() const {
	return (ShadowmaskMode)RS::get_singleton()->lightmap_get_shadowmask_mode(lightmap);
}

void LightmapGIData::update_specular_intensity(float p_intensity) {
	RS::get_singleton()->lightmap_set_specular_intensity(lightmap, p_intensity);
}

void LightmapGIData::set_capture_data(const AABB &p_bounds, bool p_interior, const PackedVector3Array &p_points, const PackedColorArray &p_point_sh, const PackedInt32Array &p_tetrahedra, const PackedInt32Array &p_bsp_tree, float p_baked_exposure, uint32_t p_lightprobe_hash) {
	if (p_points.size()) {
		ERR_FAIL_COND(p_points.size() * 9 != p_point_sh.size());
		ERR_FAIL_COND(p_tetrahedra.size() % 4 != 0 || p_bsp_tree.size() % 6 != 0);
	}
	if (p_points.size()) {
		RS::get_singleton()->lightmap_set_probe_capture_data(lightmap, p_points, p_point_sh, p_tetrahedra, p_bsp_tree);
		RS::get_singleton()->lightmap_set_probe_bounds(lightmap, p_bounds);
		RS::get_singleton()->lightmap_set_probe_interior(lightmap, p_interior);
	} else {
		RS::get_singleton()->lightmap_set_probe_capture_data(lightmap, PackedVector3Array(), PackedColorArray(), PackedInt32Array(), PackedInt32Array());
		RS::get_singleton()->lightmap_set_probe_bounds(lightmap, p_bounds);
		RS::get_singleton()->lightmap_set_probe_interior(lightmap, false);
	}
	RS::get_singleton()->lightmap_set_baked_exposure_normalization(lightmap, p_baked_exposure);
	baked_exposure = p_baked_exposure;
	lightprobe_hash = p_lightprobe_hash;
	has_probe_capture_data.set_to(!p_points.is_empty());
	interior = p_interior;
	bounds = p_bounds;
}

PackedVector3Array LightmapGIData::get_capture_points() const {
	return RS::get_singleton()->lightmap_get_probe_capture_points(lightmap);
}

PackedColorArray LightmapGIData::get_capture_sh() const {
	return RS::get_singleton()->lightmap_get_probe_capture_sh(lightmap);
}

PackedInt32Array LightmapGIData::get_capture_tetrahedra() const {
	return RS::get_singleton()->lightmap_get_probe_capture_tetrahedra(lightmap);
}

PackedInt32Array LightmapGIData::get_capture_bsp_tree() const {
	return RS::get_singleton()->lightmap_get_probe_capture_bsp_tree(lightmap);
}

uint32_t LightmapGIData::get_lightprobe_hash() const {
	return lightprobe_hash;
}

AABB LightmapGIData::get_capture_bounds() const {
	return bounds;
}

bool LightmapGIData::is_interior() const {
	return interior;
}

float LightmapGIData::get_baked_exposure() const {
	return baked_exposure;
}

void LightmapGIData::_set_probe_data(const Dictionary &p_data) {
	ERR_FAIL_COND(!p_data.has("bounds"));
	ERR_FAIL_COND(!p_data.has("points"));
	ERR_FAIL_COND(!p_data.has("tetrahedra"));
	ERR_FAIL_COND(!p_data.has("bsp"));
	ERR_FAIL_COND(!p_data.has("sh"));
	ERR_FAIL_COND(!p_data.has("interior"));
	ERR_FAIL_COND(!p_data.has("baked_exposure"));

	uint32_t phash = 0;
	if (p_data.has("lightprobe_hash")) { // Older versions will not have it.
		phash = p_data["lightprobe_hash"];
	}
	// Restore full connectivity from captures saved with geometry filtering.
	const PackedInt32Array topology = p_data.get("topology_tetrahedra", p_data["tetrahedra"]);
	set_capture_data(p_data["bounds"], p_data["interior"], p_data["points"], p_data["sh"], topology, p_data["bsp"], p_data["baked_exposure"], phash);
}

Dictionary LightmapGIData::_get_probe_data() const {
	Dictionary d;
	d["bounds"] = get_capture_bounds();
	d["points"] = get_capture_points();
	d["tetrahedra"] = get_capture_tetrahedra();
	d["bsp"] = get_capture_bsp_tree();
	d["sh"] = get_capture_sh();
	d["interior"] = is_interior();
	d["baked_exposure"] = get_baked_exposure();
	d["lightprobe_hash"] = lightprobe_hash;
	return d;
}

#ifndef DISABLE_DEPRECATED
void LightmapGIData::set_light_texture(const Ref<TextureLayered> &p_light_texture) {
	TypedArray<TextureLayered> arr = { p_light_texture };
	set_lightmap_textures(arr);
}

Ref<TextureLayered> LightmapGIData::get_light_texture() const {
	if (storage_light_textures.is_empty()) {
		return Ref<TextureLayered>();
	}
	return storage_light_textures.get(0);
}

void LightmapGIData::_set_light_textures_data(const Array &p_data) {
	set_lightmap_textures(p_data);
}

Array LightmapGIData::_get_light_textures_data() const {
	return Array(storage_light_textures);
}
#endif

void LightmapGIData::set_baked_exposure(float p_exposure) {
	ERR_FAIL_COND(!Math::is_finite(p_exposure) || p_exposure < 0);
	baked_exposure = p_exposure;
	RS::get_singleton()->lightmap_set_baked_exposure_normalization(lightmap, baked_exposure);
}

void LightmapGIData::set_lightmap_bounds(const AABB &p_bounds) {
	ERR_FAIL_COND(!p_bounds.position.is_finite() || !p_bounds.size.is_finite());
	ERR_FAIL_COND(p_bounds.size.x < 0 || p_bounds.size.y < 0 || p_bounds.size.z < 0);
	bounds = p_bounds;
	RS::get_singleton()->lightmap_set_probe_bounds(lightmap, bounds);
}

void LightmapGIData::_validate_property(PropertyInfo &p_property) const {
	// Preview threads validate properties while holding the ClassDB read lock.
	// Querying the RenderingServer here can deadlock with the main thread.
	if (p_property.name == "probe_data" && !has_probe_capture_data.is_set()) {
		p_property.usage &= ~PROPERTY_USAGE_STORAGE;
	}
}

void LightmapGIData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_lightmap_bounds", "bounds"), &LightmapGIData::set_lightmap_bounds);
	ClassDB::bind_method(D_METHOD("_get_lightmap_bounds"), &LightmapGIData::get_capture_bounds);
	ADD_PROPERTY(PropertyInfo(Variant::AABB, "_lightmap_bounds", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_INTERNAL), "set_lightmap_bounds", "_get_lightmap_bounds");
	ClassDB::bind_method(D_METHOD("set_baked_exposure", "exposure"), &LightmapGIData::set_baked_exposure);
	ClassDB::bind_method(D_METHOD("get_baked_exposure"), &LightmapGIData::get_baked_exposure);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "baked_exposure", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_INTERNAL), "set_baked_exposure", "get_baked_exposure");
	ClassDB::bind_method(D_METHOD("_set_user_data", "data"), &LightmapGIData::_set_user_data);
	ClassDB::bind_method(D_METHOD("_get_user_data"), &LightmapGIData::_get_user_data);

	ClassDB::bind_method(D_METHOD("set_lightmap_textures", "light_textures"), &LightmapGIData::set_lightmap_textures);
	ClassDB::bind_method(D_METHOD("get_lightmap_textures"), &LightmapGIData::get_lightmap_textures);

	ClassDB::bind_method(D_METHOD("set_shadowmask_textures", "shadowmask_textures"), &LightmapGIData::set_shadowmask_textures);
	ClassDB::bind_method(D_METHOD("get_shadowmask_textures"), &LightmapGIData::get_shadowmask_textures);

	ClassDB::bind_method(D_METHOD("set_uses_spherical_harmonics", "uses_spherical_harmonics"), &LightmapGIData::set_uses_spherical_harmonics);
	ClassDB::bind_method(D_METHOD("is_using_spherical_harmonics"), &LightmapGIData::is_using_spherical_harmonics);

	ClassDB::bind_method(D_METHOD("_set_uses_packed_directional", "_uses_packed_directional"), &LightmapGIData::_set_uses_packed_directional);
	ClassDB::bind_method(D_METHOD("_is_using_packed_directional"), &LightmapGIData::_is_using_packed_directional);

	ClassDB::bind_method(D_METHOD("add_user", "path", "uv_scale", "slice_index", "sub_instance"), &LightmapGIData::add_user);
	ClassDB::bind_method(D_METHOD("get_user_count"), &LightmapGIData::get_user_count);
	ClassDB::bind_method(D_METHOD("get_user_path", "user_idx"), &LightmapGIData::get_user_path);
	ClassDB::bind_method(D_METHOD("clear_users"), &LightmapGIData::clear_users);

	ClassDB::bind_method(D_METHOD("_set_probe_data", "data"), &LightmapGIData::_set_probe_data);
	ClassDB::bind_method(D_METHOD("_get_probe_data"), &LightmapGIData::_get_probe_data);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "lightmap_textures", PROPERTY_HINT_ARRAY_TYPE, "TextureLayered", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "set_lightmap_textures", "get_lightmap_textures");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "shadowmask_textures", PROPERTY_HINT_ARRAY_TYPE, "TextureLayered", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "set_shadowmask_textures", "get_shadowmask_textures");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "uses_spherical_harmonics", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "set_uses_spherical_harmonics", "is_using_spherical_harmonics");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "user_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_user_data", "_get_user_data");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "probe_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_probe_data", "_get_probe_data");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "_uses_packed_directional", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_uses_packed_directional", "_is_using_packed_directional");

#ifndef DISABLE_DEPRECATED
	ClassDB::bind_method(D_METHOD("set_light_texture", "light_texture"), &LightmapGIData::set_light_texture);
	ClassDB::bind_method(D_METHOD("get_light_texture"), &LightmapGIData::get_light_texture);

	ClassDB::bind_method(D_METHOD("_set_light_textures_data", "data"), &LightmapGIData::_set_light_textures_data);
	ClassDB::bind_method(D_METHOD("_get_light_textures_data"), &LightmapGIData::_get_light_textures_data);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "light_texture", PROPERTY_HINT_RESOURCE_TYPE, TextureLayered::get_class_static(), PROPERTY_USAGE_NONE), "set_light_texture", "get_light_texture");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "light_textures", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_INTERNAL), "_set_light_textures_data", "_get_light_textures_data");
#endif

	BIND_ENUM_CONSTANT(SHADOWMASK_MODE_NONE);
	BIND_ENUM_CONSTANT(SHADOWMASK_MODE_REPLACE);
	BIND_ENUM_CONSTANT(SHADOWMASK_MODE_OVERLAY);
}

LightmapGIData::LightmapGIData() {
	lightmap = RS::get_singleton()->lightmap_create();
}

LightmapGIData::~LightmapGIData() {
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	RS::get_singleton()->free_rid(lightmap);
}

///////////////////////////

void LightmapGI::_find_meshes_and_lights(Node *p_at_node, Vector<MeshesFound> &meshes, Vector<LightsFound> &lights) {
	MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_at_node);
	if (mi && mi->get_gi_mode() == GeometryInstance3D::GI_MODE_STATIC && mi->is_visible_in_tree()) {
		Ref<Mesh> mesh = mi->get_mesh();
		if (mesh.is_valid()) {
			bool all_have_uv2_and_normal = true;
			bool surfaces_found = false;
			for (int i = 0; i < mesh->get_surface_count(); i++) {
				if (mesh->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
					continue;
				}
				if (!(mesh->surface_get_format(i) & Mesh::ARRAY_FORMAT_TEX_UV2)) {
					all_have_uv2_and_normal = false;
					break;
				}
				if (!(mesh->surface_get_format(i) & Mesh::ARRAY_FORMAT_NORMAL)) {
					all_have_uv2_and_normal = false;
					break;
				}
				surfaces_found = true;
			}

			if (surfaces_found && all_have_uv2_and_normal) {
				//READY TO BAKE! size hint could be computed if not found, actually..

				MeshesFound mf;
				mf.xform = get_global_transform().affine_inverse() * mi->get_global_transform();
				mf.node_path = get_path_to(mi);
				mf.subindex = -1;
				mf.mesh = mesh;
				mf.lightmap_scale = mi->get_lightmap_texel_scale();

				Ref<Material> all_override = mi->get_material_override();
				for (int i = 0; i < mesh->get_surface_count(); i++) {
					if (all_override.is_valid()) {
						mf.overrides.push_back(all_override);
					} else {
						mf.overrides.push_back(mi->get_surface_override_material(i));
					}
				}

				meshes.push_back(mf);
			}
		}
	}

	Node3D *s = Object::cast_to<Node3D>(p_at_node);

	if (!mi && s && p_at_node->has_method(SNAME("get_bake_meshes"))) {
		Array bmeshes = p_at_node->call("get_bake_meshes");
		if (bmeshes.size() && (bmeshes.size() & 1) == 0) {
			Transform3D xf = get_global_transform().affine_inverse() * s->get_global_transform();
			for (int i = 0; i < bmeshes.size(); i += 2) {
				Ref<Mesh> mesh = bmeshes[i];
				if (mesh.is_null()) {
					continue;
				}

				MeshesFound mf;

				Transform3D mesh_xf = bmeshes[i + 1];
				mf.xform = xf * mesh_xf;
				mf.node_path = get_path_to(s);
				mf.subindex = i / 2;
				mf.lightmap_scale = 1.0;
				mf.mesh = mesh;

				meshes.push_back(mf);
			}
		}
	}

	Light3D *light = Object::cast_to<Light3D>(p_at_node);

	if (light && light->get_bake_mode() != Light3D::BAKE_DISABLED) {
		LightsFound lf;
		lf.xform = get_global_transform().affine_inverse() * light->get_global_transform();
		lf.light = light;
		lights.push_back(lf);
	}

	for (int i = 0; i < p_at_node->get_child_count(); i++) {
		Node *child = p_at_node->get_child(i);
		if (!child->get_owner()) {
			continue; //maybe a helper
		}

		_find_meshes_and_lights(child, meshes, lights);
	}
}

bool LightmapGI::_lightmap_bake_step_function(float p_completion, const String &p_text, void *ud, bool p_refresh) {
	BakeStepUD *bsud = (BakeStepUD *)ud;
	bool ret = false;
	if (bsud->func) {
		ret = bsud->func(bsud->from_percent + p_completion * (bsud->to_percent - bsud->from_percent), p_text, bsud->ud, p_refresh);
	}
	return ret;
}

LightmapGI::BakeError LightmapGI::_save_and_reimport_atlas_textures(const Ref<Lightmapper> p_lightmapper, const String &p_base_name, TypedArray<TextureLayered> &r_textures, bool p_is_shadowmask) const {
	Vector<Ref<Image>> images;
	images.resize(p_is_shadowmask ? p_lightmapper->get_shadowmask_texture_count() : p_lightmapper->get_bake_texture_count());

	for (int i = 0; i < images.size(); i++) {
		images.set(i, p_is_shadowmask ? p_lightmapper->get_shadowmask_texture(i) : p_lightmapper->get_bake_texture(i));
	}

	const int slice_count = images.size();
	const int slice_width = images[0]->get_width();
	const int slice_height = images[0]->get_height();
	const int slice_pixels = slice_width * slice_height;

	// Stacked slices must fit both the image dimension and total pixel limits.
	const int slices_per_texture = MIN(Image::MAX_HEIGHT / slice_height, Image::MAX_PIXELS / slice_pixels);
	ERR_FAIL_COND_V(slices_per_texture == 0, LightmapGI::BAKE_ERROR_CANT_CREATE_IMAGE);
	const int texture_count = Math::ceil(slice_count / (float)slices_per_texture);
	const int last_count = slice_count % slices_per_texture;

	r_textures.resize(texture_count);

	for (int i = 0; i < texture_count; i++) {
		const int texture_slice_count = (i == texture_count - 1 && last_count != 0) ? last_count : slices_per_texture;

		Ref<Image> texture_image = Image::create_empty(slice_width, slice_height * texture_slice_count, false, images[0]->get_format());

		for (int j = 0; j < texture_slice_count; j++) {
			texture_image->blit_rect(images[i * slices_per_texture + j], Rect2i(0, 0, slice_width, slice_height), Point2i(0, slice_height * j));
		}

		const String atlas_path = (texture_count > 1 ? p_base_name + "_" + itos(i) : p_base_name) + (p_is_shadowmask ? ".png" : ".exr");
		const String config_path = atlas_path + ".import";

		Ref<ConfigFile> config;
		config.instantiate();

		// Load an import configuration if present.
		if (FileAccess::exists(config_path)) {
			config->load(config_path);
		}

		config->set_value("remap", "importer", "2d_array_texture");
		config->set_value("remap", "type", "CompressedTexture2DArray");
		if (!config->has_section_key("params", "compress/mode")) {
			// Do not override an existing compression mode.
			config->set_value("params", "compress/mode", 2);
		}
		config->set_value("params", "compress/channel_pack", 1);
		config->set_value("params", "mipmaps/generate", false);
		config->set_value("params", "slices/horizontal", 1);
		config->set_value("params", "slices/vertical", texture_slice_count);

		config->save(config_path);

		if (supersampling_enabled) {
			texture_image->resize(texture_image->get_width() / supersampling_factor, texture_image->get_height() / supersampling_factor, Image::INTERPOLATE_TRILINEAR);
		}

		// Save the file.
		Error save_err;
		if (p_is_shadowmask) {
			save_err = texture_image->save_png(atlas_path);
		} else {
			save_err = texture_image->save_exr(atlas_path, false);
		}

		ERR_FAIL_COND_V(save_err, LightmapGI::BAKE_ERROR_CANT_CREATE_IMAGE);

		// Reimport the file.
		ResourceLoader::import(atlas_path);
		Ref<TextureLayered> t = ResourceLoader::load(atlas_path); // If already loaded, it will be updated on refocus?
		ERR_FAIL_COND_V(t.is_null(), LightmapGI::BAKE_ERROR_CANT_CREATE_IMAGE);

		// Store the atlas in the array.
		r_textures[i] = t;
	}

	return LightmapGI::BAKE_ERROR_OK;
}

void LightmapGI::_build_area_light_texture_atlas(const Vector<LightmapGI::LightsFound> &lights_found, HashMap<Ref<Texture2D>, AreaLightAtlasTexture> &r_textures, Size2i &r_atlas_size, int &r_mipmaps) const {
	if (RenderingServer::get_singleton()->get_current_rendering_method() != "gl_compatibility") { // area light textures unsupported in compat
		r_mipmaps = 8;
		r_atlas_size = Size2i(pow(2, r_mipmaps), pow(2, r_mipmaps));

		for (int i = 0; i < lights_found.size(); i++) {
			Light3D *light = lights_found[i].light;
			if (Object::cast_to<AreaLight3D>(light)) {
				AreaLight3D *l = Object::cast_to<AreaLight3D>(light);
				if (l->get_area_texture().is_valid() && !r_textures.has(l->get_area_texture())) {
					r_textures[l->get_area_texture()] = AreaLightAtlasTexture();
				}
			} else if (light->get_bake_mode() == Light3D::BAKE_STATIC && light->get_projector().is_valid() && !r_textures.has(light->get_projector())) {
				r_textures[light->get_projector()] = AreaLightAtlasTexture();
			}
		}
	}

	if (r_textures.size() == 0) {
		r_mipmaps = 1;
		r_atlas_size = Size2i(1, 1);
		return;
	}

	struct SortItem {
		Ref<Texture2D> texture;
		Size2i pixel_size;
		Size2i size;
		Point2i pos;

		bool operator<(const SortItem &p_item) const {
			//sort larger to smaller
			if (size.height == p_item.size.height) {
				return size.width > p_item.size.width;
			} else {
				return size.height > p_item.size.height;
			}
		}
	};

	//generate atlas
	Vector<SortItem> itemsv;
	itemsv.resize(r_textures.size());
	uint32_t base_size = 1;

	int idx = 0;
	int border = 1 << (r_mipmaps - 1);

	for (const KeyValue<Ref<Texture2D>, AreaLightAtlasTexture> &E : r_textures) {
		Ref<Texture2D> tex = E.key;
		Size2i tex_size = Size2i(tex->get_width(), tex->get_height());

		SortItem &si = itemsv.write[idx];

		Vector2i b_size = Vector2i(Math::ceil(float(tex_size.width) / border), Math::ceil(float(tex_size.height) / border));
		si.size.width = b_size.width + 1;
		si.size.height = b_size.height + 1;
		si.pixel_size = b_size * border; // components are either small powers of 2 or N * border
		if (tex_size.width < border) {
			si.pixel_size.width = Math::nearest_power_of_2_templated(tex_size.width);
		}
		if (tex_size.height < border) {
			si.pixel_size.height = Math::nearest_power_of_2_templated(tex_size.height);
		}

		if (base_size < (uint32_t)si.size.width) {
			base_size = Math::nearest_power_of_2_templated(si.size.width);
		}

		si.texture = tex;
		idx++;
	}

	//sort items by size
	itemsv.sort();

	//attempt to create atlas
	int item_count = itemsv.size();
	SortItem *items = itemsv.ptrw();

	int atlas_height = 0;

	while (true) {
		Vector<int> v_offsetsv;
		v_offsetsv.resize(base_size);

		int *v_offsets = v_offsetsv.ptrw();
		memset(v_offsets, 0, sizeof(int) * base_size);

		int max_height = 0;

		for (int i = 0; i < item_count; i++) {
			//best fit
			SortItem &si = items[i];
			int best_idx = -1;
			int best_height = 0x7FFFFFFF;
			for (uint32_t j = 0; j <= base_size - si.size.width; j++) {
				int height = 0;
				for (int k = 0; k < si.size.width; k++) {
					int h = v_offsets[k + j];
					if (h > height) {
						height = h;
						if (height > best_height) {
							break; //already bad
						}
					}
				}

				if (height < best_height) {
					best_height = height;
					best_idx = j;
				}
			}

			//update
			for (int k = 0; k < si.size.width; k++) {
				v_offsets[k + best_idx] = best_height + si.size.height;
			}

			si.pos.x = best_idx;
			si.pos.y = best_height;

			if (si.pos.y + si.size.height > max_height) {
				max_height = si.pos.y + si.size.height;
			}
		}

		if ((uint32_t)max_height <= base_size * 2) {
			atlas_height = max_height;
			break; //good ratio, break;
		}

		base_size *= 2;
	}

	r_atlas_size.width = base_size * border;
	r_atlas_size.height = Math::nearest_power_of_2_templated(atlas_height * border);

	for (int i = 0; i < item_count; i++) {
		Rect2 uv_rect;
		uv_rect.position = items[i].pos * border;
		uv_rect.size = items[i].pixel_size;

		uv_rect.position /= Size2(r_atlas_size);
		uv_rect.size /= Size2(r_atlas_size);
		r_textures[items[i].texture].texture_rect = uv_rect;
		float max_mipmap = MIN(Math::floor(Math::log2(MAX(MIN(items[i].pixel_size.x, items[i].pixel_size.y), 1.0f))), r_mipmaps) - 1.0f;
		r_textures[items[i].texture].max_mipmap = max_mipmap;
	}
}

Lightmapper::BakeError LightmapGI::_bake_material(int p_mesh_index, const Size2i &p_size, Ref<Image> &r_albedo, Ref<Image> &r_emission, void *p_userdata) {
	BakeMaterialsUD *ud = static_cast<BakeMaterialsUD *>(p_userdata);
	ERR_FAIL_NULL_V(ud, Lightmapper::BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
	ERR_FAIL_NULL_V(ud->meshes, Lightmapper::BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
	ERR_FAIL_INDEX_V(p_mesh_index, ud->meshes->size(), Lightmapper::BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);

	MeshesFound &mf = ud->meshes->write[p_mesh_index];
	TypedArray<RID> overrides;
	overrides.resize(mf.overrides.size());
	for (int i = 0; i < mf.overrides.size(); i++) {
		if (mf.overrides[i].is_valid()) {
			overrides[i] = mf.overrides[i]->get_rid();
		}
	}

	TypedArray<Image> images = RS::get_singleton()->bake_render_uv2(mf.mesh->get_rid(), overrides, p_size, ud->exposure_normalization);
	ERR_FAIL_COND_V(images.size() <= RSE::BAKE_CHANNEL_EMISSION, Lightmapper::BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);

	Ref<Image> albedo = images[RSE::BAKE_CHANNEL_ALBEDO_ALPHA];
	Ref<Image> orm = images[RSE::BAKE_CHANNEL_ORM];
	r_emission = images[RSE::BAKE_CHANNEL_EMISSION];
	images.clear();
	ERR_FAIL_COND_V(albedo.is_null() || orm.is_null() || r_emission.is_null(), Lightmapper::BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
	if (albedo->get_format() != Image::FORMAT_RGBA8) {
		albedo->convert(Image::FORMAT_RGBA8);
	}
	if (orm->get_format() != Image::FORMAT_RGBA8) {
		orm->convert(Image::FORMAT_RGBA8);
	}

	Vector<uint8_t> albedo_alpha = albedo->get_data();
	Vector<uint8_t> orm_data = orm->get_data();
	ERR_FAIL_COND_V(albedo_alpha.size() != orm_data.size(), Lightmapper::BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
	Vector<uint8_t> diffuse_albedo;
	const uint32_t data_size = albedo_alpha.size();
	diffuse_albedo.resize(data_size);
	const uint8_t *r_albedo_alpha = albedo_alpha.ptr();
	const uint8_t *r_orm = orm_data.ptr();
	uint8_t *w_diffuse_albedo = diffuse_albedo.ptrw();
	for (uint32_t i = 0; i < data_size; i += 4) {
		w_diffuse_albedo[i + 0] = uint8_t(CLAMP(float(r_albedo_alpha[i + 0]) * (1.0 - float(r_orm[i + 2] / 255.0)), 0, 255));
		w_diffuse_albedo[i + 1] = uint8_t(CLAMP(float(r_albedo_alpha[i + 1]) * (1.0 - float(r_orm[i + 2] / 255.0)), 0, 255));
		w_diffuse_albedo[i + 2] = uint8_t(CLAMP(float(r_albedo_alpha[i + 2]) * (1.0 - float(r_orm[i + 2] / 255.0)), 0, 255));
		w_diffuse_albedo[i + 3] = r_albedo_alpha[i + 3];
	}

	r_albedo = Image::create_from_data(p_size.x, p_size.y, false, Image::FORMAT_RGBA8, diffuse_albedo);
	if (r_emission->get_format() != Image::FORMAT_RGBAH) {
		r_emission->convert(Image::FORMAT_RGBAH);
	}
	return Lightmapper::BAKE_OK;
}

LightmapGI::BakeError LightmapGI::bake(Node *p_from_node, String p_image_data_path, Lightmapper::BakeStepFunc p_bake_step, void *p_bake_userdata, BakeMode p_mode) {
	ERR_FAIL_COND_V(p_mode < BAKE_MODE_LIGHTMAP_AND_PROBES || p_mode > BAKE_MODE_PROBES_ONLY, BAKE_ERROR_MESHES_INVALID);
	const bool bake_lightmaps = p_mode != BAKE_MODE_PROBES_ONLY;
	const bool bake_probes = p_mode != BAKE_MODE_LIGHTMAP_ONLY;
	_migrate_legacy_probe_data();
	Vector<LightProbeSystem *> probe_systems;
	collect_probe_systems(this, this, probe_systems);
	if (p_mode == BAKE_MODE_PROBES_ONLY && probe_systems.is_empty()) {
		return BAKE_ERROR_NO_PROBE_SYSTEM;
	}
	Vector<Ref<LightProbeData>> old_probe_resources;
	HashSet<ObjectID> seen_probe_resources;
	HashSet<ObjectID> shared_probe_resources;
	for (LightProbeSystem *system : probe_systems) {
		const Ref<LightProbeData> capture = system->get_light_data();
		if (capture.is_null()) {
			continue;
		}
		const ObjectID id = capture->get_instance_id();
		if (seen_probe_resources.has(id)) {
			shared_probe_resources.insert(id);
		} else {
			seen_probe_resources.insert(id);
			old_probe_resources.push_back(capture);
		}
	}
	const Ref<LightProbeData> previous_probe_capture = old_probe_resources.is_empty() ? Ref<LightProbeData>() : old_probe_resources[0];
	if (p_image_data_path.is_empty()) {
		if (!bake_lightmaps && previous_probe_capture.is_valid() && previous_probe_capture->get_path().is_resource_file()) {
			p_image_data_path = previous_probe_capture->get_path();
		} else if (get_light_data().is_valid() && get_light_data()->get_path().is_resource_file()) {
			p_image_data_path = get_light_data()->get_path();
		} else {
			return BAKE_ERROR_NO_SAVE_PATH;
		}
	}
	const Ref<LightmapGIData> previous_lightmap_data = get_light_data();

	Ref<Lightmapper> lightmapper = Lightmapper::create();
	ERR_FAIL_COND_V(lightmapper.is_null(), BAKE_ERROR_NO_LIGHTMAPPER);

	struct PreviousLightmapRestore {
		LightmapGI *owner = nullptr;
		Ref<LightmapGIData> data;
		Vector<String> light_texture_paths;
		Vector<String> shadow_texture_paths;
		bool restore = true;
		bool mutated = false;
		bool atlases_unloaded = false;
		Dictionary probe_backup;
		Array user_backup;
		TypedArray<TextureLayered> light_backup;
		TypedArray<TextureLayered> shadow_backup;
		bool sh_backup = false;
		bool packed_backup = false;
		String path_backup;

		void backup_before_mutation(const Ref<LightmapGIData> &p_data) {
			data = p_data;
			probe_backup = data->get("probe_data");
			user_backup = data->get("user_data");
			light_backup = data->get_lightmap_textures();
			shadow_backup = data->get_shadowmask_textures();
			sh_backup = data->is_using_spherical_harmonics();
			packed_backup = data->_is_using_packed_directional();
			path_backup = data->get_path();
			mutated = true;
		}

		void restore_if_needed() {
			if (!restore || data.is_null()) {
				return;
			}
			if (!mutated && !atlases_unloaded) {
				return;
			}
			if (mutated) {
				data->set("user_data", user_backup);
				data->set_uses_spherical_harmonics(sh_backup);
				data->_set_uses_packed_directional(packed_backup);
				data->set("probe_data", probe_backup);
				data->set_path(path_backup, true);
			}
			TypedArray<TextureLayered> light_textures = light_backup;
			for (const String &path : light_texture_paths) {
				Ref<TextureLayered> texture = ResourceLoader::load(path);
				if (texture.is_valid()) {
					light_textures.push_back(texture);
				}
			}
			TypedArray<TextureLayered> shadow_textures = shadow_backup;
			for (const String &path : shadow_texture_paths) {
				Ref<TextureLayered> texture = ResourceLoader::load(path);
				if (texture.is_valid()) {
					shadow_textures.push_back(texture);
				}
			}
			data->set_lightmap_textures(light_textures);
			data->set_shadowmask_textures(shadow_textures);
			owner->set_light_data(data);
			restore = false;
		}

		~PreviousLightmapRestore() {
			restore_if_needed();
		}
	} previous_lightmap;

	previous_lightmap.owner = this;
	previous_lightmap.data = get_light_data();
	if (bake_lightmaps && previous_lightmap.data.is_valid()) {
		const TypedArray<TextureLayered> light_textures = previous_lightmap.data->get_lightmap_textures();
		const TypedArray<TextureLayered> shadow_textures = previous_lightmap.data->get_shadowmask_textures();
		bool can_unload = true;
		for (int i = 0; i < light_textures.size(); i++) {
			Ref<TextureLayered> texture = light_textures[i];
			if (texture.is_null() || texture->get_path().is_empty()) {
				can_unload = false;
				break;
			}
			previous_lightmap.light_texture_paths.push_back(texture->get_path());
		}
		for (int i = 0; can_unload && i < shadow_textures.size(); i++) {
			Ref<TextureLayered> texture = shadow_textures[i];
			if (texture.is_null() || texture->get_path().is_empty()) {
				can_unload = false;
				break;
			}
			previous_lightmap.shadow_texture_paths.push_back(texture->get_path());
		}
		if (can_unload) {
			previous_lightmap.atlases_unloaded = true;
			set_light_data(Ref<LightmapGIData>());
			previous_lightmap.data->set_lightmap_textures(TypedArray<TextureLayered>());
			previous_lightmap.data->set_shadowmask_textures(TypedArray<TextureLayered>());
			RenderingServer::get_singleton()->sync();
		} else {
			previous_lightmap.data = Ref<LightmapGIData>();
			previous_lightmap.light_texture_paths.clear();
			previous_lightmap.shadow_texture_paths.clear();
		}
	}

	BakeStepUD bsud;
	bsud.func = p_bake_step;
	bsud.ud = p_bake_userdata;
	bsud.from_percent = 0.2;
	bsud.to_percent = 0.8;

	if (p_bake_step) {
		p_bake_step(0.0, RTR("Finding meshes, lights and probes"), p_bake_userdata, true);
	}
	/* STEP 1, FIND MESHES, LIGHTS AND PROBES */
	Vector<Lightmapper::MeshData> mesh_data;
	Vector<MeshesFound> meshes_found;
	BakeMaterialsUD bake_materials_ud;
	bake_materials_ud.meshes = &meshes_found;
	Vector<LightsFound> lights_found;
	Vector<Vector3> probes_found;
	Vector<int> system_probe_counts;
	AABB bounds;
	{
		_find_meshes_and_lights(p_from_node ? p_from_node : get_parent(), meshes_found, lights_found);

		if (meshes_found.is_empty()) {
			return BAKE_ERROR_NO_MESHES;
		}
		// create mesh data for insert

		//get the base material textures, help compute atlas size and bounds
		for (int m_i = 0; m_i < meshes_found.size(); m_i++) {
			if (p_bake_step) {
				float p = (float)(m_i) / meshes_found.size();
				p_bake_step(p * 0.1, vformat(RTR("Preparing geometry %d/%d"), m_i, meshes_found.size()), p_bake_userdata, false);
			}

			MeshesFound &mf = meshes_found.write[m_i];

			Size2i mesh_lightmap_size = mf.mesh->get_lightmap_size_hint();
			if (mesh_lightmap_size == Size2i(0, 0)) {
				// TODO we should compute a size if no lightmap hint is set, as we did in 3.x.
				// For now set to basic size to avoid crash.
				mesh_lightmap_size = Size2i(64, 64);
			}
			// Double lightmap texel density if downsampling is enabled, as the final texture size will be halved before saving lightmaps.
			Size2i lightmap_size = Size2i(Size2(mesh_lightmap_size) * mf.lightmap_scale * texel_scale) * (supersampling_enabled ? supersampling_factor : 1.0);
			ERR_FAIL_COND_V(lightmap_size.x == 0 || lightmap_size.y == 0, BAKE_ERROR_LIGHTMAP_TOO_SMALL);

			Lightmapper::MeshData md;
			md.lightmap_size = lightmap_size;
			md.material_index = m_i;

			{
				Dictionary d;
				d["path"] = mf.node_path;
				if (mf.subindex >= 0) {
					d["subindex"] = mf.subindex;
				}
				md.userdata = d;
			}

			//get geometry

			md.rt_transform = mf.xform;
			Basis normal_xform = mf.xform.basis.inverse().transposed();

			for (int i = 0; i < mf.mesh->get_surface_count(); i++) {
				if (mf.mesh->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
					continue;
				}
				Array a = mf.mesh->surface_get_arrays(i);
				Ref<Material> mat = i < mf.overrides.size() && mf.overrides[i].is_valid() ? mf.overrides[i] : mf.mesh->surface_get_material(i);
				RID mat_rid;
				if (mat.is_valid()) {
					mat_rid = mat->get_rid();
				}
				float alpha_scissor_threshold = -1.0f;
				Ref<BaseMaterial3D> base_material = mat;
				if (base_material.is_valid() && base_material->get_transparency() == BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR) {
					alpha_scissor_threshold = base_material->get_alpha_scissor_threshold();
				}

				Vector<Vector3> vertices = a[Mesh::ARRAY_VERTEX];
				const Vector3 *vr = vertices.ptr();
				Vector<Vector2> uv = a[Mesh::ARRAY_TEX_UV2];
				const Vector2 *uvr = nullptr;
				Vector<Vector3> normals = a[Mesh::ARRAY_NORMAL];
				const Vector3 *nr = nullptr;
				Vector<int> index = a[Mesh::ARRAY_INDEX];

				ERR_CONTINUE(uv.is_empty());
				ERR_CONTINUE(normals.is_empty());

				uvr = uv.ptr();
				nr = normals.ptr();

				int facecount;
				const int *ir = nullptr;

				if (index.size()) {
					facecount = index.size() / 3;
					ir = index.ptr();
				} else {
					facecount = vertices.size() / 3;
				}

				for (int j = 0; j < facecount; j++) {
					md.alpha_scissor_threshold.push_back(alpha_scissor_threshold);
					uint32_t vidx[3];

					if (ir) {
						for (int k = 0; k < 3; k++) {
							vidx[k] = ir[j * 3 + k];
						}
					} else {
						for (int k = 0; k < 3; k++) {
							vidx[k] = j * 3 + k;
						}
					}

					for (int k = 0; k < 3; k++) {
						Vector3 v = mf.xform.xform(vr[vidx[k]]);
						if (bounds == AABB()) {
							bounds.position = v;
						} else {
							bounds.expand_to(v);
						}
						md.points.push_back(v);
						md.rt_points.push_back(vr[vidx[k]]);

						md.uv2.push_back(uvr[vidx[k]]);
						md.normal.push_back(normal_xform.xform(nr[vidx[k]]).normalized());
						md.material.push_back(mat_rid);
					}
				}
			}

			mesh_data.push_back(md);
		}
	}

	if (bake_probes && !probe_systems.is_empty()) {
		if (p_bake_step && p_bake_step(0.12, RTR("Preparing probe validation"), p_bake_userdata, true)) {
			return BAKE_ERROR_USER_ABORTED;
		}
		LightProbeGeometry geometry;
		Error error = geometry.build(p_from_node ? p_from_node : get_parent(), p_bake_step, p_bake_userdata);
		if (error != OK) {
			return error == ERR_SKIP ? BAKE_ERROR_USER_ABORTED : BAKE_ERROR_MESHES_INVALID;
		}
		// An explicit system supplies the whole probe layout. Never add legacy
		// generated corners or rejected placements back into the accepted set.
		probes_found.clear();
		for (LightProbeSystem *system : probe_systems) {
			Vector<Vector3> accepted;
			error = system->prepare_bake(geometry, get_global_transform(), accepted, p_bake_step, p_bake_userdata);
			if (error != OK) {
				return error == ERR_SKIP ? BAKE_ERROR_USER_ABORTED : BAKE_ERROR_MESHES_INVALID;
			}
			system_probe_counts.push_back(accepted.size());
			probes_found.append_array(accepted);
		}
		for (const Vector3 &point : probes_found) {
			bounds.expand_to(point);
		}
		bounds.grow_by(bounds.size.length() * 0.001);
	}

	const bool use_physical_light_units = GLOBAL_GET("rendering/lights_and_shadows/use_physical_light_units");
	const float reference_exposure_normalization = use_physical_light_units ? RS::CAMERA_EXPOSURE_NORMALIZATION_REFERENCE : 1.0f;
	// LightmapGI camera attributes override the scene attributes for baking.
	Ref<CameraAttributes> bake_camera_attributes = camera_attributes;
	if (bake_camera_attributes.is_null()) {
		Ref<World3D> world = get_world_3d();
		if (world.is_valid()) {
			bake_camera_attributes = world->get_camera_attributes();
		}
	}

	float exposure_normalization = reference_exposure_normalization;
	if (bake_camera_attributes.is_valid()) {
		exposure_normalization = bake_camera_attributes->get_exposure_multiplier();
		if (use_physical_light_units) {
			exposure_normalization *= bake_camera_attributes->calculate_exposure_normalization();
		}
	}
	bake_materials_ud.exposure_normalization = exposure_normalization;

	// Add everything to lightmapper
	if (p_bake_step) {
		p_bake_step(0.4, RTR("Preparing Lightmapper"), p_bake_userdata, true);
	}

	{
		Size2i area_light_atlas_size;
		int area_light_atlas_mipmaps = 1;
		HashMap<Ref<Texture2D>, AreaLightAtlasTexture> area_light_atlas_textures;
		PackedByteArray area_light_atlas_data;
		_build_area_light_texture_atlas(lights_found, area_light_atlas_textures, area_light_atlas_size, area_light_atlas_mipmaps);
		if (area_light_atlas_textures.size() > 0) {
			TypedArray<RID> area_light_textures;
			TypedArray<Rect2> area_light_texture_rects;
			for (const KeyValue<Ref<Texture2D>, AreaLightAtlasTexture> &E : area_light_atlas_textures) {
				area_light_textures.push_back(E.key->get_rid());
				area_light_texture_rects.push_back(E.value.texture_rect);
			}
			area_light_atlas_data = RS::get_singleton()->bake_render_area_light_atlas(area_light_textures, area_light_texture_rects, area_light_atlas_size, area_light_atlas_mipmaps);
		} else {
			area_light_atlas_data.resize_initialized(4); // 1 pixel
		}
		lightmapper->add_area_light_atlas(area_light_atlas_size, area_light_atlas_mipmaps, area_light_atlas_data);

		if (!lightmapper->supports_bake_material_func()) {
			for (int i = 0; i < mesh_data.size(); i++) {
				Lightmapper::MeshData &md = mesh_data.write[i];
				Lightmapper::BakeError err = _bake_material(i, md.lightmap_size, md.albedo_on_uv2, md.emission_on_uv2, &bake_materials_ud);
				if (err != Lightmapper::BAKE_OK) {
					return BAKE_ERROR_CANT_CREATE_IMAGE;
				}
			}
		} else {
			lightmapper->set_bake_material_func(_bake_material, &bake_materials_ud);
		}

		for (int i = 0; i < mesh_data.size(); i++) {
			lightmapper->add_mesh(mesh_data[i]);
		}
		// Lightmapper owns the geometry and material images now. Drop this second
		// set of references so sources can be released as each mesh is atlased.
		mesh_data.clear();
		for (int i = 0; i < lights_found.size(); i++) {
			Light3D *light = lights_found[i].light;
			if (light->is_editor_only()) {
				// Don't include editor-only lights in the lightmap bake,
				// as this results in inconsistent visuals when running the project.
				continue;
			}

			Transform3D xf = lights_found[i].xform;

			// For the lightmapper, the indirect energy represents the multiplier for the indirect bounces caused by the light, so the value is not converted when using physical units.
			float indirect_energy = light->get_param(Light3D::PARAM_INDIRECT_ENERGY);
			Color linear_color = light->get_color().srgb_to_linear();
			const RSE::LightType light_type = light->get_light_type();
			const AreaLight3D *area_light = Object::cast_to<AreaLight3D>(light);
			const Vector2 area_size = area_light ? area_light->get_area_size() : Vector2(1.0f, 1.0f);
			float energy = light_energy_to_intensity(light_type, light->get_param(Light3D::PARAM_ENERGY), light->get_param(Light3D::PARAM_INTENSITY), use_physical_light_units, light->get_param(Light3D::PARAM_SPOT_FLUX_SCALE), area_light && area_light->is_area_normalizing_energy(), area_size.x * area_size.y);
			// Realtime materials apply the Lambert BRDF factor. The lightmapper
			// evaluates the cosine directly, so include 1 / PI in its input.
			energy /= Math::PI;
			if (use_physical_light_units) {
				linear_color *= light->get_correlated_color().srgb_to_linear();
			}

			if (Object::cast_to<DirectionalLight3D>(light)) {
				DirectionalLight3D *l = Object::cast_to<DirectionalLight3D>(light);
				if (l->get_sky_mode() != DirectionalLight3D::SKY_MODE_SKY_ONLY) {
					lightmapper->add_directional_light(light->get_name(), light->get_bake_mode() == Light3D::BAKE_STATIC, -xf.basis.get_column(Vector3::AXIS_Z).normalized(), linear_color, energy, indirect_energy, l->get_param(Light3D::PARAM_SIZE), l->get_param(Light3D::PARAM_SHADOW_BLUR));
				}
			} else if (Object::cast_to<OmniLight3D>(light)) {
				OmniLight3D *l = Object::cast_to<OmniLight3D>(light);
				Rect2 projector_rect;
				if (light->get_bake_mode() == Light3D::BAKE_STATIC && light->get_projector().is_valid()) {
					projector_rect = area_light_atlas_textures[light->get_projector()].texture_rect;
				}
				lightmapper->add_omni_light(light->get_name(), light->get_bake_mode() == Light3D::BAKE_STATIC, xf.origin, linear_color, energy, indirect_energy, l->get_param(Light3D::PARAM_RANGE), l->get_param(Light3D::PARAM_ATTENUATION), l->get_param(Light3D::PARAM_RANGE_FADE_START), l->get_param(Light3D::PARAM_SIZE), l->get_param(Light3D::PARAM_SHADOW_BLUR), xf.basis, projector_rect);
			} else if (Object::cast_to<SpotLight3D>(light)) {
				SpotLight3D *l = Object::cast_to<SpotLight3D>(light);
				Rect2 projector_rect;
				if (light->get_bake_mode() == Light3D::BAKE_STATIC && light->get_projector().is_valid()) {
					projector_rect = area_light_atlas_textures[light->get_projector()].texture_rect;
				}
				lightmapper->add_spot_light(light->get_name(), light->get_bake_mode() == Light3D::BAKE_STATIC, xf.origin, -xf.basis.get_column(Vector3::AXIS_Z).normalized(), linear_color, energy, indirect_energy, l->get_param(Light3D::PARAM_RANGE), l->get_param(Light3D::PARAM_ATTENUATION), l->get_param(Light3D::PARAM_RANGE_FADE_START), l->get_param(Light3D::PARAM_SPOT_ANGLE), l->get_param(Light3D::PARAM_SPOT_INNER_ANGLE), l->get_param(Light3D::PARAM_SPOT_ATTENUATION), l->get_param(Light3D::PARAM_SIZE), l->get_param(Light3D::PARAM_SHADOW_BLUR), xf.basis, projector_rect);
			} else if (Object::cast_to<AreaLight3D>(light)) {
				AreaLight3D *l = Object::cast_to<AreaLight3D>(light);
				Vector3 area_vec_x = xf.basis.get_column(Vector3::AXIS_X).normalized() * l->get_area_size().x;
				Vector3 area_vec_y = xf.basis.get_column(Vector3::AXIS_Y).normalized() * l->get_area_size().y;
				AreaLightAtlasTexture tex;
				if (l->get_area_texture().is_valid()) {
					tex = area_light_atlas_textures[l->get_area_texture()];
				}
				lightmapper->add_area_light(light->get_name(), light->get_bake_mode() == Light3D::BAKE_STATIC, xf.origin, -xf.basis.get_column(Vector3::AXIS_Z).normalized(), linear_color, energy, indirect_energy, l->get_param(Light3D::PARAM_RANGE), l->get_param(Light3D::PARAM_ATTENUATION), l->get_param(Light3D::PARAM_RANGE_FADE_START), area_vec_x, area_vec_y, l->get_param(Light3D::PARAM_SIZE), l->get_param(Light3D::PARAM_SHADOW_BLUR), tex.texture_rect, tex.max_mipmap);
			}
		}
		for (int i = 0; i < probes_found.size(); i++) {
			lightmapper->add_probe(probes_found[i]);
		}
	}

	Ref<Image> environment_image;
	Basis environment_transform;

	// Sky luminance at panorama value 1. Custom skies and colors have no intensity of their own, so they use the reference.
	float environment_intensity = 1.0f / reference_exposure_normalization;

	// Add everything to lightmapper
	if (environment_mode != ENVIRONMENT_MODE_DISABLED) {
		if (p_bake_step) {
			p_bake_step(4.1, RTR("Preparing Environment"), p_bake_userdata, true);
		}

		// The bake happens in this node's local space, so rays must be rotated back to world space before looking up the sky.
		// Orthonormalized because the lightmapper assumes a pure rotation.
		environment_transform = get_global_transform().basis.orthonormalized();

		// Bake radiance, not irradiance: the lightmapper integrates the cosine itself and importance samples
		// the sky, so a pre-convolved sky would be blurred twice and lose sun shadows and directionality.
		switch (environment_mode) {
			case ENVIRONMENT_MODE_DISABLED: {
				//nothing
			} break;
			case ENVIRONMENT_MODE_SCENE: {
				Ref<World3D> world = get_world_3d();
				if (world.is_valid()) {
					Ref<Environment> env = world->get_environment();
					if (env.is_null()) {
						env = world->get_fallback_environment();
					}

					if (env.is_valid()) {
						environment_image = RS::get_singleton()->environment_bake_panorama(env->get_rid(), false, Size2i(512, 256));
						if (use_physical_light_units) {
							environment_intensity = env->get_bg_intensity();
						}
						// Same as the renderer: world to sky space is the inverse sky orientation.
						environment_transform = Basis::from_euler(env->get_sky_rotation()).inverse() * environment_transform;
					}
				}
			} break;
			case ENVIRONMENT_MODE_CUSTOM_SKY: {
				if (environment_custom_sky.is_valid()) {
					environment_image = RS::get_singleton()->sky_bake_panorama(environment_custom_sky->get_rid(), environment_custom_energy, false, Size2i(512, 256));
				}

			} break;
			case ENVIRONMENT_MODE_CUSTOM_COLOR: {
				environment_image.instantiate();
				environment_image->initialize_data(128, 64, false, Image::FORMAT_RGBAF);
				Color c = environment_custom_color;
				c.r *= environment_custom_energy;
				c.g *= environment_custom_energy;
				c.b *= environment_custom_energy;
				environment_image->fill(c);

			} break;
		}
	}

	// The panorama has no exposure or intensity applied, while direct and emissive
	// lighting use the selected bake exposure. Apply the environment energy here
	// so it affects every environment source equally.
	const float environment_exposure_multiplier = environment_energy * exposure_normalization * environment_intensity;

	Lightmapper::BakeError bake_err = lightmapper->bake(Lightmapper::BakeQuality(bake_quality), bake_lightmaps && use_denoiser, denoiser_strength, denoiser_range, bounces,
			bounce_indirect_energy, bias, bake_lightmaps && bake_ao, ao_distance, ao_strength, ao_light_affect, ao_samples, max_texture_size, bake_lightmaps && directional, bake_lightmaps && shadowmask_mode != LightmapGIData::SHADOWMASK_MODE_NONE, use_texture_for_bounces,
			environment_image, environment_transform, _lightmap_bake_step_function, &bsud, exposure_normalization, environment_exposure_multiplier, (supersampling_enabled ? supersampling_factor : 1), bake_lightmaps, indirect_sample_clamp);

	if (bake_err == Lightmapper::BAKE_ERROR_TEXTURE_EXCEEDS_MAX_SIZE) {
		return BAKE_ERROR_TEXTURE_SIZE_TOO_SMALL;
	} else if (bake_err == Lightmapper::BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES) {
		return BAKE_ERROR_MESHES_INVALID;
	} else if (bake_err == Lightmapper::BAKE_ERROR_ATLAS_TOO_SMALL) {
		return BAKE_ERROR_ATLAS_TOO_SMALL;
	} else if (bake_err == Lightmapper::BAKE_ERROR_USER_ABORTED) {
		return BAKE_ERROR_USER_ABORTED;
	}

	TypedArray<TextureLayered> lightmap_textures;
	TypedArray<TextureLayered> shadowmask_textures;
	bool save_shadowmask = false;
	if (bake_lightmaps) {
		// POSTBAKE: Save Textures.

		const String texture_filename = p_image_data_path.get_basename();
		const int shadowmask_texture_count = lightmapper->get_shadowmask_texture_count();
		save_shadowmask = shadowmask_mode != LightmapGIData::SHADOWMASK_MODE_NONE && shadowmask_texture_count > 0;

		// Save the lightmap atlases.
		BakeError save_err = _save_and_reimport_atlas_textures(lightmapper, texture_filename, lightmap_textures, false);
		ERR_FAIL_COND_V(save_err != BAKE_ERROR_OK, save_err);

		if (save_shadowmask) {
			// Save the shadowmask atlases.
			save_err = _save_and_reimport_atlas_textures(lightmapper, texture_filename + "_shadow", shadowmask_textures, true);
			ERR_FAIL_COND_V(save_err != BAKE_ERROR_OK, save_err);
		}
	}

	Ref<LightmapGIData> gi_data;
	if (bake_lightmaps) {
		if (previous_lightmap_data.is_valid()) {
			gi_data = previous_lightmap_data;
			previous_lightmap.backup_before_mutation(gi_data);
			set_light_data(Ref<LightmapGIData>());
			gi_data->clear();
		} else {
			gi_data.instantiate();
		}
		gi_data->set_baked_exposure(exposure_normalization);
		gi_data->set_lightmap_bounds(bounds);
	}
	if (bake_lightmaps) {
		gi_data->set_lightmap_textures(lightmap_textures);

		if (save_shadowmask) {
			gi_data->set_shadowmask_textures(shadowmask_textures);
		} else {
			gi_data->clear_shadowmask_textures();
		}

		gi_data->set_uses_spherical_harmonics(directional);
		gi_data->_set_uses_packed_directional(directional); // New SH lightmaps are packed automatically.

		for (int i = 0; i < lightmapper->get_bake_mesh_count(); i++) {
			Dictionary d = lightmapper->get_bake_mesh_userdata(i);
			NodePath np = d["path"];
			int32_t subindex = -1;
			if (d.has("subindex")) {
				subindex = d["subindex"];
			}

			Rect2 uv_scale = lightmapper->get_bake_mesh_uv_scale(i);
			int slice_index = lightmapper->get_bake_mesh_texture_slice(i);
			gi_data->add_user(np, uv_scale, slice_index, subindex);
		}
	}

	Vector<Ref<LightProbeData>> probe_candidates;
	if (bake_probes && !probe_systems.is_empty()) {
		int probe_offset = 0;
		for (int system_index = 0; system_index < probe_systems.size(); system_index++) {
			LightProbeSystem *system = probe_systems[system_index];
			Ref<LightProbeData> candidate;
			const BakeError capture_error = prepare_probe_capture(lightmapper.ptr(), system, probe_offset, system_probe_counts[system_index], get_global_transform(), exposure_normalization, interior, p_bake_step, p_bake_userdata, candidate);
			if (capture_error != BAKE_ERROR_OK) {
				return capture_error;
			}
			probe_candidates.push_back(candidate);
			probe_offset += system_probe_counts[system_index];
		}
	} else {
		for (const Ref<LightProbeData> &old_capture : old_probe_resources) {
			Ref<LightProbeData> candidate;
			candidate.instantiate();
			Dictionary snapshot = old_capture->get_capture_data();
			snapshot["stale"] = old_capture->get_probe_count() > 0;
			ERR_FAIL_COND_V(candidate->set_capture_data(snapshot) != OK, BAKE_ERROR_MESHES_INVALID);
			probe_candidates.push_back(candidate);
		}
	}

	Vector<Ref<Resource>> resources_to_save;
	Vector<String> resource_paths;
	Vector<String> probe_paths;
	if (bake_lightmaps) {
		gi_data->set_path(p_image_data_path, true);
		resources_to_save.push_back(gi_data);
		resource_paths.push_back(p_image_data_path);
	}
	for (int i = 0; i < probe_candidates.size(); i++) {
		const Ref<LightProbeData> old_capture = bake_probes ? probe_systems[i]->get_light_data() : old_probe_resources[i];
		String path = !bake_lightmaps && i == 0 && (p_image_data_path.get_extension() == "tres" || p_image_data_path.get_extension() == "probebake") ? p_image_data_path : (old_capture.is_valid() ? old_capture->get_path() : String());
		if (!path.is_resource_file()) {
			path = p_image_data_path.get_basename() + (i == 0 ? "_probes.tres" : "_probes_" + itos(i) + ".tres");
		}
		if (path.get_extension() == "probebake") {
			path = path.get_basename() + ".tres";
		}
		int suffix = i;
		while (resource_paths.has(path)) {
			path = p_image_data_path.get_basename() + "_probes_" + itos(++suffix) + ".tres";
		}
		probe_paths.push_back(path);
		resources_to_save.push_back(probe_candidates[i]);
		resource_paths.push_back(path);
	}
	ERR_FAIL_COND_V(save_bake_resources(resources_to_save, resource_paths) != OK, BAKE_ERROR_CANT_CREATE_IMAGE);

	if (bake_lightmaps) {
		previous_lightmap.restore = false;
		set_light_data(gi_data);
	}
	Vector<Ref<LightProbeData>> published_captures;
	for (int i = 0; i < probe_candidates.size(); i++) {
		Ref<LightProbeData> capture = bake_probes ? probe_systems[i]->get_light_data() : old_probe_resources[i];
		const bool shared_capture = bake_probes && capture.is_valid() && shared_probe_resources.has(capture->get_instance_id());
		if (capture.is_null() || shared_capture) {
			capture = probe_candidates[i];
		} else {
			if (bake_probes) {
				capture->consume_cache_invalidation();
			}
			capture->set_capture_data(probe_candidates[i]->get_capture_data());
		}
		capture->set_path(probe_paths[i], true);
		if (bake_probes) {
			published_captures.push_back(capture);
		}
	}
	for (int i = 0; i < probe_systems.size(); i++) {
		LightProbeSystem *system = probe_systems[i];
		if (bake_probes && i < published_captures.size()) {
			system->set_capture_transform(system->get_global_transform().affine_inverse() * get_global_transform());
			system->set_light_data(published_captures[i]);
			system->mark_layout_baked();
		}
	}
	update_configuration_warnings();

	return BAKE_ERROR_OK;
}

void LightmapGI::_migrate_legacy_probe_data() {
	if (light_data.is_null() || (light_data->get_capture_points().is_empty() && light_data->get_migrated_probe_data().is_null()) || !is_inside_tree()) {
		return;
	}
	Vector<LightProbeSystem *> systems;
	collect_probe_systems(this, this, systems);
	Ref<LightProbeData> migrated = light_data->get_migrated_probe_data();
	bool transferred = false;
	for (LightProbeSystem *system : systems) {
		if (system->get_light_data().is_null()) {
			if (migrated.is_null()) {
				migrated.instantiate();
				Dictionary snapshot = light_data->get("probe_data");
				snapshot["stale"] = light_data->get_meta(SNAME("_lightmap_probes_stale"), false);
				if (migrated->set_capture_data(snapshot) != OK) {
					return;
				}
				light_data->set_migrated_probe_data(migrated);
			}
			system->set_capture_transform(system->get_global_transform().affine_inverse() * get_global_transform());
			system->set_light_data(migrated);
			system->mark_layout_baked();
			transferred = true;
		}
		transferred = transferred || !light_data->get_capture_points().is_empty();
	}
	if (transferred) {
		if (!light_data->get_capture_points().is_empty()) {
			light_data->set_capture_data(light_data->get_capture_bounds(), light_data->is_interior(), PackedVector3Array(), PackedColorArray(), PackedInt32Array(), PackedInt32Array(), light_data->get_baked_exposure(), 0);
		}
		// Refresh this instance's bounds and capture pairs, including instances sharing legacy data.
		const Ref<LightmapGIData> retained_data = light_data;
		set_light_data(retained_data);
	}
}

void LightmapGI::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_POST_ENTER_TREE: {
			callable_mp(this, &LightmapGI::_migrate_legacy_probe_data).call_deferred();
			if (light_data.is_valid()) {
				ERR_FAIL_COND_MSG(
						light_data->is_using_spherical_harmonics() && !light_data->_is_using_packed_directional(),
						vformat(
								"%s (%s): The directional lightmap textures are stored in a format that isn't supported anymore. Please bake lightmaps again to make lightmaps display from this node again.",
								get_light_data()->get_path(), get_name()));

				if (last_owner && last_owner != get_owner()) {
					light_data->clear_users();
				}

				_assign_lightmaps();
			}
		} break;

		case NOTIFICATION_EXIT_TREE: {
			last_owner = get_owner();

			if (light_data.is_valid()) {
				_clear_lightmaps();
			}
		} break;
	}
}

void LightmapGI::_assign_lightmaps() {
	ERR_FAIL_COND(light_data.is_null());

	Vector<String> missing_node_paths;

	for (int i = 0; i < light_data->get_user_count(); i++) {
		NodePath user_path = light_data->get_user_path(i);
		Node *node = get_node_or_null(user_path);
		if (!node) {
			missing_node_paths.push_back(String(user_path));
			continue;
		}
		int instance_idx = light_data->get_user_sub_instance(i);
		if (instance_idx >= 0) {
			RID instance_id = node->call("get_bake_mesh_instance", instance_idx);
			if (instance_id.is_valid()) {
				RS::get_singleton()->instance_geometry_set_lightmap(instance_id, get_instance(), light_data->get_user_lightmap_uv_scale(i), light_data->get_user_lightmap_slice_index(i));
			}
		} else {
			VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(node);
			ERR_CONTINUE(!vi);
			RS::get_singleton()->instance_geometry_set_lightmap(vi->get_instance(), get_instance(), light_data->get_user_lightmap_uv_scale(i), light_data->get_user_lightmap_slice_index(i));
		}
	}

	if (!missing_node_paths.is_empty()) {
		String missing_paths_text;
		if (missing_node_paths.size() <= 3) {
			missing_paths_text = String(", ").join(missing_node_paths);
		} else {
			missing_paths_text = vformat("%s and %d more", String(", ").join(missing_node_paths.slice(0, 3)), missing_node_paths.size() - 3);
		}
		WARN_PRINT(vformat("%s couldn't find previously baked nodes and needs a rebake (missing nodes: %s).", get_name(), missing_paths_text));
	}
}

void LightmapGI::_clear_lightmaps() {
	ERR_FAIL_COND(light_data.is_null());
	for (int i = 0; i < light_data->get_user_count(); i++) {
		Node *node = get_node_or_null(light_data->get_user_path(i));
		if (!node) {
			continue;
		}
		int instance_idx = light_data->get_user_sub_instance(i);
		if (instance_idx >= 0) {
			RID instance_id = node->call("get_bake_mesh_instance", instance_idx);
			if (instance_id.is_valid()) {
				RS::get_singleton()->instance_geometry_set_lightmap(instance_id, RID(), Rect2(), 0);
			}
		} else {
			VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(node);
			ERR_CONTINUE(!vi);
			RS::get_singleton()->instance_geometry_set_lightmap(vi->get_instance(), RID(), Rect2(), 0);
		}
	}
}

void LightmapGI::set_light_data(const Ref<LightmapGIData> &p_data) {
	if (light_data.is_valid()) {
		if (is_inside_tree()) {
			_clear_lightmaps();
		}
		set_base(RID());
	}
	light_data = p_data;

	if (light_data.is_valid()) {
		set_base(light_data->get_rid());
		if (is_inside_tree()) {
			_assign_lightmaps();
		}
		light_data->update_shadowmask_mode(shadowmask_mode);
		light_data->update_specular_intensity(specular_intensity);
		_update_color_adjustments();
	}

	update_gizmos();
}

Ref<LightmapGIData> LightmapGI::get_light_data() const {
	return light_data;
}

void LightmapGI::set_bake_quality(BakeQuality p_quality) {
	bake_quality = p_quality;
}

LightmapGI::BakeQuality LightmapGI::get_bake_quality() const {
	return bake_quality;
}

AABB LightmapGI::get_aabb() const {
	return AABB();
}

void LightmapGI::set_use_denoiser(bool p_enable) {
	use_denoiser = p_enable;
	notify_property_list_changed();
}

bool LightmapGI::is_using_denoiser() const {
	return use_denoiser;
}

void LightmapGI::set_denoiser_strength(float p_denoiser_strength) {
	denoiser_strength = p_denoiser_strength;
}

float LightmapGI::get_denoiser_strength() const {
	return denoiser_strength;
}

void LightmapGI::set_denoiser_range(int p_denoiser_range) {
	denoiser_range = p_denoiser_range;
}

int LightmapGI::get_denoiser_range() const {
	return denoiser_range;
}

void LightmapGI::set_directional(bool p_enable) {
	directional = p_enable;

	notify_property_list_changed();
}

bool LightmapGI::is_directional() const {
	return directional;
}

void LightmapGI::set_shadowmask_mode(LightmapGIData::ShadowmaskMode p_mode) {
	shadowmask_mode = p_mode;
	if (light_data.is_valid()) {
		light_data->update_shadowmask_mode(p_mode);
	}

	update_configuration_warnings();
}

LightmapGIData::ShadowmaskMode LightmapGI::get_shadowmask_mode() const {
	return shadowmask_mode;
}

void LightmapGI::set_use_texture_for_bounces(bool p_enable) {
	use_texture_for_bounces = p_enable;
}

bool LightmapGI::is_using_texture_for_bounces() const {
	return use_texture_for_bounces;
}

void LightmapGI::set_interior(bool p_enable) {
	interior = p_enable;
}

bool LightmapGI::is_interior() const {
	return interior;
}

void LightmapGI::set_environment_mode(EnvironmentMode p_mode) {
	environment_mode = p_mode;
	notify_property_list_changed();
}

LightmapGI::EnvironmentMode LightmapGI::get_environment_mode() const {
	return environment_mode;
}

void LightmapGI::set_environment_energy(float p_energy) {
	ERR_FAIL_COND(p_energy < 0.0);
	environment_energy = p_energy;
}

float LightmapGI::get_environment_energy() const {
	return environment_energy;
}

void LightmapGI::set_environment_custom_sky(const Ref<Sky> &p_sky) {
	environment_custom_sky = p_sky;
}

Ref<Sky> LightmapGI::get_environment_custom_sky() const {
	return environment_custom_sky;
}

void LightmapGI::set_environment_custom_color(const Color &p_color) {
	environment_custom_color = p_color;
}

Color LightmapGI::get_environment_custom_color() const {
	return environment_custom_color;
}

void LightmapGI::set_environment_custom_energy(float p_energy) {
	environment_custom_energy = p_energy;
}

float LightmapGI::get_environment_custom_energy() const {
	return environment_custom_energy;
}

void LightmapGI::set_bounces(int p_bounces) {
	ERR_FAIL_COND(p_bounces < 0 || p_bounces > 16);
	bounces = p_bounces;
}

int LightmapGI::get_bounces() const {
	return bounces;
}

void LightmapGI::set_bounce_indirect_energy(float p_indirect_energy) {
	ERR_FAIL_COND(p_indirect_energy < 0.0);
	bounce_indirect_energy = p_indirect_energy;
}

float LightmapGI::get_bounce_indirect_energy() const {
	return bounce_indirect_energy;
}

void LightmapGI::set_indirect_sample_clamp(float p_clamp) {
	ERR_FAIL_COND(p_clamp < 0.0);
	indirect_sample_clamp = p_clamp;
}

float LightmapGI::get_indirect_sample_clamp() const {
	return indirect_sample_clamp;
}

void LightmapGI::set_bias(float p_bias) {
	ERR_FAIL_COND(p_bias < 0.00001);
	bias = p_bias;
}

float LightmapGI::get_bias() const {
	return bias;
}

void LightmapGI::set_bake_ao(bool p_enable) {
	bake_ao = p_enable;
	notify_property_list_changed();
}

bool LightmapGI::is_baking_ao() const {
	return bake_ao;
}

void LightmapGI::set_ao_distance(float p_distance) {
	ERR_FAIL_COND(p_distance <= 0.0);
	ao_distance = p_distance;
}

float LightmapGI::get_ao_distance() const {
	return ao_distance;
}

void LightmapGI::set_ao_strength(float p_strength) {
	ERR_FAIL_COND(p_strength < 0.0);
	ao_strength = p_strength;
}

float LightmapGI::get_ao_strength() const {
	return ao_strength;
}

void LightmapGI::set_ao_light_affect(float p_light_affect) {
	ERR_FAIL_COND(p_light_affect < 0.0 || p_light_affect > 1.0);
	ao_light_affect = p_light_affect;
}

float LightmapGI::get_ao_light_affect() const {
	return ao_light_affect;
}

void LightmapGI::set_ao_samples(int p_samples) {
	ERR_FAIL_COND(p_samples < 1 || p_samples > 1024);
	ao_samples = p_samples;
}

int LightmapGI::get_ao_samples() const {
	return ao_samples;
}

void LightmapGI::set_texel_scale(float p_multiplier) {
	ERR_FAIL_COND(p_multiplier < (0.01 - CMP_EPSILON));
	texel_scale = p_multiplier;
}

float LightmapGI::get_texel_scale() const {
	return texel_scale;
}

void LightmapGI::set_max_texture_size(int p_size) {
	ERR_FAIL_COND_MSG(p_size < 2048, vformat("The LightmapGI maximum texture size supplied (%d) is too small. The minimum allowed value is 2048.", p_size));
	ERR_FAIL_COND_MSG(p_size > 16384, vformat("The LightmapGI maximum texture size supplied (%d) is too large. The maximum allowed value is 16384.", p_size));
	max_texture_size = p_size;
}

int LightmapGI::get_max_texture_size() const {
	return max_texture_size;
}

void LightmapGI::set_supersampling_enabled(bool p_enable) {
	supersampling_enabled = p_enable;

	notify_property_list_changed();
}

bool LightmapGI::is_supersampling_enabled() const {
	return supersampling_enabled;
}

void LightmapGI::set_supersampling_factor(float p_factor) {
	ERR_FAIL_COND(p_factor < 1);

	supersampling_factor = p_factor;
}

float LightmapGI::get_supersampling_factor() const {
	return supersampling_factor;
}

void LightmapGI::set_camera_attributes(const Ref<CameraAttributes> &p_camera_attributes) {
	camera_attributes = p_camera_attributes;
}

Ref<CameraAttributes> LightmapGI::get_camera_attributes() const {
	return camera_attributes;
}

void LightmapGI::set_shadows_black_point(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value));
	adjustments.tonal_black_point[0] = CLAMP(p_value, 0.0f, adjustments.tonal_white_point[0] - 0.001f);
	_update_color_adjustments();
}

float LightmapGI::get_shadows_black_point() const {
	return adjustments.tonal_black_point[0];
}

void LightmapGI::set_shadows_white_point(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value));
	adjustments.tonal_white_point[0] = CLAMP(p_value, adjustments.tonal_black_point[0] + 0.001f, 1.0f);
	_update_color_adjustments();
}

float LightmapGI::get_shadows_white_point() const {
	return adjustments.tonal_white_point[0];
}

void LightmapGI::set_shadows_gamma(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value));
	adjustments.tonal_gamma[0] = CLAMP(p_value, 0.05f, 4.0f);
	_update_color_adjustments();
}

float LightmapGI::get_shadows_gamma() const {
	return adjustments.tonal_gamma[0];
}

void LightmapGI::set_shadows_saturation(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value) || p_value < 0.0f || p_value > 4.0f);
	if (adjustments.tonal_saturation[0] == p_value) {
		return;
	}
	adjustments.tonal_saturation[0] = p_value;
	_update_color_adjustments();
}

float LightmapGI::get_shadows_saturation() const {
	return adjustments.tonal_saturation[0];
}

void LightmapGI::set_shadows_contrast(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value) || p_value < 0.0f || p_value > 4.0f);
	if (adjustments.tonal_contrast[0] == p_value) {
		return;
	}
	adjustments.tonal_contrast[0] = p_value;
	_update_color_adjustments();
}

float LightmapGI::get_shadows_contrast() const {
	return adjustments.tonal_contrast[0];
}

void LightmapGI::set_midtones_black_point(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value));
	adjustments.tonal_black_point[1] = CLAMP(p_value, 0.0f, adjustments.tonal_white_point[1] - 0.001f);
	_update_color_adjustments();
}

float LightmapGI::get_midtones_black_point() const {
	return adjustments.tonal_black_point[1];
}

void LightmapGI::set_midtones_white_point(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value));
	adjustments.tonal_white_point[1] = CLAMP(p_value, adjustments.tonal_black_point[1] + 0.001f, 1.0f);
	_update_color_adjustments();
}

float LightmapGI::get_midtones_white_point() const {
	return adjustments.tonal_white_point[1];
}

void LightmapGI::set_midtones_gamma(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value));
	adjustments.tonal_gamma[1] = CLAMP(p_value, 0.05f, 4.0f);
	_update_color_adjustments();
}

float LightmapGI::get_midtones_gamma() const {
	return adjustments.tonal_gamma[1];
}

void LightmapGI::set_midtones_saturation(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value) || p_value < 0.0f || p_value > 4.0f);
	if (adjustments.tonal_saturation[1] == p_value) {
		return;
	}
	adjustments.tonal_saturation[1] = p_value;
	_update_color_adjustments();
}

float LightmapGI::get_midtones_saturation() const {
	return adjustments.tonal_saturation[1];
}

void LightmapGI::set_midtones_contrast(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value) || p_value < 0.0f || p_value > 4.0f);
	if (adjustments.tonal_contrast[1] == p_value) {
		return;
	}
	adjustments.tonal_contrast[1] = p_value;
	_update_color_adjustments();
}

float LightmapGI::get_midtones_contrast() const {
	return adjustments.tonal_contrast[1];
}

void LightmapGI::set_highlights_black_point(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value));
	adjustments.tonal_black_point[2] = CLAMP(p_value, 0.0f, adjustments.tonal_white_point[2] - 0.001f);
	_update_color_adjustments();
}

float LightmapGI::get_highlights_black_point() const {
	return adjustments.tonal_black_point[2];
}

void LightmapGI::set_highlights_white_point(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value));
	adjustments.tonal_white_point[2] = CLAMP(p_value, adjustments.tonal_black_point[2] + 0.001f, 1.0f);
	_update_color_adjustments();
}

float LightmapGI::get_highlights_white_point() const {
	return adjustments.tonal_white_point[2];
}

void LightmapGI::set_highlights_gamma(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value));
	adjustments.tonal_gamma[2] = CLAMP(p_value, 0.05f, 4.0f);
	_update_color_adjustments();
}

float LightmapGI::get_highlights_gamma() const {
	return adjustments.tonal_gamma[2];
}

void LightmapGI::set_highlights_saturation(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value) || p_value < 0.0f || p_value > 4.0f);
	if (adjustments.tonal_saturation[2] == p_value) {
		return;
	}
	adjustments.tonal_saturation[2] = p_value;
	_update_color_adjustments();
}

float LightmapGI::get_highlights_saturation() const {
	return adjustments.tonal_saturation[2];
}

void LightmapGI::set_highlights_contrast(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value) || p_value < 0.0f || p_value > 4.0f);
	if (adjustments.tonal_contrast[2] == p_value) {
		return;
	}
	adjustments.tonal_contrast[2] = p_value;
	_update_color_adjustments();
}

float LightmapGI::get_highlights_contrast() const {
	return adjustments.tonal_contrast[2];
}

void LightmapGI::_set_tonal_range(int p_index, float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value));
	const Color previous = adjustments.tonal_ranges;
	const float minimum[4] = { 0.0f, 0.0001f, 0.0001f, 0.0002f };
	const float maximum[4] = { 0.9998f, 0.9999f, 0.9999f, 1.0f };
	const float gap[3] = { 0.0001f, 0.0f, 0.0001f };
	adjustments.tonal_ranges[p_index] = CLAMP(p_value, minimum[p_index], maximum[p_index]);
	// Move adjacent boundaries when necessary, keeping both transitions ordered.
	// Independent setter limits also allow valid ranges to round-trip during scene loading.
	for (int i = p_index - 1; i >= 0; i--) {
		adjustments.tonal_ranges[i] = MIN(adjustments.tonal_ranges[i], adjustments.tonal_ranges[i + 1] - gap[i]);
	}
	for (int i = p_index + 1; i < 4; i++) {
		adjustments.tonal_ranges[i] = MAX(adjustments.tonal_ranges[i], adjustments.tonal_ranges[i - 1] + gap[i - 1]);
	}
	if (previous == adjustments.tonal_ranges) {
		return;
	}
	_update_color_adjustments();
}

void LightmapGI::set_tonal_midtones_start(float p_value) {
	_set_tonal_range(0, p_value);
}

float LightmapGI::get_tonal_midtones_start() const {
	return adjustments.tonal_ranges[0];
}

void LightmapGI::set_tonal_shadows_end(float p_value) {
	_set_tonal_range(1, p_value);
}

float LightmapGI::get_tonal_shadows_end() const {
	return adjustments.tonal_ranges[1];
}

void LightmapGI::set_tonal_highlights_start(float p_value) {
	_set_tonal_range(2, p_value);
}

float LightmapGI::get_tonal_highlights_start() const {
	return adjustments.tonal_ranges[2];
}

void LightmapGI::set_tonal_midtones_end(float p_value) {
	_set_tonal_range(3, p_value);
}

float LightmapGI::get_tonal_midtones_end() const {
	return adjustments.tonal_ranges[3];
}

void LightmapGI::set_shadows_tint(const Color &p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value.r) || !Math::is_finite(p_value.g) || !Math::is_finite(p_value.b) || p_value.r < 0.0f || p_value.g < 0.0f || p_value.b < 0.0f);
	if (adjustments.shadows_tint == p_value) {
		return;
	}
	adjustments.shadows_tint = p_value;
	_update_color_adjustments();
}

Color LightmapGI::get_shadows_tint() const {
	return adjustments.shadows_tint;
}

void LightmapGI::set_midtones_tint(const Color &p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value.r) || !Math::is_finite(p_value.g) || !Math::is_finite(p_value.b) || p_value.r < 0.0f || p_value.g < 0.0f || p_value.b < 0.0f);
	if (adjustments.midtones_tint == p_value) {
		return;
	}
	adjustments.midtones_tint = p_value;
	_update_color_adjustments();
}

Color LightmapGI::get_midtones_tint() const {
	return adjustments.midtones_tint;
}

void LightmapGI::set_highlights_tint(const Color &p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value.r) || !Math::is_finite(p_value.g) || !Math::is_finite(p_value.b) || p_value.r < 0.0f || p_value.g < 0.0f || p_value.b < 0.0f);
	if (adjustments.highlights_tint == p_value) {
		return;
	}
	adjustments.highlights_tint = p_value;
	_update_color_adjustments();
}

Color LightmapGI::get_highlights_tint() const {
	return adjustments.highlights_tint;
}

void LightmapGI::set_debug_mode(DebugMode p_value) {
	ERR_FAIL_COND(p_value < DEBUG_DISABLED || p_value > DEBUG_HIGHLIGHTS);
	if (adjustments.debug_mode == p_value) {
		return;
	}
	adjustments.debug_mode = p_value;
	_update_color_adjustments();
}

LightmapGI::DebugMode LightmapGI::get_debug_mode() const {
	return (DebugMode)adjustments.debug_mode;
}

void LightmapGI::_update_color_adjustments() {
	if (light_data.is_null()) {
		return;
	}
	const RID lightmap = light_data->get_rid();
	RS::get_singleton()->lightmap_set_adjustments(lightmap, adjustments.energy, adjustments.tint, adjustments.saturation, adjustments.contrast, adjustments.shadows, adjustments.midtones, adjustments.highlights, 1.0f);
	RS::get_singleton()->lightmap_set_tonal_settings(lightmap, adjustments.tonal_ranges, adjustments.shadows_tint, adjustments.midtones_tint, adjustments.highlights_tint, adjustments.tonal_saturation, adjustments.tonal_contrast, adjustments.debug_mode);
	RS::get_singleton()->lightmap_set_tonal_levels(lightmap, adjustments.tonal_black_point, adjustments.tonal_white_point, adjustments.tonal_gamma);
	RS::get_singleton()->lightmap_set_adjustments_enabled(lightmap, adjustments.enabled);
}

void LightmapGI::set_adjustments_enabled(bool p_enabled) {
	if (adjustments.enabled == p_enabled) {
		return;
	}
	adjustments.enabled = p_enabled;
	_update_color_adjustments();
}

bool LightmapGI::is_adjustments_enabled() const {
	return adjustments.enabled;
}

void LightmapGI::set_energy(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value));
	ERR_FAIL_COND(p_value < 0.0f);
	if (adjustments.energy == p_value) {
		return;
	}
	adjustments.energy = p_value;
	_update_color_adjustments();
}

float LightmapGI::get_energy() const {
	return adjustments.energy;
}

void LightmapGI::set_tint(const Color &p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value.r) || !Math::is_finite(p_value.g) || !Math::is_finite(p_value.b) || !Math::is_finite(p_value.a) || p_value.r < 0.0f || p_value.g < 0.0f || p_value.b < 0.0f);
	if (adjustments.tint == p_value) {
		return;
	}
	adjustments.tint = p_value;
	_update_color_adjustments();
}

Color LightmapGI::get_tint() const {
	return adjustments.tint;
}

void LightmapGI::set_saturation(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value));
	ERR_FAIL_COND(p_value < 0.0f);
	if (adjustments.saturation == p_value) {
		return;
	}
	adjustments.saturation = p_value;
	_update_color_adjustments();
}

float LightmapGI::get_saturation() const {
	return adjustments.saturation;
}

void LightmapGI::set_contrast(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value));
	ERR_FAIL_COND(p_value < 0.0f || p_value > 4.0f);
	if (adjustments.contrast == p_value) {
		return;
	}
	adjustments.contrast = p_value;
	_update_color_adjustments();
}

float LightmapGI::get_contrast() const {
	return adjustments.contrast;
}

void LightmapGI::set_shadows(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value));
	ERR_FAIL_COND(p_value < -4.0f || p_value > 4.0f);
	if (adjustments.shadows == p_value) {
		return;
	}
	adjustments.shadows = p_value;
	_update_color_adjustments();
}

float LightmapGI::get_shadows() const {
	return adjustments.shadows;
}

void LightmapGI::set_midtones(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value));
	ERR_FAIL_COND(p_value < -4.0f || p_value > 4.0f);
	if (adjustments.midtones == p_value) {
		return;
	}
	adjustments.midtones = p_value;
	_update_color_adjustments();
}

float LightmapGI::get_midtones() const {
	return adjustments.midtones;
}

void LightmapGI::set_highlights(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value));
	ERR_FAIL_COND(p_value < -4.0f || p_value > 4.0f);
	if (adjustments.highlights == p_value) {
		return;
	}
	adjustments.highlights = p_value;
	_update_color_adjustments();
}

float LightmapGI::get_highlights() const {
	return adjustments.highlights;
}

void LightmapGI::set_directionality_strength(float p_value) {
	ERR_FAIL_COND(!Math::is_finite(p_value));
	ERR_FAIL_COND(p_value < 0.0f || p_value > 4.0f);
	if (adjustments.directionality_strength == p_value) {
		return;
	}
	adjustments.directionality_strength = p_value;
}

float LightmapGI::get_directionality_strength() const {
	return adjustments.directionality_strength;
}

float LightmapGI::get_specular_intensity() const {
	return specular_intensity;
}

void LightmapGI::set_specular_intensity(float p_strength) {
	specular_intensity = p_strength;

	if (light_data.is_valid()) {
		light_data->update_specular_intensity(p_strength);
	}
}

PackedStringArray LightmapGI::get_configuration_warnings() const {
	PackedStringArray warnings = VisualInstance3D::get_configuration_warnings();

#ifdef MODULE_LIGHTMAPPER_RD_ENABLED
	if (!DisplayServer::get_singleton()->can_create_rendering_device()) {
		warnings.push_back(vformat(RTR("Lightmaps can only be baked from a GPU that supports the RenderingDevice backends.\nYour GPU (%s) does not support RenderingDevice, as it does not support Vulkan, Direct3D 12, or Metal.\nLightmap baking will not be available on this device, although rendering existing baked lightmaps will work."), RenderingServer::get_singleton()->get_video_adapter_name()));
		return warnings;
	}

	if (shadowmask_mode != LightmapGIData::SHADOWMASK_MODE_NONE && light_data.is_valid() && !light_data->has_shadowmask_textures()) {
		warnings.push_back(RTR("The lightmap has no baked shadowmask textures. Please rebake with the Shadowmask Mode set to anything other than None."));
	}

#elif defined(ANDROID_ENABLED) || defined(APPLE_EMBEDDED_ENABLED)
	warnings.push_back(vformat(RTR("Lightmaps cannot be baked on %s. Rendering existing baked lightmaps will still work."), OS::get_singleton()->get_name()));
#else
	warnings.push_back(RTR("Lightmaps cannot be baked, as the `lightmapper_rd` module was disabled at compile-time. Rendering existing baked lightmaps will still work."));
#endif

	return warnings;
}

bool LightmapGI::_set(const StringName &p_name, const Variant &p_value) {
	// Accept removed bake options in old scene files without persisting them again.
	return p_name == SNAME("generate_probes_subdiv") || p_name == SNAME("probe_ray_count");
}

void LightmapGI::_validate_property(PropertyInfo &p_property) const {
	if (!Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	if (p_property.name == "supersampling_factor") {
		if (!supersampling_enabled) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	} else if (p_property.name == "environment_energy") {
		if (environment_mode == ENVIRONMENT_MODE_DISABLED) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	} else if (p_property.name == "environment_custom_sky") {
		if (environment_mode != ENVIRONMENT_MODE_CUSTOM_SKY) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	} else if (p_property.name == "environment_custom_color") {
		if (environment_mode != ENVIRONMENT_MODE_CUSTOM_COLOR) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	} else if (p_property.name == "environment_custom_energy") {
		if (environment_mode != ENVIRONMENT_MODE_CUSTOM_COLOR && environment_mode != ENVIRONMENT_MODE_CUSTOM_SKY) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	} else if (p_property.name == "denoiser_strength") {
		if (!use_denoiser) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	} else if (p_property.name == "denoiser_range") {
		if (!use_denoiser) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	} else if (p_property.name == "ao_distance" || p_property.name == "ao_strength" || p_property.name == "ao_light_affect" || p_property.name == "ao_samples") {
		if (!bake_ao) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	}
	if (p_property.name == "specular_intensity") {
		if (!directional) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	}
}

void LightmapGI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_light_data", "data"), &LightmapGI::set_light_data);
	ClassDB::bind_method(D_METHOD("get_light_data"), &LightmapGI::get_light_data);

	ClassDB::bind_method(D_METHOD("set_bake_quality", "bake_quality"), &LightmapGI::set_bake_quality);
	ClassDB::bind_method(D_METHOD("get_bake_quality"), &LightmapGI::get_bake_quality);

	ClassDB::bind_method(D_METHOD("set_bounces", "bounces"), &LightmapGI::set_bounces);
	ClassDB::bind_method(D_METHOD("get_bounces"), &LightmapGI::get_bounces);

	ClassDB::bind_method(D_METHOD("set_bounce_indirect_energy", "bounce_indirect_energy"), &LightmapGI::set_bounce_indirect_energy);
	ClassDB::bind_method(D_METHOD("get_bounce_indirect_energy"), &LightmapGI::get_bounce_indirect_energy);

	ClassDB::bind_method(D_METHOD("set_indirect_sample_clamp", "indirect_sample_clamp"), &LightmapGI::set_indirect_sample_clamp);
	ClassDB::bind_method(D_METHOD("get_indirect_sample_clamp"), &LightmapGI::get_indirect_sample_clamp);

	ClassDB::bind_method(D_METHOD("set_bias", "bias"), &LightmapGI::set_bias);
	ClassDB::bind_method(D_METHOD("get_bias"), &LightmapGI::get_bias);
	ClassDB::bind_method(D_METHOD("set_bake_ao", "enable"), &LightmapGI::set_bake_ao);
	ClassDB::bind_method(D_METHOD("is_baking_ao"), &LightmapGI::is_baking_ao);
	ClassDB::bind_method(D_METHOD("set_ao_distance", "distance"), &LightmapGI::set_ao_distance);
	ClassDB::bind_method(D_METHOD("get_ao_distance"), &LightmapGI::get_ao_distance);
	ClassDB::bind_method(D_METHOD("set_ao_strength", "strength"), &LightmapGI::set_ao_strength);
	ClassDB::bind_method(D_METHOD("get_ao_strength"), &LightmapGI::get_ao_strength);
	ClassDB::bind_method(D_METHOD("set_ao_light_affect", "light_affect"), &LightmapGI::set_ao_light_affect);
	ClassDB::bind_method(D_METHOD("get_ao_light_affect"), &LightmapGI::get_ao_light_affect);
	ClassDB::bind_method(D_METHOD("set_ao_samples", "samples"), &LightmapGI::set_ao_samples);
	ClassDB::bind_method(D_METHOD("get_ao_samples"), &LightmapGI::get_ao_samples);

	ClassDB::bind_method(D_METHOD("set_environment_mode", "mode"), &LightmapGI::set_environment_mode);
	ClassDB::bind_method(D_METHOD("get_environment_mode"), &LightmapGI::get_environment_mode);

	ClassDB::bind_method(D_METHOD("set_environment_energy", "energy"), &LightmapGI::set_environment_energy);
	ClassDB::bind_method(D_METHOD("get_environment_energy"), &LightmapGI::get_environment_energy);

	ClassDB::bind_method(D_METHOD("set_environment_custom_sky", "sky"), &LightmapGI::set_environment_custom_sky);
	ClassDB::bind_method(D_METHOD("get_environment_custom_sky"), &LightmapGI::get_environment_custom_sky);

	ClassDB::bind_method(D_METHOD("set_environment_custom_color", "color"), &LightmapGI::set_environment_custom_color);
	ClassDB::bind_method(D_METHOD("get_environment_custom_color"), &LightmapGI::get_environment_custom_color);

	ClassDB::bind_method(D_METHOD("set_environment_custom_energy", "energy"), &LightmapGI::set_environment_custom_energy);
	ClassDB::bind_method(D_METHOD("get_environment_custom_energy"), &LightmapGI::get_environment_custom_energy);

	ClassDB::bind_method(D_METHOD("set_texel_scale", "texel_scale"), &LightmapGI::set_texel_scale);
	ClassDB::bind_method(D_METHOD("get_texel_scale"), &LightmapGI::get_texel_scale);

	ClassDB::bind_method(D_METHOD("set_max_texture_size", "max_texture_size"), &LightmapGI::set_max_texture_size);
	ClassDB::bind_method(D_METHOD("get_max_texture_size"), &LightmapGI::get_max_texture_size);

	ClassDB::bind_method(D_METHOD("set_supersampling_enabled", "enable"), &LightmapGI::set_supersampling_enabled);
	ClassDB::bind_method(D_METHOD("is_supersampling_enabled"), &LightmapGI::is_supersampling_enabled);

	ClassDB::bind_method(D_METHOD("set_supersampling_factor", "factor"), &LightmapGI::set_supersampling_factor);
	ClassDB::bind_method(D_METHOD("get_supersampling_factor"), &LightmapGI::get_supersampling_factor);

	ClassDB::bind_method(D_METHOD("set_use_denoiser", "use_denoiser"), &LightmapGI::set_use_denoiser);
	ClassDB::bind_method(D_METHOD("is_using_denoiser"), &LightmapGI::is_using_denoiser);

	ClassDB::bind_method(D_METHOD("set_denoiser_strength", "denoiser_strength"), &LightmapGI::set_denoiser_strength);
	ClassDB::bind_method(D_METHOD("get_denoiser_strength"), &LightmapGI::get_denoiser_strength);

	ClassDB::bind_method(D_METHOD("set_denoiser_range", "denoiser_range"), &LightmapGI::set_denoiser_range);
	ClassDB::bind_method(D_METHOD("get_denoiser_range"), &LightmapGI::get_denoiser_range);

	ClassDB::bind_method(D_METHOD("set_interior", "enable"), &LightmapGI::set_interior);
	ClassDB::bind_method(D_METHOD("is_interior"), &LightmapGI::is_interior);

	ClassDB::bind_method(D_METHOD("set_directional", "directional"), &LightmapGI::set_directional);
	ClassDB::bind_method(D_METHOD("is_directional"), &LightmapGI::is_directional);

	ClassDB::bind_method(D_METHOD("set_shadowmask_mode", "mode"), &LightmapGI::set_shadowmask_mode);
	ClassDB::bind_method(D_METHOD("get_shadowmask_mode"), &LightmapGI::get_shadowmask_mode);

	ClassDB::bind_method(D_METHOD("set_use_texture_for_bounces", "use_texture_for_bounces"), &LightmapGI::set_use_texture_for_bounces);
	ClassDB::bind_method(D_METHOD("is_using_texture_for_bounces"), &LightmapGI::is_using_texture_for_bounces);

	ClassDB::bind_method(D_METHOD("set_camera_attributes", "camera_attributes"), &LightmapGI::set_camera_attributes);
	ClassDB::bind_method(D_METHOD("get_camera_attributes"), &LightmapGI::get_camera_attributes);

	ClassDB::bind_method(D_METHOD("set_specular_intensity", "specular_intensity"), &LightmapGI::set_specular_intensity);
	ClassDB::bind_method(D_METHOD("get_specular_intensity"), &LightmapGI::get_specular_intensity);

	//	ClassDB::bind_method(D_METHOD("bake", "from_node"), &LightmapGI::bake, DEFVAL(Variant()));

	ClassDB::bind_method(D_METHOD("set_energy", "value"), &LightmapGI::set_energy);
	ClassDB::bind_method(D_METHOD("get_energy"), &LightmapGI::get_energy);
	ClassDB::bind_method(D_METHOD("set_tint", "value"), &LightmapGI::set_tint);
	ClassDB::bind_method(D_METHOD("get_tint"), &LightmapGI::get_tint);
	ClassDB::bind_method(D_METHOD("set_saturation", "value"), &LightmapGI::set_saturation);
	ClassDB::bind_method(D_METHOD("get_saturation"), &LightmapGI::get_saturation);
	ClassDB::bind_method(D_METHOD("set_contrast", "value"), &LightmapGI::set_contrast);
	ClassDB::bind_method(D_METHOD("get_contrast"), &LightmapGI::get_contrast);
	ClassDB::bind_method(D_METHOD("set_shadows", "value"), &LightmapGI::set_shadows);
	ClassDB::bind_method(D_METHOD("get_shadows"), &LightmapGI::get_shadows);
	ClassDB::bind_method(D_METHOD("set_midtones", "value"), &LightmapGI::set_midtones);
	ClassDB::bind_method(D_METHOD("get_midtones"), &LightmapGI::get_midtones);
	ClassDB::bind_method(D_METHOD("set_highlights", "value"), &LightmapGI::set_highlights);
	ClassDB::bind_method(D_METHOD("get_highlights"), &LightmapGI::get_highlights);
	ClassDB::bind_method(D_METHOD("set_directionality_strength", "value"), &LightmapGI::set_directionality_strength);
	ClassDB::bind_method(D_METHOD("get_directionality_strength"), &LightmapGI::get_directionality_strength);

	ClassDB::bind_method(D_METHOD("set_shadows_tint", "value"), &LightmapGI::set_shadows_tint);
	ClassDB::bind_method(D_METHOD("get_shadows_tint"), &LightmapGI::get_shadows_tint);
	ClassDB::bind_method(D_METHOD("set_midtones_tint", "value"), &LightmapGI::set_midtones_tint);
	ClassDB::bind_method(D_METHOD("get_midtones_tint"), &LightmapGI::get_midtones_tint);
	ClassDB::bind_method(D_METHOD("set_highlights_tint", "value"), &LightmapGI::set_highlights_tint);
	ClassDB::bind_method(D_METHOD("get_highlights_tint"), &LightmapGI::get_highlights_tint);
	ClassDB::bind_method(D_METHOD("set_debug_mode", "value"), &LightmapGI::set_debug_mode);
	ClassDB::bind_method(D_METHOD("get_debug_mode"), &LightmapGI::get_debug_mode);
	BIND_ENUM_CONSTANT(DEBUG_DISABLED);
	BIND_ENUM_CONSTANT(DEBUG_SHADOWS);
	BIND_ENUM_CONSTANT(DEBUG_MIDDLETONES);
	BIND_ENUM_CONSTANT(DEBUG_HIGHLIGHTS);

	ClassDB::bind_method(D_METHOD("set_shadows_saturation", "value"), &LightmapGI::set_shadows_saturation);
	ClassDB::bind_method(D_METHOD("set_shadows_gamma", "value"), &LightmapGI::set_shadows_gamma);
	ClassDB::bind_method(D_METHOD("get_shadows_gamma"), &LightmapGI::get_shadows_gamma);
	ClassDB::bind_method(D_METHOD("set_shadows_white_point", "value"), &LightmapGI::set_shadows_white_point);
	ClassDB::bind_method(D_METHOD("get_shadows_white_point"), &LightmapGI::get_shadows_white_point);
	ClassDB::bind_method(D_METHOD("set_shadows_black_point", "value"), &LightmapGI::set_shadows_black_point);
	ClassDB::bind_method(D_METHOD("get_shadows_black_point"), &LightmapGI::get_shadows_black_point);
	ClassDB::bind_method(D_METHOD("get_shadows_saturation"), &LightmapGI::get_shadows_saturation);
	ClassDB::bind_method(D_METHOD("set_shadows_contrast", "value"), &LightmapGI::set_shadows_contrast);
	ClassDB::bind_method(D_METHOD("get_shadows_contrast"), &LightmapGI::get_shadows_contrast);
	ClassDB::bind_method(D_METHOD("set_midtones_saturation", "value"), &LightmapGI::set_midtones_saturation);
	ClassDB::bind_method(D_METHOD("set_midtones_gamma", "value"), &LightmapGI::set_midtones_gamma);
	ClassDB::bind_method(D_METHOD("get_midtones_gamma"), &LightmapGI::get_midtones_gamma);
	ClassDB::bind_method(D_METHOD("set_midtones_white_point", "value"), &LightmapGI::set_midtones_white_point);
	ClassDB::bind_method(D_METHOD("get_midtones_white_point"), &LightmapGI::get_midtones_white_point);
	ClassDB::bind_method(D_METHOD("set_midtones_black_point", "value"), &LightmapGI::set_midtones_black_point);
	ClassDB::bind_method(D_METHOD("get_midtones_black_point"), &LightmapGI::get_midtones_black_point);
	ClassDB::bind_method(D_METHOD("get_midtones_saturation"), &LightmapGI::get_midtones_saturation);
	ClassDB::bind_method(D_METHOD("set_midtones_contrast", "value"), &LightmapGI::set_midtones_contrast);
	ClassDB::bind_method(D_METHOD("get_midtones_contrast"), &LightmapGI::get_midtones_contrast);
	ClassDB::bind_method(D_METHOD("set_highlights_saturation", "value"), &LightmapGI::set_highlights_saturation);
	ClassDB::bind_method(D_METHOD("set_highlights_gamma", "value"), &LightmapGI::set_highlights_gamma);
	ClassDB::bind_method(D_METHOD("get_highlights_gamma"), &LightmapGI::get_highlights_gamma);
	ClassDB::bind_method(D_METHOD("set_highlights_white_point", "value"), &LightmapGI::set_highlights_white_point);
	ClassDB::bind_method(D_METHOD("get_highlights_white_point"), &LightmapGI::get_highlights_white_point);
	ClassDB::bind_method(D_METHOD("set_highlights_black_point", "value"), &LightmapGI::set_highlights_black_point);
	ClassDB::bind_method(D_METHOD("get_highlights_black_point"), &LightmapGI::get_highlights_black_point);
	ClassDB::bind_method(D_METHOD("get_highlights_saturation"), &LightmapGI::get_highlights_saturation);
	ClassDB::bind_method(D_METHOD("set_highlights_contrast", "value"), &LightmapGI::set_highlights_contrast);
	ClassDB::bind_method(D_METHOD("get_highlights_contrast"), &LightmapGI::get_highlights_contrast);

	ClassDB::bind_method(D_METHOD("set_tonal_midtones_start", "value"), &LightmapGI::set_tonal_midtones_start);
	ClassDB::bind_method(D_METHOD("get_tonal_midtones_start"), &LightmapGI::get_tonal_midtones_start);
	ClassDB::bind_method(D_METHOD("set_tonal_shadows_end", "value"), &LightmapGI::set_tonal_shadows_end);
	ClassDB::bind_method(D_METHOD("get_tonal_shadows_end"), &LightmapGI::get_tonal_shadows_end);
	ClassDB::bind_method(D_METHOD("set_tonal_highlights_start", "value"), &LightmapGI::set_tonal_highlights_start);
	ClassDB::bind_method(D_METHOD("get_tonal_highlights_start"), &LightmapGI::get_tonal_highlights_start);
	ClassDB::bind_method(D_METHOD("set_tonal_midtones_end", "value"), &LightmapGI::set_tonal_midtones_end);
	ClassDB::bind_method(D_METHOD("get_tonal_midtones_end"), &LightmapGI::get_tonal_midtones_end);

	ClassDB::bind_method(D_METHOD("set_adjustments_enabled", "enabled"), &LightmapGI::set_adjustments_enabled);
	ClassDB::bind_method(D_METHOD("is_adjustments_enabled"), &LightmapGI::is_adjustments_enabled);

	ADD_GROUP("Adjustments", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "adjustments_enabled"), "set_adjustments_enabled", "is_adjustments_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "energy", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_energy", "get_energy");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "tint", PROPERTY_HINT_COLOR_NO_ALPHA), "set_tint", "get_tint");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "saturation", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_saturation", "get_saturation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "contrast", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_contrast", "get_contrast");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "directionality_strength", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_directionality_strength", "get_directionality_strength");

	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_mode", PROPERTY_HINT_ENUM, "Disabled,Shadows,Middletones,Highlights"), "set_debug_mode", "get_debug_mode");

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shadows", PROPERTY_HINT_RANGE, "-4,4,0.01,suffix:EV", PROPERTY_USAGE_STORAGE), "set_shadows", "get_shadows");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "midtones", PROPERTY_HINT_RANGE, "-4,4,0.01,suffix:EV", PROPERTY_USAGE_STORAGE), "set_midtones", "get_midtones");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "highlights", PROPERTY_HINT_RANGE, "-4,4,0.01,suffix:EV", PROPERTY_USAGE_STORAGE), "set_highlights", "get_highlights");
	ADD_SUBGROUP("Tonal Ranges", "tonal_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tonal_midtones_start", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_tonal_midtones_start", "get_tonal_midtones_start");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tonal_shadows_end", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_tonal_shadows_end", "get_tonal_shadows_end");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tonal_highlights_start", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_tonal_highlights_start", "get_tonal_highlights_start");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tonal_midtones_end", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_tonal_midtones_end", "get_tonal_midtones_end");
	ADD_SUBGROUP("Shadows", "shadows_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shadows_exposure", PROPERTY_HINT_RANGE, "-4,4,0.01,suffix:EV", PROPERTY_USAGE_EDITOR), "set_shadows", "get_shadows");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "shadows_tint", PROPERTY_HINT_COLOR_NO_ALPHA), "set_shadows_tint", "get_shadows_tint");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shadows_saturation", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_shadows_saturation", "get_shadows_saturation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shadows_contrast", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_shadows_contrast", "get_shadows_contrast");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shadows_black_point", PROPERTY_HINT_RANGE, "0,0.1,0.0001,or_greater"), "set_shadows_black_point", "get_shadows_black_point");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shadows_white_point", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_shadows_white_point", "get_shadows_white_point");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shadows_gamma", PROPERTY_HINT_RANGE, "0.05,4,0.01"), "set_shadows_gamma", "get_shadows_gamma");
	ADD_SUBGROUP("Midtones", "midtones_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "midtones_exposure", PROPERTY_HINT_RANGE, "-4,4,0.01,suffix:EV", PROPERTY_USAGE_EDITOR), "set_midtones", "get_midtones");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "midtones_tint", PROPERTY_HINT_COLOR_NO_ALPHA), "set_midtones_tint", "get_midtones_tint");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "midtones_saturation", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_midtones_saturation", "get_midtones_saturation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "midtones_contrast", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_midtones_contrast", "get_midtones_contrast");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "midtones_black_point", PROPERTY_HINT_RANGE, "0,0.1,0.0001,or_greater"), "set_midtones_black_point", "get_midtones_black_point");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "midtones_white_point", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_midtones_white_point", "get_midtones_white_point");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "midtones_gamma", PROPERTY_HINT_RANGE, "0.05,4,0.01"), "set_midtones_gamma", "get_midtones_gamma");
	ADD_SUBGROUP("Highlights", "highlights_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "highlights_exposure", PROPERTY_HINT_RANGE, "-4,4,0.01,suffix:EV", PROPERTY_USAGE_EDITOR), "set_highlights", "get_highlights");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "highlights_tint", PROPERTY_HINT_COLOR_NO_ALPHA), "set_highlights_tint", "get_highlights_tint");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "highlights_saturation", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_highlights_saturation", "get_highlights_saturation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "highlights_contrast", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_highlights_contrast", "get_highlights_contrast");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "highlights_black_point", PROPERTY_HINT_RANGE, "0,0.1,0.0001,or_greater"), "set_highlights_black_point", "get_highlights_black_point");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "highlights_white_point", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_highlights_white_point", "get_highlights_white_point");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "highlights_gamma", PROPERTY_HINT_RANGE, "0.05,4,0.01"), "set_highlights_gamma", "get_highlights_gamma");

	ADD_GROUP("Tweaks", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "quality", PROPERTY_HINT_ENUM, "Low,Medium,High,Ultra"), "set_bake_quality", "get_bake_quality");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "supersampling"), "set_supersampling_enabled", "is_supersampling_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "supersampling_factor", PROPERTY_HINT_RANGE, "1,8,1"), "set_supersampling_factor", "get_supersampling_factor");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bounces", PROPERTY_HINT_RANGE, "0,6,1,or_greater"), "set_bounces", "get_bounces");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bounce_indirect_energy", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_bounce_indirect_energy", "get_bounce_indirect_energy");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "indirect_sample_clamp", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater"), "set_indirect_sample_clamp", "get_indirect_sample_clamp");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "directional"), "set_directional", "is_directional");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "specular_intensity", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_specular_intensity", "get_specular_intensity");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shadowmask_mode", PROPERTY_HINT_ENUM, "None,Replace,Overlay"), "set_shadowmask_mode", "get_shadowmask_mode");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_texture_for_bounces"), "set_use_texture_for_bounces", "is_using_texture_for_bounces");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "interior"), "set_interior", "is_interior");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_denoiser"), "set_use_denoiser", "is_using_denoiser");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "denoiser_strength", PROPERTY_HINT_RANGE, "0.001,0.2,0.001,or_greater"), "set_denoiser_strength", "get_denoiser_strength");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "denoiser_range", PROPERTY_HINT_RANGE, "1,20"), "set_denoiser_range", "get_denoiser_range");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bias", PROPERTY_HINT_RANGE, "0.00001,0.1,0.00001,or_greater"), "set_bias", "get_bias");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "bake_ao"), "set_bake_ao", "is_baking_ao");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ao_distance", PROPERTY_HINT_RANGE, "0.01,100.0,0.01,or_greater,suffix:m"), "set_ao_distance", "get_ao_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ao_strength", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_ao_strength", "get_ao_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ao_light_affect", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_ao_light_affect", "get_ao_light_affect");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ao_samples", PROPERTY_HINT_RANGE, "1,1024,1"), "set_ao_samples", "get_ao_samples");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "texel_scale", PROPERTY_HINT_RANGE, "0.01,100.0,0.01"), "set_texel_scale", "get_texel_scale");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_texture_size", PROPERTY_HINT_RANGE, "2048,16384,1"), "set_max_texture_size", "get_max_texture_size");
	ADD_GROUP("Environment", "environment_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "environment_mode", PROPERTY_HINT_ENUM, "Disabled,Scene,Custom Sky,Custom Color"), "set_environment_mode", "get_environment_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "environment_energy", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_environment_energy", "get_environment_energy");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "environment_custom_sky", PROPERTY_HINT_RESOURCE_TYPE, Sky::get_class_static()), "set_environment_custom_sky", "get_environment_custom_sky");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "environment_custom_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_environment_custom_color", "get_environment_custom_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "environment_custom_energy", PROPERTY_HINT_RANGE, "0,64,0.01"), "set_environment_custom_energy", "get_environment_custom_energy");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "camera_attributes", PROPERTY_HINT_RESOURCE_TYPE, "CameraAttributesPractical,CameraAttributesPhysical"), "set_camera_attributes", "get_camera_attributes");

	ADD_GROUP("Data", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "light_data", PROPERTY_HINT_RESOURCE_TYPE, LightmapGIData::get_class_static()), "set_light_data", "get_light_data");

	BIND_ENUM_CONSTANT(BAKE_MODE_LIGHTMAP_AND_PROBES);
	BIND_ENUM_CONSTANT(BAKE_MODE_LIGHTMAP_ONLY);
	BIND_ENUM_CONSTANT(BAKE_MODE_PROBES_ONLY);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_PROBE_SYSTEM);

	BIND_ENUM_CONSTANT(BAKE_QUALITY_LOW);
	BIND_ENUM_CONSTANT(BAKE_QUALITY_MEDIUM);
	BIND_ENUM_CONSTANT(BAKE_QUALITY_HIGH);
	BIND_ENUM_CONSTANT(BAKE_QUALITY_ULTRA);

	BIND_ENUM_CONSTANT(BAKE_ERROR_OK);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_SCENE_ROOT);
	BIND_ENUM_CONSTANT(BAKE_ERROR_FOREIGN_DATA);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_LIGHTMAPPER);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_SAVE_PATH);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_MESHES);
	BIND_ENUM_CONSTANT(BAKE_ERROR_MESHES_INVALID);
	BIND_ENUM_CONSTANT(BAKE_ERROR_CANT_CREATE_IMAGE);
	BIND_ENUM_CONSTANT(BAKE_ERROR_USER_ABORTED);
	BIND_ENUM_CONSTANT(BAKE_ERROR_TEXTURE_SIZE_TOO_SMALL);
	BIND_ENUM_CONSTANT(BAKE_ERROR_LIGHTMAP_TOO_SMALL);
	BIND_ENUM_CONSTANT(BAKE_ERROR_ATLAS_TOO_SMALL);

	BIND_ENUM_CONSTANT(ENVIRONMENT_MODE_DISABLED);
	BIND_ENUM_CONSTANT(ENVIRONMENT_MODE_SCENE);
	BIND_ENUM_CONSTANT(ENVIRONMENT_MODE_CUSTOM_SKY);
	BIND_ENUM_CONSTANT(ENVIRONMENT_MODE_CUSTOM_COLOR);
}

LightmapGI::LightmapGI() {
}

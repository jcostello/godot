/**************************************************************************/
/*  light_probe_editor_plugin.cpp                                         */
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

#include "light_probe_editor_plugin.h"

#include "core/math/dynamic_bvh.h"
#include "core/object/callable_mp.h"
#include "editor/editor_data.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/scene/3d/gizmos/gizmo_3d_helper.h"
#include "editor/settings/editor_settings.h"
#include "scene/3d/camera_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/option_button.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/shader.h"
#include "scene/scene_string_names.h"

LightProbeVolumeGizmoPlugin::LightProbeVolumeGizmoPlugin() {
	helper.instantiate();
	create_material("volume", Color(0.35, 0.75, 1.0, 0.5));
	create_handle_material("handles");
	probe_size = EDITOR_GET("editors/3d_gizmos/gizmo_settings/lightmap_gi_probe_size");

	// Shared latitude/longitude lines, without filled faces or triangle diagonals.
	const int stacks = 8;
	const int sectors = 16;
	PackedVector3Array vertices;
	vertices.resize(2 * sectors * (2 * stacks - 1));
	Vector3 *vertex_write = vertices.ptrw();
	int vertex = 0;
	auto sphere_point = [=](int p_stack, int p_sector) {
		const real_t latitude = Math::PI * p_stack / stacks;
		const real_t longitude = Math::TAU * p_sector / sectors;
		return Vector3(Math::sin(latitude) * Math::cos(longitude), Math::cos(latitude), Math::sin(latitude) * Math::sin(longitude));
	};
	for (int stack = 1; stack < stacks; stack++) {
		for (int sector = 0; sector < sectors; sector++) {
			vertex_write[vertex++] = sphere_point(stack, sector);
			vertex_write[vertex++] = sphere_point(stack, (sector + 1) % sectors);
		}
	}
	for (int sector = 0; sector < sectors; sector++) {
		for (int stack = 0; stack < stacks; stack++) {
			vertex_write[vertex++] = sphere_point(stack, sector);
			vertex_write[vertex++] = sphere_point(stack + 1, sector);
		}
	}
	DEV_ASSERT(vertex == vertices.size());
	PackedColorArray colors;
	colors.resize(vertices.size());
	colors.fill(Color(1, 1, 1));
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_COLOR] = colors;
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_shading_mode(StandardMaterial3D::SHADING_MODE_UNSHADED);
	material->set_flag(StandardMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	material->set_flag(StandardMaterial3D::FLAG_SRGB_VERTEX_COLOR, false);
	material->set_flag(StandardMaterial3D::FLAG_DISABLE_FOG, true);
	material->set_distance_fade(StandardMaterial3D::DISTANCE_FADE_PIXEL_DITHER);
	material->set_distance_fade_min_distance(probe_size * 0.5);
	material->set_distance_fade_max_distance(probe_size * 1.5);
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_LINES, arrays);
	mesh->surface_set_material(0, material);
	probe_mesh = mesh;
	Ref<SphereMesh> solid_sphere;
	solid_sphere.instantiate();
	solid_sphere->set_radius(1.0);
	solid_sphere->set_height(2.0);
	solid_sphere->set_radial_segments(16);
	solid_sphere->set_rings(8);
	baked_probe_mesh = solid_sphere;
	baked_probe_shader.instantiate();
	baked_probe_shader->set_code(R"(
shader_type spatial;
render_mode unshaded, fog_disabled;
uniform sampler2D probe_sh : filter_nearest, repeat_disable;
uniform mat3 normal_to_capture;
varying vec3 probe_light;
void vertex() {
    int probe = int(INSTANCE_CUSTOM.x);
    ivec2 base = ivec2((probe % 64) * 9, probe / 64);
    vec3 n = normalize(normal_to_capture * NORMAL);
    float c[9] = float[](
        0.886227, 1.023327 * n.y, 1.023327 * n.z, 1.023327 * n.x,
        0.858086 * n.x * n.y, 0.858086 * n.y * n.z,
        0.247708 * (3.0 * n.z * n.z - 1.0),
        0.858086 * n.x * n.z, 0.429043 * (n.x * n.x - n.y * n.y));
    probe_light = vec3(0.0);
    for (int j = 0; j < 9; j++) {
        probe_light += texelFetch(probe_sh, base + ivec2(j, 0), 0).rgb * c[j];
    }
}
void fragment() {
    // Unshaded materials do not receive camera exposure automatically.
    ALBEDO = clamp(probe_light / EXPOSURE, vec3(0.0), vec3(64512.0));
}
)");
}

String LightProbeVolumeGizmoPlugin::get_handle_name(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	return helper->box_get_handle_name(p_id);
}

Variant LightProbeVolumeGizmoPlugin::get_handle_value(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	return Object::cast_to<LightProbeVolume>(p_gizmo->get_node_3d())->get_size();
}

void LightProbeVolumeGizmoPlugin::begin_handle_action(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) {
	helper->initialize_handle_action(get_handle_value(p_gizmo, p_id, p_secondary), p_gizmo->get_node_3d()->get_global_transform());
}

void LightProbeVolumeGizmoPlugin::set_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point) {
	LightProbeVolume *volume = Object::cast_to<LightProbeVolume>(p_gizmo->get_node_3d());
	Vector3 segment[2];
	helper->get_segment(p_camera, p_point, segment);
	Vector3 size = volume->get_size();
	Vector3 position;
	helper->box_set_handle(segment, p_id, size, position);
	volume->set_size(size);
	volume->set_global_position(position);
}

void LightProbeVolumeGizmoPlugin::commit_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, const Variant &p_restore, bool p_cancel) {
	helper->box_commit_handle(TTR("Resize Light Probe Volume"), p_cancel, p_gizmo->get_node_3d());
}

bool LightProbeVolumeGizmoPlugin::has_gizmo(Node3D *p_node) {
	return Object::cast_to<LightProbeVolume>(p_node) != nullptr;
}

String LightProbeVolumeGizmoPlugin::get_gizmo_name() const {
	return "LightProbeVolume";
}

int LightProbeVolumeGizmoPlugin::get_priority() const {
	return -1;
}

void LightProbeVolumeGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	p_gizmo->clear();
	LightProbeVolume *volume = Object::cast_to<LightProbeVolume>(p_gizmo->get_node_3d());
	const AABB bounds(-volume->get_size() * 0.5, volume->get_size());
	Vector<Vector3> outline;
	for (int i = 0; i < 12; i++) {
		Vector3 a;
		Vector3 b;
		bounds.get_edge(i, a, b);
		outline.push_back(a);
		outline.push_back(b);
	}
	p_gizmo->add_lines(outline, get_material("volume", p_gizmo));
	p_gizmo->add_collision_segments(outline);
	if (p_gizmo->is_selected()) {
		p_gizmo->add_handles(helper->box_get_handles(volume->get_size()), get_material("handles"));
	}
	const EditorSelection *selection = EditorNode::get_singleton()->get_editor_selection();
	if (!selection->is_selected(volume) && (!volume->is_enabled() || !volume->get_system() || !selection->is_selected(volume->get_system()))) {
		return;
	}
	const Ref<LightProbeVolumeData> probe_data = volume->get_data();
	if (probe_data.is_null()) {
		return;
	}
	const PackedVector3Array positions = probe_data->get_positions();
	const PackedInt32Array ids = probe_data->get_probe_ids();
	if (positions.is_empty() || probe_size <= 0.0f) {
		return;
	}
	const HashSet<int> baked = baked_preview ? _add_baked_probes(p_gizmo, volume) : HashSet<int>();
	// One shared sphere mesh and one upload/draw group per volume.
	Ref<MultiMesh> spheres;
	spheres.instantiate();
	spheres->set_transform_format(MultiMesh::TRANSFORM_3D);
	spheres->set_use_colors(true);
	spheres->set_mesh(probe_mesh);
	spheres->set_instance_count(positions.size());
	const real_t radius = probe_size * 0.5;
	const Vector3 extent(radius, radius, radius);
	AABB sphere_bounds(positions[0] - extent, extent * 2.0);
	Vector<float> buffer;
	buffer.resize(positions.size() * 16); // 3x4 transform + RGBA instance color.
	float *write = buffer.ptrw();
	for (int i = 0; i < positions.size(); i++) {
		const Vector3 &point = positions[i];
		float *instance = write + i * 16;
		instance[0] = radius;
		instance[1] = 0;
		instance[2] = 0;
		instance[3] = point.x;
		instance[4] = 0;
		instance[5] = radius;
		instance[6] = 0;
		instance[7] = point.y;
		instance[8] = 0;
		instance[9] = 0;
		instance[10] = radius;
		instance[11] = point.z;
		if (baked.has(ids[i]) && !p_gizmo->is_subgizmo_selected(ids[i])) {
			// Keep the shared wire buffer layout; collapse baked, unselected markers.
			instance[0] = 0;
			instance[5] = 0;
			instance[10] = 0;
		} else if (baked.has(ids[i])) {
			// Lift the selection wire slightly above the shaded surface.
			instance[0] *= 1.02f;
			instance[5] *= 1.02f;
			instance[10] *= 1.02f;
		}
		const bool invalid = volume->get_probe_validation_status(ids[i]) > LightProbeVolume::VALIDATION_VALID;
		const bool selected = p_gizmo->is_subgizmo_selected(ids[i]);
		const Color color = invalid ? (selected ? Color(1.0, 0.4, 0.25) : Color(1.0, 0.12, 0.08)) : (selected ? Color(1.0, 0.8, 0.2) : Color(0.35, 0.75, 1.0));
		instance[12] = color.r;
		instance[13] = color.g;
		instance[14] = color.b;
		instance[15] = color.a;
		sphere_bounds.expand_to(point - extent);
		sphere_bounds.expand_to(point + extent);
	}
	spheres->set_custom_aabb(sphere_bounds);
	spheres->set_buffer(buffer);
	p_gizmo->add_multimesh(spheres);
}

void LightProbeVolumeGizmoPlugin::_clear_capture_lookups() {
	for (KeyValue<ObjectID, CaptureLookup *> &entry : capture_lookups) {
		memdelete(entry.value);
	}
	capture_lookups.clear();
}

LightProbeVolumeGizmoPlugin::~LightProbeVolumeGizmoPlugin() {
	_clear_capture_lookups();
}

HashSet<int> LightProbeVolumeGizmoPlugin::_add_baked_probes(EditorNode3DGizmo *p_gizmo, LightProbeVolume *p_volume) {
	HashSet<int> baked_ids;
	LightProbeSystem *system = p_volume->get_system();
	if (!system || p_volume->get_data().is_null() || system->get_light_data().is_null() || Math::is_zero_approx(system->get_capture_global_transform().basis.determinant()) || Math::is_zero_approx(p_volume->get_global_transform().basis.determinant())) {
		return baked_ids;
	}
	const Ref<LightProbeData> capture = system->get_light_data();
	const PackedVector3Array capture_points = capture->get_points();
	const PackedColorArray sh = capture->get_coefficients();
	if (capture_points.is_empty() || sh.size() != capture_points.size() * 9) {
		return baked_ids;
	}
	// shortcut: entries of freed LightProbeData are only dropped by this size cap; prune by ObjectID if it ever matters.
	if (capture_lookups.size() > 32) {
		_clear_capture_lookups();
	}
	CaptureLookup **cached_ptr = capture_lookups.getptr(capture->get_instance_id());
	if (!cached_ptr) {
		cached_ptr = &capture_lookups.insert(capture->get_instance_id(), memnew(CaptureLookup))->value;
	}
	CaptureLookup &cached = **cached_ptr;
	// Unchanged copy-on-write arrays keep sharing the same buffer.
	if (cached.points.ptr() != capture_points.ptr() || cached.points.size() != capture_points.size()) {
		cached.bvh.clear();
		for (int i = 0; i < capture_points.size(); i++) {
			cached.bvh.insert(AABB(capture_points[i], Vector3()), reinterpret_cast<void *>(uintptr_t(i + 1)));
		}
		cached.points = capture_points;
	}
	DynamicBVH &lookup = cached.bvh;
	const Transform3D to_capture = system->get_capture_global_transform().affine_inverse() * p_volume->get_global_transform();
	const PackedVector3Array positions = p_volume->get_data()->get_positions();
	const PackedInt32Array ids = p_volume->get_data()->get_probe_ids();
	Vector<int> matches;
	Vector<int> source_indices;
	for (int i = 0; i < positions.size(); i++) {
		if (p_volume->get_probe_validation_status(ids[i]) > LightProbeVolume::VALIDATION_VALID) {
			continue;
		}
		const Vector3 point = to_capture.xform(positions[i]);
		// Account for float transform roundoff, without sampling neighboring probes.
		const real_t tolerance = MAX(real_t(0.00001), point.abs()[point.abs().max_axis_index()] * real_t(0.000001));
		real_t best_distance = tolerance * tolerance;
		int match = -1;
		auto find = [&](void *p_entry) {
			const int index = int(reinterpret_cast<uintptr_t>(p_entry)) - 1;
			const real_t distance = point.distance_squared_to(capture_points[index]);
			if (distance < best_distance || (distance == best_distance && (match < 0 || index < match))) {
				match = index;
				best_distance = distance;
			}
			return false;
		};
		lookup.aabb_query(AABB(point, Vector3()).grow(tolerance), find);
		if (match >= 0) {
			matches.push_back(i);
			source_indices.push_back(match);
			baked_ids.insert(ids[i]);
		}
	}
	if (matches.is_empty()) {
		return baked_ids;
	}
	// One float texture and one instanced draw per volume. No per-probe materials
	// or expanded sphere geometry, and no HDR quantization through vertex colors.
	Ref<Image> image = Image::create_empty(9 * 64, (matches.size() + 63) / 64, false, Image::FORMAT_RGBAF);
	image->fill(Color(0, 0, 0, 0));
	const float inverse_exposure = 1.0f / MAX(1e-20f, capture->get_baked_exposure());
	Ref<MultiMesh> spheres;
	spheres.instantiate();
	spheres->set_transform_format(MultiMesh::TRANSFORM_3D);
	spheres->set_use_custom_data(true);
	spheres->set_mesh(baked_probe_mesh);
	spheres->set_instance_count(matches.size());
	const real_t radius = probe_size * 0.5;
	AABB bounds(positions[matches[0]], Vector3());
	for (int i = 0; i < matches.size(); i++) {
		for (int j = 0; j < 9; j++) {
			image->set_pixel((i % 64) * 9 + j, i / 64, sh[source_indices[i] * 9 + j] * inverse_exposure);
		}
		spheres->set_instance_transform(i, Transform3D(Basis::from_scale(Vector3(radius, radius, radius)), positions[matches[i]]));
		spheres->set_instance_custom_data(i, Color(i, 0, 0, 0));
		bounds.expand_to(positions[matches[i]]);
	}
	spheres->set_custom_aabb(bounds.grow(radius));
	Ref<ShaderMaterial> material;
	material.instantiate();
	material->set_shader(baked_probe_shader);
	material->set_shader_parameter("probe_sh", ImageTexture::create_from_image(image));
	const Basis normal_to_capture = system->get_capture_global_transform().basis.orthonormalized().transposed() * p_volume->get_global_transform().basis.inverse().transposed();
	material->set_shader_parameter("normal_to_capture", normal_to_capture);
	p_gizmo->add_multimesh(spheres, material, true);
	return baked_ids;
}

int LightProbeVolumeGizmoPlugin::subgizmos_intersect_ray(const EditorNode3DGizmo *p_gizmo, Camera3D *p_camera, const Vector2 &p_point) const {
	const LightProbeVolume *volume = Object::cast_to<LightProbeVolume>(p_gizmo->get_node_3d());
	const Ref<LightProbeVolumeData> probe_data = volume->get_data();
	if (probe_data.is_null()) {
		return -1;
	}
	const PackedVector3Array positions = probe_data->get_positions();
	const PackedInt32Array ids = probe_data->get_probe_ids();
	const Transform3D transform = volume->get_global_transform();
	ERR_FAIL_COND_V(Math::is_zero_approx(transform.basis.determinant()), -1);
	const Transform3D inverse = transform.affine_inverse();
	const Vector3 world_ray_origin = p_camera->project_ray_origin(p_point);
	const Vector3 ray_origin = inverse.xform(world_ray_origin);
	const Vector3 ray_direction = inverse.basis.xform(p_camera->project_ray_normal(p_point));
	const real_t ray_length_squared = ray_direction.length_squared();
	ERR_FAIL_COND_V(ray_length_squared <= 0.0 || !Math::is_finite(ray_length_squared), -1);
	const real_t radius = MAX(probe_size * 0.5f, 0.0f);
	int closest = -1;
	real_t closest_distance = 1e30;
	for (int i = 0; i < positions.size(); i++) {
		const Vector3 world = transform.xform(positions[i]);
		if (p_camera->is_position_behind(world)) {
			continue;
		}
		const Vector3 offset = ray_origin - positions[i];
		const real_t b = offset.dot(ray_direction);
		const real_t discriminant = b * b - ray_length_squared * (offset.length_squared() - radius * radius);
		const real_t root = Math::sqrt(MAX(real_t(0.0), discriminant));
		const bool hits_sphere = discriminant >= 0.0 && -b + root >= 0.0;
		// Keep a minimum screen-space target when a sphere is very small.
		if (!hits_sphere && p_camera->unproject_position(world).distance_squared_to(p_point) > 100.0) {
			continue;
		}
		const real_t distance = hits_sphere ? MAX(real_t(0.0), (-b - root) / ray_length_squared) : (world - world_ray_origin).length();
		if (distance < closest_distance) {
			closest = ids[i];
			closest_distance = distance;
		}
	}
	return closest;
}

Vector<int> LightProbeVolumeGizmoPlugin::subgizmos_intersect_frustum(const EditorNode3DGizmo *p_gizmo, const Camera3D *p_camera, const Vector<Plane> &p_frustum) const {
	Vector<int> result;
	const LightProbeVolume *volume = Object::cast_to<LightProbeVolume>(p_gizmo->get_node_3d());
	const Ref<LightProbeVolumeData> probe_data = volume->get_data();
	if (probe_data.is_null()) {
		return result;
	}
	const PackedVector3Array positions = probe_data->get_positions();
	const PackedInt32Array ids = probe_data->get_probe_ids();
	const Transform3D transform = volume->get_global_transform();
	for (int i = 0; i < positions.size(); i++) {
		const Vector3 world = transform.xform(positions[i]);
		bool inside = true;
		for (const Plane &plane : p_frustum) {
			if (plane.is_point_over(world)) {
				inside = false;
				break;
			}
		}
		if (inside) {
			result.push_back(ids[i]);
		}
	}
	return result;
}

Transform3D LightProbeVolumeGizmoPlugin::get_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id) const {
	const LightProbeVolume *volume = Object::cast_to<LightProbeVolume>(p_gizmo->get_node_3d());
	const Ref<LightProbeVolumeData> probe_data = volume->get_data();
	return Transform3D(Basis(), probe_data.is_valid() && probe_data->find_probe(p_id) >= 0 ? probe_data->get_probe_position(p_id) : Vector3());
}

void LightProbeVolumeGizmoPlugin::set_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id, Transform3D p_transform) {
	const LightProbeVolume *volume = Object::cast_to<LightProbeVolume>(p_gizmo->get_node_3d());
	const Ref<LightProbeVolumeData> probe_data = volume->get_data();
	if (probe_data.is_valid() && probe_data->find_probe(p_id) >= 0) {
		probe_data->set_probe_position(p_id, p_transform.origin);
	}
}

void LightProbeVolumeGizmoPlugin::commit_subgizmos(const EditorNode3DGizmo *p_gizmo, const Vector<int> &p_ids, const Vector<Transform3D> &p_restore, bool p_cancel) {
	LightProbeVolume *volume = Object::cast_to<LightProbeVolume>(p_gizmo->get_node_3d());
	const Ref<LightProbeVolumeData> probe_data = volume->get_data();
	if (probe_data.is_null()) {
		return;
	}
	ERR_FAIL_COND(p_ids.size() != p_restore.size());
	if (p_cancel) {
		for (int i = 0; i < p_ids.size(); i++) {
			if (probe_data->find_probe(p_ids[i]) >= 0) {
				probe_data->set_probe_position(p_ids[i], p_restore[i].origin);
			}
		}
		return;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Move Light Probes"), UndoRedo::MERGE_DISABLE, volume);
	for (int i = 0; i < p_ids.size(); i++) {
		if (probe_data->find_probe(p_ids[i]) >= 0) {
			undo_redo->add_do_method(probe_data.ptr(), "set_probe_position", p_ids[i], probe_data->get_probe_position(p_ids[i]));
			undo_redo->add_undo_method(probe_data.ptr(), "set_probe_position", p_ids[i], p_restore[i].origin);
		}
	}
	undo_redo->commit_action(false);
}

void LightProbeEditorPlugin::_update_validation_summary() {
	const LightProbeVolume *volume = ObjectDB::get_instance<LightProbeVolume>(edited_volume);
	LightProbeSystem *system = ObjectDB::get_instance<LightProbeSystem>(edited_system);
	clear_cache->set_disabled(!system || system->get_light_data().is_null());
	validation_summary->set_text(volume ? volume->get_validation_summary() : String());
	bool has_invalid = false;
	if (volume && volume->get_data().is_valid()) {
		for (int id : volume->get_data()->get_probe_ids()) {
			if (volume->get_probe_validation_status(id) > LightProbeVolume::VALIDATION_VALID) {
				has_invalid = true;
				break;
			}
		}
	}
	remove_invalid->set_disabled(!has_invalid);
}

void LightProbeEditorPlugin::_commit_layout(const String &p_action, const Dictionary &p_layout) {
	LightProbeVolume *volume = ObjectDB::get_instance<LightProbeVolume>(edited_volume);
	if (!volume || p_layout.is_empty()) {
		return;
	}
	Ref<LightProbeVolumeData> probe_data = volume->get_data();
	const bool create_data = probe_data.is_null();
	if (create_data) {
		probe_data.instantiate();
	}
	volume->clear_subgizmo_selection();
	EditorUndoRedoManager *undo_redo = get_undo_redo();
	undo_redo->create_action(p_action, UndoRedo::MERGE_DISABLE, volume);
	if (create_data) {
		undo_redo->add_do_method(volume, "set_data", probe_data);
		undo_redo->add_undo_method(volume, "set_data", Ref<LightProbeVolumeData>());
	}
	undo_redo->add_do_method(volume, "clear_subgizmo_selection");
	undo_redo->add_undo_method(volume, "clear_subgizmo_selection");
	undo_redo->add_do_method(probe_data.ptr(), "set_layout", p_layout);
	undo_redo->add_undo_method(probe_data.ptr(), "set_layout", probe_data->get_layout());
	undo_redo->commit_action();
}

void LightProbeEditorPlugin::_generate_grid() {
	LightProbeVolume *volume = ObjectDB::get_instance<LightProbeVolume>(edited_volume);
	if (!volume) {
		return;
	}
	pending_generation = edited_volume;
	if (volume->get_data().is_valid() && volume->get_data()->has_custom_probes()) {
		generate_dialog->popup_centered();
	} else {
		_regenerate_grid(false);
	}
}

bool LightProbeEditorPlugin::_grid_step(float p_progress, const String &p_description, void *p_userdata, bool p_refresh) {
	EditorProgress *progress = static_cast<EditorProgress *>(p_userdata);
	return progress->step(p_description, CLAMP(int(p_progress * 1000), 0, 1000), p_refresh);
}

void LightProbeEditorPlugin::_regenerate_grid(bool p_preserve_custom) {
	generate_dialog->hide();
	const ObjectID target = pending_generation;
	pending_generation = ObjectID();
	LightProbeVolume *volume = ObjectDB::get_instance<LightProbeVolume>(target);
	if (!volume || target != edited_volume || !get_tree()->get_edited_scene_root()) {
		return;
	}
	EditorProgress progress("generate_light_probe_grid", TTR("Generating Probe Grid"), 1000, true);
	const Dictionary layout = volume->create_filtered_grid_layout(get_tree()->get_edited_scene_root(), p_preserve_custom, _grid_step, &progress);
	if (target != edited_volume || !ObjectDB::get_instance<LightProbeVolume>(target)) {
		return;
	}
	_commit_layout(TTR("Generate Light Probe Grid"), layout);
}

void LightProbeEditorPlugin::_add_probe() {
	LightProbeVolume *volume = ObjectDB::get_instance<LightProbeVolume>(edited_volume);
	if (!volume) {
		return;
	}
	Ref<LightProbeVolumeData> probe_data = volume->get_data();
	if (probe_data.is_null()) {
		probe_data.instantiate();
	}
	ERR_FAIL_COND(probe_data->get_probe_count() >= LightProbeVolumeData::MAX_PROBES || probe_data->get_next_id() >= INT32_MAX);
	Dictionary layout = probe_data->get_layout();
	PackedVector3Array positions = layout["positions"];
	PackedInt32Array ids = layout["ids"];
	positions.push_back(Vector3());
	ids.push_back(probe_data->get_next_id());
	layout["positions"] = positions;
	layout["ids"] = ids;
	layout["next_id"] = probe_data->get_next_id() + 1;
	_commit_layout(TTR("Add Light Probe"), layout);
	for (const Ref<Node3DGizmo> &base : volume->get_gizmos()) {
		const Ref<EditorNode3DGizmo> gizmo = base;
		if (gizmo.is_valid() && gizmo->get_plugin() == gizmo_plugin) {
			volume->set_subgizmo_selection(base, ids[ids.size() - 1], Transform3D());
			break;
		}
	}
}

void LightProbeEditorPlugin::_remove_selected() {
	LightProbeVolume *volume = ObjectDB::get_instance<LightProbeVolume>(edited_volume);
	if (!volume || volume->get_data().is_null()) {
		return;
	}
	HashSet<int> selected;
	for (const Ref<Node3DGizmo> &base : volume->get_gizmos()) {
		const Ref<EditorNode3DGizmo> gizmo = base;
		if (gizmo.is_valid()) {
			for (int id : gizmo->get_subgizmo_selection()) {
				selected.insert(id);
			}
		}
	}
	if (selected.is_empty()) {
		return;
	}
	_remove_probes(selected, TTR("Remove Light Probes"));
}

void LightProbeEditorPlugin::_remove_invalid() {
	LightProbeVolume *volume = ObjectDB::get_instance<LightProbeVolume>(edited_volume);
	if (!volume || volume->get_data().is_null()) {
		return;
	}
	HashSet<int> invalid;
	for (int id : volume->get_data()->get_probe_ids()) {
		if (volume->get_probe_validation_status(id) > LightProbeVolume::VALIDATION_VALID) {
			invalid.insert(id);
		}
	}
	_remove_probes(invalid, TTR("Remove Invalid Light Probes"));
}

void LightProbeEditorPlugin::_remove_probes(const HashSet<int> &p_ids, const String &p_action) {
	LightProbeVolume *volume = ObjectDB::get_instance<LightProbeVolume>(edited_volume);
	if (!volume || volume->get_data().is_null() || p_ids.is_empty()) {
		return;
	}
	const Ref<LightProbeVolumeData> probe_data = volume->get_data();
	const PackedVector3Array previous_positions = probe_data->get_positions();
	const PackedInt32Array previous_ids = probe_data->get_probe_ids();
	PackedVector3Array positions;
	PackedInt32Array ids;
	for (int i = 0; i < previous_ids.size(); i++) {
		if (!p_ids.has(previous_ids[i])) {
			positions.push_back(previous_positions[i]);
			ids.push_back(previous_ids[i]);
		}
	}
	Dictionary layout = probe_data->get_layout();
	layout["positions"] = positions;
	layout["ids"] = ids;
	_commit_layout(p_action, layout);
}

bool LightProbeEditorPlugin::handles(Object *p_object) const {
	return Object::cast_to<LightProbeVolume>(p_object) != nullptr || Object::cast_to<LightProbeSystem>(p_object) != nullptr;
}

void LightProbeEditorPlugin::edit(Object *p_object) {
	generate_dialog->hide();
	pending_generation = ObjectID();
	const Callable changed = callable_mp(this, &LightProbeEditorPlugin::_update_validation_summary);
	if (LightProbeSystem *previous = ObjectDB::get_instance<LightProbeSystem>(edited_system)) {
		if (previous->is_connected(SNAME("bake_updated"), changed)) {
			previous->disconnect(SNAME("bake_updated"), changed);
		}
	}
	if (LightProbeVolume *previous = ObjectDB::get_instance<LightProbeVolume>(edited_volume)) {
		if (previous->is_connected(SNAME("validation_changed"), changed)) {
			previous->disconnect(SNAME("validation_changed"), changed);
		}
	}
	LightProbeVolume *volume = Object::cast_to<LightProbeVolume>(p_object);
	edited_volume = volume ? volume->get_instance_id() : ObjectID();
	LightProbeSystem *system = volume ? volume->get_system() : Object::cast_to<LightProbeSystem>(p_object);
	edited_system = system ? system->get_instance_id() : ObjectID();
	for (Button *control : volume_controls) {
		control->set_visible(volume != nullptr);
	}
	clear_cache->set_disabled(!system || system->get_light_data().is_null());
	if (system) {
		system->connect(SNAME("bake_updated"), changed);
	}
	if (volume) {
		volume->connect(SNAME("validation_changed"), changed);
	}
	_update_validation_summary();
}

void LightProbeEditorPlugin::make_visible(bool p_visible) {
	toolbar->set_visible(p_visible);
	if (!p_visible) {
		generate_dialog->hide();
		pending_generation = ObjectID();
	}
}

void LightProbeEditorPlugin::edited_scene_changed() {
	edit(nullptr);
	toolbar->hide();
}

void LightProbeEditorPlugin::_update_volume_gizmos(Node *p_node) {
	if (!p_node) {
		return;
	}
	if (LightProbeVolume *volume = Object::cast_to<LightProbeVolume>(p_node)) {
		volume->update_gizmos();
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_update_volume_gizmos(p_node->get_child(i));
	}
}

void LightProbeEditorPlugin::_preview_mode_changed(int p_index) {
	gizmo_plugin->set_baked_preview(p_index == 1);
	_selection_changed();
}

void LightProbeEditorPlugin::_clear_probe_cache() {
	if (LightProbeSystem *system = ObjectDB::get_instance<LightProbeSystem>(edited_system)) {
		system->clear_probe_cache();
	}
}

void LightProbeEditorPlugin::_selection_changed() {
	_update_volume_gizmos(get_tree()->get_edited_scene_root());
}

LightProbeEditorPlugin::LightProbeEditorPlugin() {
	EditorNode::get_singleton()->get_editor_selection()->connect(SNAME("selection_changed"), callable_mp(this, &LightProbeEditorPlugin::_selection_changed));
	gizmo_plugin.instantiate();
	add_node_3d_gizmo_plugin(gizmo_plugin);
	toolbar = memnew(HBoxContainer);
	Button *generate = memnew(Button(TTR("Generate Grid")));
	generate->set_tooltip_text(TTR("Generate a grid outside static opaque geometry. Choose whether to keep custom or moved probes. This action can be undone."));
	generate->connect(SceneStringName(pressed), callable_mp(this, &LightProbeEditorPlugin::_generate_grid));
	toolbar->add_child(generate);
	volume_controls.push_back(generate);
	Button *add = memnew(Button(TTR("Add Probe")));
	add->set_tooltip_text(TTR("Add a probe at the volume's local origin. Select its point gizmo to move it."));
	add->connect(SceneStringName(pressed), callable_mp(this, &LightProbeEditorPlugin::_add_probe));
	toolbar->add_child(add);
	volume_controls.push_back(add);
	Button *remove = memnew(Button(TTR("Remove Selected Probes")));
	remove->connect(SceneStringName(pressed), callable_mp(this, &LightProbeEditorPlugin::_remove_selected));
	toolbar->add_child(remove);
	volume_controls.push_back(remove);
	remove_invalid = memnew(Button(TTR("Remove Invalid Probes")));
	remove_invalid->set_tooltip_text(TTR("Remove probes marked invalid by the last validation, including moved probes that still have a red marker. Undo restores them. Bake again to refresh validation first if needed."));
	remove_invalid->set_disabled(true);
	remove_invalid->connect(SceneStringName(pressed), callable_mp(this, &LightProbeEditorPlugin::_remove_invalid));
	toolbar->add_child(remove_invalid);
	volume_controls.push_back(remove_invalid);

	preview_mode = memnew(OptionButton);
	preview_mode->add_item(TTR("Wireframe"));
	preview_mode->add_item(TTR("Baked Lighting"));
	preview_mode->select(1);
	preview_mode->set_tooltip_text(TTR("Preview baked probes using the same spherical harmonics and exposure as dynamic objects. Unbaked and moved probes stay wireframe; invalid probes stay red."));
	preview_mode->connect(SceneStringName(item_selected), callable_mp(this, &LightProbeEditorPlugin::_preview_mode_changed));
	toolbar->add_child(preview_mode);

	clear_cache = memnew(Button(TTR("Clear Probe Cache")));
	clear_cache->set_tooltip_text(TTR("Rebuild this system's probe topology on the next LightmapGI bake. Keeps the currently baked illumination."));
	clear_cache->connect(SceneStringName(pressed), callable_mp(this, &LightProbeEditorPlugin::_clear_probe_cache));
	toolbar->add_child(clear_cache);

	validation_summary = memnew(Label);
	toolbar->add_child(validation_summary);
	toolbar->hide();
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, toolbar);
	generate_dialog = memnew(ConfirmationDialog);
	generate_dialog->set_title(TTR("Generate Probe Grid"));
	generate_dialog->set_text(TTR("This volume contains custom or moved probes. Keep their positions and IDs when regenerating the grid?"));
	generate_dialog->set_ok_button_text(TTR("Keep Custom and Moved Probes"));
	generate_dialog->connect(SceneStringName(confirmed), callable_mp(this, &LightProbeEditorPlugin::_regenerate_grid).bind(true));
	Button *replace = generate_dialog->add_button(TTR("Replace All Probes"));
	replace->connect(SceneStringName(pressed), callable_mp(this, &LightProbeEditorPlugin::_regenerate_grid).bind(false));
	add_child(generate_dialog);
}

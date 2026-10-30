/**************************************************************************/
/*  light_probe_editor_plugin.h                                           */
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
#include "editor/plugins/editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_gizmos.h"
#include "scene/3d/light_probe_volume.h"

class HBoxContainer;
class Button;
class ConfirmationDialog;
class Label;
class Gizmo3DHelper;
class OptionButton;
class Shader;

class LightProbeVolumeGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(LightProbeVolumeGizmoPlugin, EditorNode3DGizmoPlugin);

	Ref<Gizmo3DHelper> helper;
	Ref<Mesh> probe_mesh;
	Ref<Mesh> baked_probe_mesh;
	Ref<Shader> baked_probe_shader;
	bool baked_preview = true;
	float probe_size = 0.4f;
	// Capture point lookup per LightProbeData, rebuilt only when its points change.
	struct CaptureLookup {
		PackedVector3Array points; // Holding a reference keeps the buffer, and its address, unique.
		DynamicBVH bvh;
	};
	// Pointers because DynamicBVH can't be copied.
	HashMap<ObjectID, CaptureLookup *> capture_lookups;
	void _clear_capture_lookups();
	HashSet<int> _add_baked_probes(EditorNode3DGizmo *p_gizmo, LightProbeVolume *p_volume);

public:
	void set_baked_preview(bool p_enabled) { baked_preview = p_enabled; }
	bool has_gizmo(Node3D *p_node) override;
	String get_gizmo_name() const override;
	int get_priority() const override;
	void redraw(EditorNode3DGizmo *p_gizmo) override;
	String get_handle_name(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const override;
	Variant get_handle_value(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const override;
	void begin_handle_action(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) override;
	void set_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point) override;
	void commit_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, const Variant &p_restore, bool p_cancel) override;
	int subgizmos_intersect_ray(const EditorNode3DGizmo *p_gizmo, Camera3D *p_camera, const Vector2 &p_point) const override;
	Vector<int> subgizmos_intersect_frustum(const EditorNode3DGizmo *p_gizmo, const Camera3D *p_camera, const Vector<Plane> &p_frustum) const override;
	Transform3D get_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id) const override;
	void set_subgizmo_transform(const EditorNode3DGizmo *p_gizmo, int p_id, Transform3D p_transform) override;
	void commit_subgizmos(const EditorNode3DGizmo *p_gizmo, const Vector<int> &p_ids, const Vector<Transform3D> &p_restore, bool p_cancel) override;
	LightProbeVolumeGizmoPlugin();
	~LightProbeVolumeGizmoPlugin();
};

class LightProbeEditorPlugin : public EditorPlugin {
	GDCLASS(LightProbeEditorPlugin, EditorPlugin);

	ObjectID edited_volume;
	ObjectID edited_system;
	HBoxContainer *toolbar = nullptr;
	Label *validation_summary = nullptr;
	Button *remove_invalid = nullptr;
	Button *clear_cache = nullptr;
	OptionButton *preview_mode = nullptr;
	Vector<Button *> volume_controls;
	ConfirmationDialog *generate_dialog = nullptr;
	ObjectID pending_generation;
	Ref<LightProbeVolumeGizmoPlugin> gizmo_plugin;

	void _update_validation_summary();
	void _selection_changed();
	void _preview_mode_changed(int p_index);
	void _clear_probe_cache();
	void _update_volume_gizmos(Node *p_node);
	void _generate_grid();
	void _regenerate_grid(bool p_preserve_custom);
	static bool _grid_step(float p_progress, const String &p_description, void *p_userdata, bool p_refresh);
	void _add_probe();
	void _remove_selected();
	void _remove_invalid();
	void _remove_probes(const HashSet<int> &p_ids, const String &p_action);
	void _commit_layout(const String &p_action, const Dictionary &p_layout);

public:
	String get_plugin_name() const override { return "LightProbes"; }
	bool handles(Object *p_object) const override;
	void edit(Object *p_object) override;
	void make_visible(bool p_visible) override;
	void edited_scene_changed() override;
	LightProbeEditorPlugin();
};

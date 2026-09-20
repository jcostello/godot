/**************************************************************************/
/*  light_3d_gizmo_plugin.cpp                                             */
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

#include "light_3d_gizmo_plugin.h"

#include "core/math/geometry_3d.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/3d/light_3d.h"

Light3DGizmoPlugin::Light3DGizmoPlugin() {
	// Enable vertex colors for the materials below as the gizmo color depends on the light color.
	create_material("lines_primary", Color(1, 1, 1), false, false, true);
	create_material("lines_secondary", Color(1, 1, 1, 0.35), false, false, true);
	create_material("lines_billboard", Color(1, 1, 1), true, false, true);

	create_icon_material("light_directional_icon", EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("GizmoDirectionalLight"), EditorStringName(EditorIcons)));
	create_icon_material("light_omni_icon", EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("GizmoLight"), EditorStringName(EditorIcons)));
	create_icon_material("light_spot_icon", EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("GizmoSpotLight"), EditorStringName(EditorIcons)));
	create_icon_material("light_area_icon", EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("GizmoAreaLight"), EditorStringName(EditorIcons)));

	create_handle_material("handles");
	create_handle_material("handles_billboard", true);
}

bool Light3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<Light3D>(p_spatial) != nullptr;
}

String Light3DGizmoPlugin::get_gizmo_name() const {
	return "Light3D";
}

int Light3DGizmoPlugin::get_priority() const {
	return -1;
}

String Light3DGizmoPlugin::get_handle_name(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	if (p_id == 2 && Object::cast_to<SpotLight3D>(p_gizmo->get_node_3d())) {
		return "Inner aperture";
	}
	if (p_id == 1 && Object::cast_to<OmniLight3D>(p_gizmo->get_node_3d())) {
		return "Inner radius";
	}
	if (p_id == 0) {
		if (Object::cast_to<AreaLight3D>(p_gizmo->get_node_3d())) {
			return "Area width";
		} else {
			return "Radius";
		}
	} else {
		if (Object::cast_to<AreaLight3D>(p_gizmo->get_node_3d())) {
			return "Area height";
		} else {
			return "Aperture";
		}
	}
}

Variant Light3DGizmoPlugin::get_handle_value(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	Light3D *light = Object::cast_to<Light3D>(p_gizmo->get_node_3d());
	if (p_id == 0) {
		AreaLight3D *al = Object::cast_to<AreaLight3D>(light);
		if (al) {
			return al->get_area_size();
		} else {
			return light->get_param(Light3D::PARAM_RANGE);
		}
	}
	if (p_id == 1) {
		if (Object::cast_to<OmniLight3D>(light)) {
			return light->get_param(Light3D::PARAM_RANGE_FADE_START);
		}
		AreaLight3D *al = Object::cast_to<AreaLight3D>(light);
		if (al) {
			return al->get_area_size();
		} else {
			return light->get_param(Light3D::PARAM_SPOT_ANGLE);
		}
	}

	if (p_id == 2 && Object::cast_to<SpotLight3D>(light)) {
		return light->get_param(Light3D::PARAM_SPOT_INNER_ANGLE);
	}

	return Variant();
}

void Light3DGizmoPlugin::set_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point) {
	Light3D *light = Object::cast_to<Light3D>(p_gizmo->get_node_3d());
	Transform3D gt = light->get_global_transform();
	Transform3D gi = gt.affine_inverse();

	Vector3 ray_from = p_camera->project_ray_origin(p_point);
	Vector3 ray_dir = p_camera->project_ray_normal(p_point);

	Vector3 s[2] = { gi.xform(ray_from), gi.xform(ray_from + ray_dir * 4096) };
	if (p_id == 2 && Object::cast_to<SpotLight3D>(light)) {
		// The inner handle is on the opposite side and closer to the origin,
		// so coincident cones and a zero inner angle remain independently editable.
		s[0].x = -s[0].x;
		s[1].x = -s[1].x;
		float outer_angle = light->get_param(Light3D::PARAM_SPOT_ANGLE);
		float a = _find_closest_angle_to_arc(s[0], s[1], light->get_param(Light3D::PARAM_RANGE) * 0.8, outer_angle);
		light->set_param(Light3D::PARAM_SPOT_INNER_ANGLE, CLAMP(a, 0.0f, outer_angle));
		return;
	}
	if (p_id == 1 && Object::cast_to<OmniLight3D>(light)) {
		Plane cp = Plane(p_camera->get_transform().basis.get_column(2), gt.origin);
		Vector3 inters;
		if (cp.intersects_ray(ray_from, ray_dir, &inters)) {
			float radius = inters.distance_to(gt.origin);
			if (Node3DEditor::get_singleton()->is_snap_enabled()) {
				radius = Math::snapped(radius, Node3DEditor::get_singleton()->get_translate_snap());
			}
			float outer = light->get_param(Light3D::PARAM_RANGE);
			light->set_param(Light3D::PARAM_RANGE_FADE_START, outer > 0.0f ? CLAMP(radius / outer, 0.0f, 1.0f) : 0.0f);
		}
		return;
	}
	if (p_id == 0) {
		if (Object::cast_to<SpotLight3D>(light)) {
			Vector3 ra, rb;
			Geometry3D::get_closest_points_between_segments(Vector3(), Vector3(0, 0, -4096), s[0], s[1], ra, rb);

			float d = -ra.z;
			if (Node3DEditor::get_singleton()->is_snap_enabled()) {
				d = Math::snapped(d, Node3DEditor::get_singleton()->get_translate_snap());
			}

			if (d <= 0) { // Equal is here for negative zero.
				d = 0;
			}

			light->set_param(Light3D::PARAM_RANGE, d);
		} else if (Object::cast_to<OmniLight3D>(light)) {
			Plane cp = Plane(p_camera->get_transform().basis.get_column(2), gt.origin);

			Vector3 inters;
			if (cp.intersects_ray(ray_from, ray_dir, &inters)) {
				float r = inters.distance_to(gt.origin);
				if (Node3DEditor::get_singleton()->is_snap_enabled()) {
					r = Math::snapped(r, Node3DEditor::get_singleton()->get_translate_snap());
				}

				light->set_param(Light3D::PARAM_RANGE, r);
			}

		} else if (Object::cast_to<AreaLight3D>(light)) {
			Vector3 cfv = p_camera->get_transform().basis.get_column(2);
			float cf_dot_lr = cfv.dot(gt.basis.get_column(0));
			const float min_cos_angle = 0.001; // if cosine of angle between cam forward and the edited light axis is less than this, we don't move the gizmo at all to prevent unstable results

			if (Math::abs(cf_dot_lr) < 1.0 - min_cos_angle) {
				float cf_dot_lf = cfv.dot(gt.basis.get_column(2));
				float cf_dot_lu = cfv.dot(gt.basis.get_column(1));
				Plane p;
				if (Math::abs(cf_dot_lf) > Math::abs(cf_dot_lu)) { // we are looking directly onto the light, use light plane
					p = Plane(gt.basis.get_column(2), gt.origin);
				} else { // we see the light at an angle, use plane normal to light up
					p = Plane(gt.basis.get_column(1), gt.origin);
				}
				Vector3 inters;
				if (p.intersects_ray(ray_from, ray_dir, &inters)) {
					Vector3 inv = gi.xform(inters); // point local to light

					float a = inv.x;
					if (a >= 0) {
						AreaLight3D *al = Object::cast_to<AreaLight3D>(light);
						Vector2 area_size = al->get_area_size();
						area_size.x = MAX(a * 2, 0.001);
						al->set_area_size(area_size);
					}
				}
			}
		}
	} else if (p_id == 1) {
		if (Object::cast_to<SpotLight3D>(light)) {
			float a = _find_closest_angle_to_arc(s[0], s[1], light->get_param(Light3D::PARAM_RANGE));
			light->set_param(Light3D::PARAM_SPOT_ANGLE, CLAMP(a, 0.01, 89.99));
		} else if (Object::cast_to<AreaLight3D>(light)) {
			Vector3 cfv = p_camera->get_transform().basis.get_column(2);
			float cf_dot_lu = cfv.dot(gt.basis.get_column(1));
			const float min_cos_angle = 0.001; // if cosine of angle between cam forward and the edited light axis is less than this, we don't move the gizmo at all to prevent unstable results

			if (Math::abs(cf_dot_lu) < 1.0 - min_cos_angle) {
				float cf_dot_lf = cfv.dot(gt.basis.get_column(2));
				float cf_dot_lr = cfv.dot(gt.basis.get_column(0));
				Plane p;
				if (Math::abs(cf_dot_lf) > Math::abs(cf_dot_lr)) { // we are looking directly onto the light, use light plane
					p = Plane(gt.basis.get_column(2), gt.origin);
				} else { // we see the light at an angle, use plane normal to light right
					p = Plane(gt.basis.get_column(0), gt.origin);
				}
				Vector3 inters;
				if (p.intersects_ray(ray_from, ray_dir, &inters)) {
					Vector3 inv = gi.xform(inters); // point local to light

					float b = inv.y;
					if (b >= 0) {
						AreaLight3D *al = Object::cast_to<AreaLight3D>(light);
						Vector2 area_size = al->get_area_size();
						area_size.y = MAX(b * 2, 0.001);
						al->set_area_size(area_size);
					}
				}
			}
		}
	}
}

void Light3DGizmoPlugin::commit_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, const Variant &p_restore, bool p_cancel) {
	Light3D *light = Object::cast_to<Light3D>(p_gizmo->get_node_3d());
	if (p_cancel) {
		AreaLight3D *al = Object::cast_to<AreaLight3D>(light);
		if (al) {
			al->set_area_size(p_restore);
		} else {
			light->set_param(p_id == 0 ? Light3D::PARAM_RANGE : (p_id == 2 ? Light3D::PARAM_SPOT_INNER_ANGLE : (Object::cast_to<OmniLight3D>(light) ? Light3D::PARAM_RANGE_FADE_START : Light3D::PARAM_SPOT_ANGLE)), p_restore);
		}
	} else if (p_id == 0) {
		EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
		AreaLight3D *al = Object::cast_to<AreaLight3D>(light);
		if (al) {
			ur->create_action(TTR("Change Area Light Width"));
			ur->add_do_method(al, "set_area_size", al->get_area_size());
			ur->add_undo_method(al, "set_area_size", p_restore);
			ur->commit_action();
		} else {
			ur->create_action(TTR("Change Light Radius"));
			ur->add_do_method(light, "set_param", Light3D::PARAM_RANGE, light->get_param(Light3D::PARAM_RANGE));
			ur->add_undo_method(light, "set_param", Light3D::PARAM_RANGE, p_restore);
			ur->commit_action();
		}
	} else if (p_id == 1 && Object::cast_to<OmniLight3D>(light)) {
		EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
		ur->create_action(TTR("Change Omni Light Inner Radius"));
		ur->add_do_method(light, "set_param", Light3D::PARAM_RANGE_FADE_START, light->get_param(Light3D::PARAM_RANGE_FADE_START));
		ur->add_undo_method(light, "set_param", Light3D::PARAM_RANGE_FADE_START, p_restore);
		ur->commit_action();
	} else if (p_id == 2 && Object::cast_to<SpotLight3D>(light)) {
		EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
		ur->create_action(TTR("Change Spot Light Inner Angle"));
		ur->add_do_method(light, "set_param", Light3D::PARAM_SPOT_INNER_ANGLE, light->get_param(Light3D::PARAM_SPOT_INNER_ANGLE));
		ur->add_undo_method(light, "set_param", Light3D::PARAM_SPOT_INNER_ANGLE, p_restore);
		ur->commit_action();
	} else if (p_id == 1) {
		EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
		AreaLight3D *al = Object::cast_to<AreaLight3D>(light);
		if (al) {
			ur->create_action(TTR("Change Area Light Height"));
			ur->add_do_method(al, "set_area_size", al->get_area_size());
			ur->add_undo_method(al, "set_area_size", p_restore);
			ur->commit_action();
		} else {
			ur->create_action(TTR("Change Spot Light Outer Angle"));
			ur->add_do_method(light, "set_param", Light3D::PARAM_SPOT_ANGLE, light->get_param(Light3D::PARAM_SPOT_ANGLE));
			ur->add_undo_method(light, "set_param", Light3D::PARAM_SPOT_ANGLE, p_restore);
			ur->commit_action();
		}
	}
}

void Light3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	Light3D *light = Object::cast_to<Light3D>(p_gizmo->get_node_3d());

	Color color = light->get_color().srgb_to_linear() * light->get_correlated_color().srgb_to_linear();
	color = color.linear_to_srgb();
	// Make the gizmo color as bright as possible for better visibility
	color.set_hsv(color.get_h(), color.get_s(), 1);

	p_gizmo->clear();

	if (Object::cast_to<DirectionalLight3D>(light)) {
		if (p_gizmo->is_selected()) {
			Ref<Material> material = get_material("lines_primary", p_gizmo);

			constexpr int arrow_points = 7;
			constexpr float arrow_length = 1.5;

			const Vector3 arrow[arrow_points] = {
				Vector3(0, 0, -1),
				Vector3(0, 0.8, 0),
				Vector3(0, 0.3, 0),
				Vector3(0, 0.3, arrow_length),
				Vector3(0, -0.3, arrow_length),
				Vector3(0, -0.3, 0),
				Vector3(0, -0.8, 0)
			};

			constexpr int arrow_sides = 2;

			Vector<Vector3> lines;

			for (int i = 0; i < arrow_sides; i++) {
				for (int j = 0; j < arrow_points; j++) {
					Basis ma(Vector3(0, 0, 1), Math::PI * i / arrow_sides);

					Vector3 v1 = arrow[j] - Vector3(0, 0, arrow_length);
					Vector3 v2 = arrow[(j + 1) % arrow_points] - Vector3(0, 0, arrow_length);

					lines.push_back(ma.xform(v1));
					lines.push_back(ma.xform(v2));
				}
			}

			p_gizmo->add_lines(lines, material, false, color);
		}

		Ref<Material> icon = get_material("light_directional_icon", p_gizmo);
		p_gizmo->add_unscaled_billboard(icon, 0.05, color);
	}

	if (Object::cast_to<OmniLight3D>(light)) {
		if (p_gizmo->is_selected()) {
			// Use both a billboard circle and 3 non-billboard circles for a better sphere-like representation
			const Ref<Material> lines_material = get_material("lines_secondary", p_gizmo);
			const Ref<Material> lines_billboard_material = get_material("lines_billboard", p_gizmo);

			OmniLight3D *on = Object::cast_to<OmniLight3D>(light);
			const float r = on->get_param(Light3D::PARAM_RANGE);
			const float fade_start = on->get_param(Light3D::PARAM_RANGE_FADE_START);
			const float inner_r = r * (fade_start < 0.0f ? 0.5f : fade_start);
			Vector<Vector3> inner_points;
			Vector<Vector3> points;
			Vector<Vector3> points_billboard;

			for (int i = 0; i < 120; i++) {
				// Create a circle
				const float ra = Math::deg_to_rad((float)(i * 3));
				const float rb = Math::deg_to_rad((float)((i + 1) * 3));
				const Point2 a = Vector2(Math::sin(ra), Math::cos(ra)) * r;
				const Point2 b = Vector2(Math::sin(rb), Math::cos(rb)) * r;

				// Draw axis-aligned circles
				points.push_back(Vector3(a.x, 0, a.y));
				points.push_back(Vector3(b.x, 0, b.y));
				points.push_back(Vector3(0, a.x, a.y));
				points.push_back(Vector3(0, b.x, b.y));
				points.push_back(Vector3(a.x, a.y, 0));
				points.push_back(Vector3(b.x, b.y, 0));

				if (fade_start >= 0.0f && i % 2 == 0) {
					inner_points.push_back(Vector3(a.x * fade_start, a.y * fade_start, 0));
					inner_points.push_back(Vector3(b.x * fade_start, b.y * fade_start, 0));
				}

				// Draw a billboarded circle
				points_billboard.push_back(Vector3(a.x, a.y, 0));
				points_billboard.push_back(Vector3(b.x, b.y, 0));
			}

			p_gizmo->add_lines(points, lines_material, true, color);
			p_gizmo->add_lines(points_billboard, lines_billboard_material, true, color);
			if (fade_start >= 0.0f) {
				p_gizmo->add_lines(inner_points, lines_material, true, color);
			}

			Vector<Vector3> handles;
			handles.push_back(Vector3(r, 0, 0));
			handles.push_back(Vector3(-inner_r, 0, 0));
			p_gizmo->add_handles(handles, get_material("handles_billboard"), Vector<int>(), true);
		}

		const Ref<Material> icon = get_material("light_omni_icon", p_gizmo);
		p_gizmo->add_unscaled_billboard(icon, 0.05, color);
	}

	if (Object::cast_to<SpotLight3D>(light)) {
		if (p_gizmo->is_selected()) {
			const Ref<Material> material_primary = get_material("lines_primary", p_gizmo);
			const Ref<Material> material_secondary = get_material("lines_secondary", p_gizmo);

			Vector<Vector3> points_primary;
			Vector<Vector3> points_secondary;
			SpotLight3D *sl = Object::cast_to<SpotLight3D>(light);

			float r = sl->get_param(Light3D::PARAM_RANGE);
			float w = r * Math::sin(Math::deg_to_rad(sl->get_param(Light3D::PARAM_SPOT_ANGLE)));
			float d = r * Math::cos(Math::deg_to_rad(sl->get_param(Light3D::PARAM_SPOT_ANGLE)));
			float inner_angle = sl->get_param(Light3D::PARAM_SPOT_INNER_ANGLE);
			// In legacy mode, offer a handle halfway into the cone to enable the new profile.
			float handle_angle = inner_angle < 0.0f ? sl->get_param(Light3D::PARAM_SPOT_ANGLE) * 0.5f : inner_angle;
			float inner_w = r * 0.8f * Math::sin(Math::deg_to_rad(handle_angle));
			float inner_d = r * 0.8f * Math::cos(Math::deg_to_rad(handle_angle));
			if (inner_angle >= 0.0f) {
				for (int i = 0; i < 120; i++) {
					const float a = Math::deg_to_rad(float(i * 3));
					const float b = Math::deg_to_rad(float((i + 1) * 3));
					Vector3 from(Math::sin(a) * inner_w, Math::cos(a) * inner_w, -inner_d);
					Vector3 to(Math::sin(b) * inner_w, Math::cos(b) * inner_w, -inner_d);
					if (i % 2 == 0) {
						points_secondary.push_back(from);
						points_secondary.push_back(to);
					}
					if (i % 30 == 0) {
						points_secondary.push_back(Vector3());
						points_secondary.push_back(from);
					}
				}
			}

			for (int i = 0; i < 120; i++) {
				// Draw a circle
				const float ra = Math::deg_to_rad((float)(i * 3));
				const float rb = Math::deg_to_rad((float)((i + 1) * 3));
				const Point2 a = Vector2(Math::sin(ra), Math::cos(ra)) * w;
				const Point2 b = Vector2(Math::sin(rb), Math::cos(rb)) * w;

				points_primary.push_back(Vector3(a.x, a.y, -d));
				points_primary.push_back(Vector3(b.x, b.y, -d));

				if (i % 15 == 0) {
					// Draw 8 lines from the cone origin to the sides of the circle
					points_secondary.push_back(Vector3(a.x, a.y, -d));
					points_secondary.push_back(Vector3());
				}
			}

			points_primary.push_back(Vector3(0, 0, -r));
			points_primary.push_back(Vector3());

			p_gizmo->add_lines(points_primary, material_primary, false, color);
			p_gizmo->add_lines(points_secondary, material_secondary, false, color);

			Vector<Vector3> handles = {
				Vector3(0, 0, -r),
				Vector3(w, 0, -d),
				Vector3(-inner_w, 0, -inner_d)
			};

			p_gizmo->add_handles(handles, get_material("handles"));
		}

		const Ref<Material> icon = get_material("light_spot_icon", p_gizmo);
		p_gizmo->add_unscaled_billboard(icon, 0.05, color);
	}

	if (Object::cast_to<AreaLight3D>(light)) {
		if (p_gizmo->is_selected()) {
			const Ref<Material> material = get_material("lines_primary", p_gizmo);
			Vector<Vector3> points;

			AreaLight3D *cl = Object::cast_to<AreaLight3D>(light);
			Vector2 area_size = cl->get_area_size();
			float a = area_size.x;
			float b = area_size.y;

			// Draw rectangle
			points.push_back(Vector3(-a / 2, b / 2, 0));
			points.push_back(Vector3(a / 2, b / 2, 0));
			points.push_back(Vector3(a / 2, b / 2, 0));
			points.push_back(Vector3(a / 2, -b / 2, 0));
			points.push_back(Vector3(a / 2, -b / 2, 0));
			points.push_back(Vector3(-a / 2, -b / 2, 0));
			points.push_back(Vector3(-a / 2, -b / 2, 0));
			points.push_back(Vector3(-a / 2, b / 2, 0));

			p_gizmo->add_lines(points, material, false, color);

			Vector<Vector3> handles = {
				Vector3(a / 2, 0, 0),
				Vector3(0, b / 2, 0)
			};

			p_gizmo->add_handles(handles, get_material("handles"));
		}

		const Ref<Material> icon = get_material("light_area_icon", p_gizmo);
		p_gizmo->add_unscaled_billboard(icon, 0.05, color);
	}
}

float Light3DGizmoPlugin::_find_closest_angle_to_arc(const Vector3 &p_from, const Vector3 &p_to, float p_arc_radius, float p_max_angle) {
	// Approximate the arc with segments for stable dragging.
	static const int arc_test_points = 64;
	float min_d = 1e20;
	Vector3 min_p;

	for (int i = 0; i < arc_test_points; i++) {
		float a = i * Math::deg_to_rad(p_max_angle) / arc_test_points;
		float an = (i + 1) * Math::deg_to_rad(p_max_angle) / arc_test_points;
		Vector3 p = Vector3(Math::sin(a), 0, -Math::cos(a)) * p_arc_radius;
		Vector3 n = Vector3(Math::sin(an), 0, -Math::cos(an)) * p_arc_radius;

		Vector3 ra, rb;
		Geometry3D::get_closest_points_between_segments(p, n, p_from, p_to, ra, rb);

		float d = ra.distance_to(rb);
		if (d < min_d) {
			min_d = d;
			min_p = ra;
		}
	}

	float a = Vector2(-min_p.z, MAX(min_p.x, 0.0f)).angle();
	return Math::rad_to_deg(a);
}

/**************************************************************************/
/*  environment_color_grading_editor.cpp                                 */
/**************************************************************************/

#include "environment_color_grading_editor.h"

#include "core/object/callable_mp.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_spin_slider.h"
#include "editor/inspector/editor_properties.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/lightmap_gi.h"
#include "scene/gui/box_container.h"
#include "scene/gui/tab_container.h"
#include "scene/resources/environment.h"
#include "servers/rendering/storage/environment_color_grading.h"

static const char *smh_color_properties[] = {
	"adjustment_shadows_color",
	"adjustment_midtones_color",
	"adjustment_highlights_color",
};

static const char *smh_luminance_properties[] = {
	"adjustment_shadows_intensity",
	"adjustment_midtones_intensity",
	"adjustment_highlights_intensity",
};

static const char *lgg_color_properties[] = { "adjustment_lift_color", "adjustment_gamma_color", "adjustment_gain_color" };
static const char *lgg_intensity_properties[] = { "adjustment_lift_intensity", "adjustment_gamma_intensity", "adjustment_gain_intensity" };

bool EnvironmentColorGradingTrackballControl::_is_vertical_layout() const {
	return get_size().x < 360.0 * EDSCALE;
}

Rect2 EnvironmentColorGradingTrackballControl::_get_trackball_rect(int p_index) const {
	if (_is_vertical_layout()) {
		const float row_height = 190.0 * EDSCALE;
		const float diameter = MAX(1.0f, MIN(get_size().x - 12.0 * EDSCALE, 132.0 * EDSCALE));
		return Rect2((get_size().x - diameter) * 0.5, row_height * p_index + 24.0 * EDSCALE, diameter, diameter);
	}

	const float column_width = get_size().x / trackball_count;
	const float diameter = MIN(column_width - 4.0 * EDSCALE, 132.0 * EDSCALE);
	return Rect2(column_width * p_index + (column_width - diameter) * 0.5, 24.0 * EDSCALE, diameter, diameter);
}

void EnvironmentColorGradingTrackballControl::_apply_trackball_motion(int p_index, const Vector2 &p_relative, bool p_fine, bool p_changing) {
	const float radius = _get_trackball_rect(p_index).size.x * 0.46f;
	const float sensitivity = 0.25f * (p_fine ? 0.1f : 1.0f);
	drag_color_position += p_relative * sensitivity / MAX(radius, 1.0f);
	if (drag_color_position.length_squared() > 1.0f) {
		drag_color_position = drag_color_position.normalized();
	}
	const float saturation = drag_color_position.length();
	const float hue = Math::fposmod(Math::atan2(drag_color_position.y, drag_color_position.x) / Math::TAU + 1.0, 1.0);
	Color color = Color::from_hsv(hue, saturation, 1.0);
	colors[p_index] = color;
	editor->set_trackball_color(p_index, color, p_changing);
	queue_redraw();
}

void EnvironmentColorGradingTrackballControl::_luminance_changed(double p_value, int p_index) {
	if (!updating) {
		editor->set_trackball_luminance(p_index, p_value);
	}
}

void EnvironmentColorGradingTrackballControl::_reset_trackball(int p_index) {
	ERR_FAIL_INDEX(p_index, TRACKBALL_COUNT);

	colors[p_index] = Color(1.0, 1.0, 1.0);
	updating = true;
	luminance_sliders[p_index]->set_value(neutral_value);
	updating = false;

	editor->reset_trackball(p_index);
	queue_redraw();
}

void EnvironmentColorGradingTrackballControl::_notification(int p_what) {
	if (p_what == NOTIFICATION_RESIZED) {
		const String labels[TRACKBALL_COUNT] = { lift_gamma_gain ? TTR("Lift") : TTR("Shadows"), lift_gamma_gain ? TTR("Gamma") : TTR("Midtones"), lift_gamma_gain ? TTR("Gain") : TTR("Highlights"), String() };
		Ref<Font> font = get_theme_font(SNAME("font"), SNAME("Label"));
		int font_size = get_theme_font_size(SNAME("font_size"), SNAME("Label"));
		const bool vertical = _is_vertical_layout();
		const float row_height = 190.0 * EDSCALE;
		const float item_width = vertical ? get_size().x : get_size().x / trackball_count;
		set_custom_minimum_size(Size2(0, (vertical ? trackball_count : 1) * row_height));
		for (int i = 0; i < trackball_count; i++) {
			const float item_x = vertical ? 0.0 : item_width * i;
			const float item_y = vertical ? row_height * i : 0.0;
			luminance_sliders[i]->set_position(Vector2(item_x + 6.0 * EDSCALE, item_y + row_height - 22.0 * EDSCALE));
			luminance_sliders[i]->set_size(Size2(item_width - 12.0 * EDSCALE, 20.0 * EDSCALE));
			const float label_width = font->get_string_size(labels[i], HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
			reset_buttons[i]->set_position(Vector2(item_x + item_width * 0.5f + label_width * 0.5f + 4.0 * EDSCALE, item_y + 1.0 * EDSCALE));
			reset_buttons[i]->set_size(Size2(18.0 * EDSCALE, 18.0 * EDSCALE));
		}
		return;
	}
	if (p_what == NOTIFICATION_THEME_CHANGED) {
		for (int i = 0; i < trackball_count; i++) {
			reset_buttons[i]->set_button_icon(get_editor_theme_icon(SNAME("Reload")));
		}
		return;
	}
	if (p_what != NOTIFICATION_DRAW) {
		return;
	}

	const String labels[TRACKBALL_COUNT] = { lift_gamma_gain ? TTR("Lift") : TTR("Shadows"), lift_gamma_gain ? TTR("Gamma") : TTR("Midtones"), lift_gamma_gain ? TTR("Gain") : TTR("Highlights"), String() };
	Ref<Font> font = get_theme_font(SNAME("font"), SNAME("Label"));
	int font_size = get_theme_font_size(SNAME("font_size"), SNAME("Label"));
	Color font_color = get_theme_color(SNAME("font_color"), SNAME("Label"));

	for (int i = 0; i < trackball_count; i++) {
		Rect2 rect = _get_trackball_rect(i);
		Vector2 center = rect.get_center();
		float radius = rect.size.x * 0.46;
		const float title_y = (_is_vertical_layout() ? 190.0 * EDSCALE * i : 0.0) + 16.0 * EDSCALE;
		draw_string(font, Vector2(rect.position.x + (rect.size.x - font->get_string_size(labels[i], HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x) * 0.5, title_y), labels[i], HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, font_color);
		draw_circle(center, radius, Color(0.16, 0.16, 0.16));
		for (int segment = 0; segment < 72; segment++) {
			float from = Math::TAU * segment / 72.0;
			float to = Math::TAU * (segment + 1) / 72.0;
			draw_arc(center, radius, from, to, 3, Color::from_hsv(float(segment) / 72.0, 1.0, 1.0), 7.0 * EDSCALE, true);
		}
		const float zero_radius = radius * 0.08f;
		const Color zero_color(0.72, 0.72, 0.72, 0.8);
		draw_line(center - Vector2(zero_radius * 1.7f, 0), center + Vector2(zero_radius * 1.7f, 0), zero_color, EDSCALE);
		draw_line(center - Vector2(0, zero_radius * 1.7f), center + Vector2(0, zero_radius * 1.7f), zero_color, EDSCALE);
		draw_arc(center, zero_radius, 0, Math::TAU, 16, zero_color, EDSCALE, true);
		float hue = colors[i].get_h();
		float saturation = colors[i].get_s();
		Vector2 marker = center + Vector2(Math::cos(hue * Math::TAU), Math::sin(hue * Math::TAU)) * saturation * radius;
		draw_circle(marker, 5.0 * EDSCALE, Color(0.08, 0.08, 0.08));
		draw_arc(marker, 5.0 * EDSCALE, 0, Math::TAU, 16, Color(0.9, 0.9, 0.9), 1.5 * EDSCALE, true);
	}
}

void EnvironmentColorGradingTrackballControl::gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> button = p_event;
	if (button.is_valid() && button->get_button_index() == MouseButton::LEFT) {
		if (button->is_pressed()) {
			if (editor->is_read_only()) {
				return;
			}
			for (int i = 0; i < trackball_count; i++) {
				if (_get_trackball_rect(i).has_point(button->get_position())) {
					dragging_trackball = i;
					const float hue = colors[i].get_h() * Math::TAU;
					drag_color_position = Vector2(Math::cos(hue), Math::sin(hue)) * colors[i].get_s();
					accept_event();
					return;
				}
			}
		} else if (dragging_trackball >= 0) {
			editor->set_trackball_color(dragging_trackball, colors[dragging_trackball], false);
			dragging_trackball = -1;
			accept_event();
		}
	}

	Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid() && dragging_trackball >= 0) {
		_apply_trackball_motion(dragging_trackball, motion->get_relative(), motion->is_shift_pressed(), true);
		accept_event();
	}
}

void EnvironmentColorGradingTrackballControl::set_read_only(bool p_read_only) {
	for (int i = 0; i < trackball_count; i++) {
		luminance_sliders[i]->set_editable(!p_read_only);
		reset_buttons[i]->set_disabled(p_read_only);
	}
}

void EnvironmentColorGradingEditor::_set_read_only(bool p_read_only) {
	trackballs->set_read_only(p_read_only);
}

void EnvironmentColorGradingEditor::update_property() {
	Object *edited_object = get_edited_object();
	ERR_FAIL_NULL(edited_object);
	Color values[TRACKBALL_COUNT];
	float luminances[TRACKBALL_COUNT];
	for (int i = 0; i < trackball_count; i++) {
		values[i] = edited_object->get(color_properties[i]);
		luminances[i] = edited_object->get(luminance_properties[i]);
	}
	trackballs->set_values(values, luminances);
}

void EnvironmentColorGradingEditor::set_trackball_color(int p_index, const Color &p_color, bool p_changing) {
	ERR_FAIL_INDEX(p_index, TRACKBALL_COUNT);
	emit_changed(color_properties[p_index], p_color, StringName(), p_changing);
}

void EnvironmentColorGradingEditor::set_trackball_luminance(int p_index, float p_luminance) {
	ERR_FAIL_INDEX(p_index, TRACKBALL_COUNT);
	emit_changed(luminance_properties[p_index], p_luminance);
}

void EnvironmentColorGradingEditor::reset_trackball(int p_index) {
	ERR_FAIL_INDEX(p_index, trackball_count);
	Object *edited_object = get_edited_object();
	ERR_FAIL_NULL(edited_object);
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	ERR_FAIL_NULL(undo_redo);
	undo_redo->create_action(TTR("Reset Color Grading Trackball"));
	undo_redo->add_do_property(edited_object, color_properties[p_index], Color(1, 1, 1));
	undo_redo->add_do_property(edited_object, luminance_properties[p_index], 0.0f);
	undo_redo->add_undo_property(edited_object, color_properties[p_index], edited_object->get(color_properties[p_index]));
	undo_redo->add_undo_property(edited_object, luminance_properties[p_index], edited_object->get(luminance_properties[p_index]));
	undo_redo->add_do_method(this, "update_property");
	undo_redo->add_undo_method(this, "update_property");
	undo_redo->commit_action();
	update_property();
}

void EnvironmentColorGradingTrackballControl::set_values(const Color p_colors[TRACKBALL_COUNT], const float p_luminances[TRACKBALL_COUNT]) {
	updating = true;
	for (int i = 0; i < trackball_count; i++) {
		colors[i] = p_colors[i];
		luminance_sliders[i]->set_value(p_luminances[i]);
	}
	updating = false;
	queue_redraw();
}

EnvironmentColorGradingTrackballControl::EnvironmentColorGradingTrackballControl(EnvironmentColorGradingEditor *p_editor, int p_count, float p_neutral_value, bool p_lift_gamma_gain) {
	editor = p_editor;
	trackball_count = p_count;
	neutral_value = p_neutral_value;
	lift_gamma_gain = p_lift_gamma_gain;
	set_custom_minimum_size(Size2(0, 190.0 * EDSCALE));
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_mouse_filter(MOUSE_FILTER_STOP);
	set_clip_contents(true);
	for (int i = 0; i < trackball_count; i++) {
		luminance_sliders[i] = memnew(HSlider);
		add_child(luminance_sliders[i]);
		luminance_sliders[i]->set_min(neutral_value == 0.0f ? -1.0 : 0.0);
		luminance_sliders[i]->set_max(neutral_value == 0.0f ? 1.0 : 2.0);
		luminance_sliders[i]->set_step(0.01);
		luminance_sliders[i]->set_value(neutral_value);
		luminance_sliders[i]->set_ticks(3);
		luminance_sliders[i]->set_ticks_position(Slider::TICK_POSITION_CENTER);
		luminance_sliders[i]->connect(SceneStringName(value_changed), callable_mp(this, &EnvironmentColorGradingTrackballControl::_luminance_changed).bind(i));

		reset_buttons[i] = memnew(Button);
		add_child(reset_buttons[i]);
		reset_buttons[i]->set_flat(true);
		reset_buttons[i]->set_focus_mode(FOCUS_NONE);
		reset_buttons[i]->set_tooltip_text(TTR("Reset this trackball and its luminance."));
		reset_buttons[i]->connect(SceneStringName(pressed), callable_mp(this, &EnvironmentColorGradingTrackballControl::_reset_trackball).bind(i));
	}
}

EnvironmentColorGradingEditor::EnvironmentColorGradingEditor(bool p_lift_gamma_gain) {
	trackball_count = 3;
	for (int i = 0; i < trackball_count; i++) {
		color_properties[i] = p_lift_gamma_gain ? lgg_color_properties[i] : smh_color_properties[i];
		luminance_properties[i] = p_lift_gamma_gain ? lgg_intensity_properties[i] : smh_luminance_properties[i];
	}
	set_draw_label(false);
	trackballs = memnew(EnvironmentColorGradingTrackballControl(this, trackball_count, 0.0f, p_lift_gamma_gain));
	add_child(trackballs);
	set_bottom_editor(trackballs);
}

bool EditorInspectorEnvironmentColorGradingPlugin::can_handle(Object *p_object) {
	return Object::cast_to<Environment>(p_object) != nullptr || Object::cast_to<LightmapGI>(p_object) != nullptr;
}

float EnvironmentTonalRangesControl::_value_to_x(float p_value) const {
	Rect2 bar = _bar_rect();
	return bar.position.x + CLAMP(p_value, 0.0f, 1.0f) * bar.size.x;
}

float EnvironmentTonalRangesControl::_x_to_value(float p_x) const {
	Rect2 bar = _bar_rect();
	if (bar.size.x <= 0.0f) {
		return 0.0f;
	}
	return CLAMP((p_x - bar.position.x) / bar.size.x, 0.0f, 1.0f);
}

Rect2 EnvironmentTonalRangesControl::_bar_rect() const {
	const float margin = 10.0f * EDSCALE;
	const float bar_height = 20.0f * EDSCALE;
	return Rect2(margin, 16.0f * EDSCALE, MAX(1.0f, get_size().x - margin * 2.0f), bar_height);
}

void EnvironmentTonalRangesControl::_set_handles_from_position(int p_handle, const Vector2 &p_position, bool p_changing) {
	float value = Math::snapped(_x_to_value(p_position.x - drag_offset), 0.001f);
	const int pair_start = p_handle & ~1;
	limits[p_handle] = p_handle % 2 == 0 ? MIN(value, limits[pair_start + 1]) : MAX(value, limits[pair_start]);
	editor->set_tonal_cutoff(p_handle, limits[p_handle], p_changing);
	queue_redraw();
}

void EnvironmentTonalRangesControl::_notification(int p_what) {
	if (p_what == NOTIFICATION_RESIZED) {
		set_custom_minimum_size(Size2(0, 62.0f * EDSCALE));
		return;
	}
	if (p_what != NOTIFICATION_DRAW) {
		return;
	}

	Rect2 bar = _bar_rect();
	const float handle_radius = 7.0f * EDSCALE;
	const float x0 = _value_to_x(0.0f);
	const Color shadow_color(0.30, 0.34, 0.42);
	const Color midtone_color(0.48, 0.49, 0.52);
	const Color highlight_color(0.77, 0.73, 0.61);
	const int steps = MAX(1, int(Math::ceil(bar.size.x)));
	for (int i = 0; i < steps; i++) {
		const float luma = (float(i) + 0.5f) / steps;
		const Vector3 weights = EnvironmentColorGrading::tonal_weights(luma, Vector2(limits[0], limits[1]), Vector2(limits[2], limits[3]));
		Color color = shadow_color * weights.x + midtone_color * weights.y + highlight_color * weights.z;
		color.a = 1.0f;
		draw_rect(Rect2(x0 + float(i) / steps * bar.size.x, bar.position.y, bar.size.x / steps, bar.size.y), color);
	}
	draw_rect(bar, Color(0.92, 0.92, 0.92, 0.3), false, 1.0f * EDSCALE);

	for (int i = 0; i < 4; i++) {
		const Vector2 handle(_value_to_x(limits[i]), bar.get_center().y);
		draw_circle(handle, handle_radius, Color(0.14, 0.14, 0.14));
		draw_circle(handle, handle_radius - 2.0f * EDSCALE, i < 2 ? shadow_color.lightened(0.5) : highlight_color.lightened(0.25));
	}

	Ref<Font> font = get_theme_font(SNAME("font"), SNAME("Label"));
	int font_size = get_theme_font_size(SNAME("font_size"), SNAME("Label"));
	Color font_color = get_theme_color(SNAME("font_color"), SNAME("Label"));
	const String label = TTR("Shadows  |  Midtones  |  Highlights");
	draw_string(font, Vector2(bar.position.x, bar.position.y + bar.size.y + 18.0f * EDSCALE), label, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, font_color);
}

void EnvironmentTonalRangesControl::gui_input(const Ref<InputEvent> &p_event) {
	if (editor->is_read_only()) {
		return;
	}
	Ref<InputEventMouseButton> button = p_event;
	if (button.is_valid() && button->get_button_index() == MouseButton::LEFT) {
		if (button->is_pressed()) {
			Rect2 bar = _bar_rect();
			if (!bar.grow(7.0f * EDSCALE).has_point(button->get_position())) {
				return;
			}
			float closest_distance = INFINITY;
			for (int i = 0; i < 4; i++) {
				const float distance = Math::abs(_value_to_x(limits[i]) - button->get_position().x);
				if (distance < closest_distance || (editor->lightmap_mode && Math::is_equal_approx(distance, closest_distance) && button->get_position().x > _value_to_x(limits[i]))) {
					closest_distance = distance;
					dragging_handle = i;
				}
			}
			const Vector2 handle_position(_value_to_x(limits[dragging_handle]), bar.get_center().y);
			drag_offset = handle_position.distance_to(button->get_position()) <= 7.0f * EDSCALE ? button->get_position().x - handle_position.x : 0.0f;
			_set_handles_from_position(dragging_handle, button->get_position(), true);
			accept_event();
		} else if (dragging_handle >= 0) {
			_set_handles_from_position(dragging_handle, button->get_position(), false);
			dragging_handle = -1;
			accept_event();
		}
	}

	Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid() && dragging_handle >= 0) {
		_set_handles_from_position(dragging_handle, motion->get_position(), true);
		accept_event();
	}
}

void EnvironmentTonalRangesControl::set_values(const float p_limits[4]) {
	for (int i = 0; i < 4; i++) {
		limits[i] = p_limits[i];
	}
	queue_redraw();
}

EnvironmentTonalRangesControl::EnvironmentTonalRangesControl(EnvironmentTonalRangesEditor *p_editor) {
	editor = p_editor;
	set_custom_minimum_size(Size2(0, 62.0f * EDSCALE));
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_mouse_filter(MOUSE_FILTER_STOP);
}

void EnvironmentTonalRangesEditor::update_property() {
	Object *edited_object = get_edited_object();
	ERR_FAIL_NULL(edited_object);
	float values[4];
	for (int i = 0; i < 4; i++) {
		values[i] = edited_object->get(properties[i]);
		cutoff_sliders[i]->set_value_no_signal(values[i]);
	}
	ranges_control->set_values(values);
}

void EnvironmentTonalRangesEditor::_set_read_only(bool p_read_only) {
	for (int i = 0; i < 4; i++) {
		cutoff_sliders[i]->set_read_only(p_read_only);
	}
}

void EnvironmentTonalRangesEditor::_cutoff_changed(double p_value, int p_handle) {
	set_tonal_cutoff(p_handle, p_value, false);
}

void EnvironmentTonalRangesEditor::set_tonal_cutoff(int p_handle, float p_value, bool p_changing) {
	if (is_read_only()) {
		return;
	}
	Object *edited_object = get_edited_object();
	ERR_FAIL_NULL(edited_object);
	float value;
	if (lightmap_mode) {
		// Keep this edit within its neighbors so Undo/Redo changes only the selected property.
		// The runtime setters can move adjacent boundaries when scripts assign crossing values.
		const float gap[3] = { 0.0001f, 0.0f, 0.0001f };
		const float minimum = p_handle == 0 ? 0.0f : float(edited_object->get(properties[p_handle - 1])) + gap[p_handle - 1];
		const float maximum = p_handle == 3 ? 1.0f : float(edited_object->get(properties[p_handle + 1])) - gap[p_handle];
		value = CLAMP(p_value, minimum, maximum);
	} else {
		const int pair_start = p_handle & ~1;
		const float other = edited_object->get(properties[p_handle % 2 == 0 ? pair_start + 1 : pair_start]);
		value = p_handle % 2 == 0 ? CLAMP(p_value, 0.0f, other) : CLAMP(p_value, other, 1.0f);
	}
	emit_changed(properties[p_handle], value, StringName(), p_changing);
	cutoff_sliders[p_handle]->set_value_no_signal(value);
	float values[4];
	for (int i = 0; i < 4; i++) {
		values[i] = i == p_handle ? value : float(edited_object->get(properties[i]));
	}
	ranges_control->set_values(values);
}

EnvironmentTonalRangesEditor::EnvironmentTonalRangesEditor(bool p_lightmap_mode) {
	lightmap_mode = p_lightmap_mode;
	if (lightmap_mode) {
		properties[0] = "tonal_midtones_start";
		properties[1] = "tonal_shadows_end";
		properties[2] = "tonal_highlights_start";
		properties[3] = "tonal_midtones_end";
	}
	set_draw_label(true);
	set_label(TTR("Tonal Transitions"));
	VBoxContainer *container = memnew(VBoxContainer);
	add_child(container);
	set_bottom_editor(container);
	ranges_control = memnew(EnvironmentTonalRangesControl(this));
	container->add_child(ranges_control);
	const String labels[4] = { TTR("Shadows Start"), TTR("Shadows End"), TTR("Highlights Start"), TTR("Highlights End") };
	for (int i = 0; i < 4; i++) {
		cutoff_sliders[i] = memnew(EditorSpinSlider);
		cutoff_sliders[i]->set_label(labels[i]);
		cutoff_sliders[i]->set_tooltip_text(lightmap_mode ? TTR("Transition boundary over the original lightmap luminance: 0 is the texture minimum and 1 is its maximum. Shadows Start is where midtones begin; Highlights End is where midtones stop contributing.") : TTR("Limit used by the Unity-style smooth transition over scene-linear luminance before tonemapping."));
		cutoff_sliders[i]->set_min(0.0);
		cutoff_sliders[i]->set_max(1.0);
		cutoff_sliders[i]->set_step(0.001);
		cutoff_sliders[i]->set_flat(true);
		container->add_child(cutoff_sliders[i]);
		add_focusable(cutoff_sliders[i]);
		cutoff_sliders[i]->connect(SceneStringName(value_changed), callable_mp(this, &EnvironmentTonalRangesEditor::_cutoff_changed).bind(i));
	}
}

static const char *tonal_correction_color_properties[4] = { "adjustment_correction_master_color", "adjustment_correction_shadows_color", "adjustment_correction_midtones_color", "adjustment_correction_highlights_color" };

static const char *tonal_correction_properties[4][5] = {
	{ "adjustment_correction_master_saturation", "adjustment_correction_master_contrast", "adjustment_correction_master_gamma", "adjustment_correction_master_gain", "adjustment_correction_master_lift" },
	{ "adjustment_correction_shadows_saturation", "adjustment_correction_shadows_contrast", "adjustment_correction_shadows_gamma", "adjustment_correction_shadows_gain", "adjustment_correction_shadows_lift" },
	{ "adjustment_correction_midtones_saturation", "adjustment_correction_midtones_contrast", "adjustment_correction_midtones_gamma", "adjustment_correction_midtones_gain", "adjustment_correction_midtones_lift" },
	{ "adjustment_correction_highlights_saturation", "adjustment_correction_highlights_contrast", "adjustment_correction_highlights_gamma", "adjustment_correction_highlights_gain", "adjustment_correction_highlights_lift" },
};

void EnvironmentTonalCorrectionEditor::_color_changed(const StringName &p_property, const Variant &p_value, const StringName &p_field, bool p_changing) {
	emit_changed(p_property, p_value, p_field, p_changing);
}

void EnvironmentTonalCorrectionEditor::_parameter_changed(double p_value, int p_range, int p_parameter) {
	if (!updating) {
		emit_changed(tonal_correction_properties[p_range][p_parameter], p_value);
	}
}

void EnvironmentTonalCorrectionEditor::_reset_control(int p_range, int p_parameter) {
	ERR_FAIL_INDEX(p_range, RANGE_COUNT);
	ERR_FAIL_COND(p_parameter < -1 || p_parameter >= PARAMETER_COUNT);
	if (is_read_only()) {
		return;
	}
	if (p_parameter == -1) {
		emit_changed(tonal_correction_color_properties[p_range], Color(1, 1, 1));
	} else {
		emit_changed(tonal_correction_properties[p_range][p_parameter], p_parameter == 4 ? 0.0f : 1.0f);
	}
}

void EnvironmentTonalCorrectionEditor::_reset_range(int p_range) {
	if (is_read_only()) {
		return;
	}
	ERR_FAIL_INDEX(p_range, RANGE_COUNT);
	Object *edited_object = get_edited_object();
	ERR_FAIL_NULL(edited_object);
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	ERR_FAIL_NULL(undo_redo);
	undo_redo->create_action(TTR("Reset Color Correction Range"));
	undo_redo->add_do_property(edited_object, tonal_correction_color_properties[p_range], Color(1, 1, 1));
	undo_redo->add_undo_property(edited_object, tonal_correction_color_properties[p_range], edited_object->get(tonal_correction_color_properties[p_range]));
	for (int parameter = 0; parameter < PARAMETER_COUNT; parameter++) {
		const char *parameter_property = tonal_correction_properties[p_range][parameter];
		undo_redo->add_do_property(edited_object, parameter_property, parameter == 4 ? 0.0f : 1.0f);
		undo_redo->add_undo_property(edited_object, parameter_property, edited_object->get(parameter_property));
	}
	undo_redo->add_do_method(this, "update_property");
	undo_redo->add_undo_method(this, "update_property");
	undo_redo->commit_action();
	update_property();
}

void EnvironmentTonalCorrectionEditor::_set_read_only(bool p_read_only) {
	for (int range = 0; range < RANGE_COUNT; range++) {
		reset_buttons[range]->set_disabled(p_read_only);
		color_reset_buttons[range]->set_disabled(p_read_only);
		color_editors[range]->set_read_only(p_read_only);
		for (int parameter = 0; parameter < PARAMETER_COUNT; parameter++) {
			sliders[range][parameter]->set_read_only(p_read_only);
			parameter_reset_buttons[range][parameter]->set_disabled(p_read_only);
		}
	}
}

void EnvironmentTonalCorrectionEditor::update_property() {
	Object *edited_object = get_edited_object();
	ERR_FAIL_NULL(edited_object);
	updating = true;
	for (int range = 0; range < RANGE_COUNT; range++) {
		color_editors[range]->set_object_and_property(edited_object, tonal_correction_color_properties[range]);
		color_editors[range]->update_property();
		for (int parameter = 0; parameter < PARAMETER_COUNT; parameter++) {
			sliders[range][parameter]->set_value(edited_object->get(tonal_correction_properties[range][parameter]));
		}
	}
	updating = false;
}

EnvironmentTonalCorrectionEditor::EnvironmentTonalCorrectionEditor() {
	set_draw_label(false);
	TabContainer *tabs = memnew(TabContainer);
	add_child(tabs);
	set_bottom_editor(tabs);
	const String range_labels[RANGE_COUNT] = { TTR("Master"), TTR("Shadows"), TTR("Midtones"), TTR("Highlights") };
	const String parameter_labels[PARAMETER_COUNT] = { TTR("Saturation"), TTR("Contrast"), TTR("Gamma"), TTR("Gain"), TTR("Lift") };
	const String tooltips[PARAMETER_COUNT] = {
		TTR("Adjust saturation while preserving linear luminance and fitting the RGB gamut. 1 is neutral."),
		TTR("Soft contrast using the existing contrast pivot. 1 is neutral."),
		TTR("Apply a power of 1 / Gamma. 1 is neutral; larger values brighten midtones."),
		TTR("Multiply RGB before lift and gamma. 1 is neutral."),
		TTR("Add to RGB before gamma. 0 is neutral."),
	};
	for (int range = 0; range < RANGE_COUNT; range++) {
		VBoxContainer *page = memnew(VBoxContainer);
		tabs->add_child(page);
		tabs->set_tab_title(range, range_labels[range]);
		HBoxContainer *color_row = memnew(HBoxContainer);
		page->add_child(color_row);
		color_editors[range] = memnew(EditorPropertyColor);
		color_editors[range]->setup(false);
		color_editors[range]->set_label(TTR("Color"));
		color_editors[range]->set_tooltip_text(TTR("Modulate this range in linear RGB. White is neutral; Amount controls the strength."));
		color_editors[range]->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		color_row->add_child(color_editors[range]);
		color_reset_buttons[range] = memnew(Button);
		color_row->add_child(color_reset_buttons[range]);
		color_reset_buttons[range]->set_text(TTR("Reset"));
		color_reset_buttons[range]->set_tooltip_text(TTR("Reset this range's color to neutral white."));
		color_reset_buttons[range]->connect(SceneStringName(pressed), callable_mp(this, &EnvironmentTonalCorrectionEditor::_reset_control).bind(range, -1));
		add_focusable(color_reset_buttons[range]);
		color_editors[range]->connect("property_changed", callable_mp(this, &EnvironmentTonalCorrectionEditor::_color_changed));
		for (int parameter = 0; parameter < PARAMETER_COUNT; parameter++) {
			EditorSpinSlider *slider = memnew(EditorSpinSlider);
			sliders[range][parameter] = slider;
			HBoxContainer *parameter_row = memnew(HBoxContainer);
			page->add_child(parameter_row);
			slider->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			parameter_row->add_child(slider);
			slider->set_label(parameter_labels[parameter]);
			slider->set_tooltip_text(tooltips[parameter]);
			slider->set_min(parameter == 4 ? -1.0 : parameter == 2 ? 0.05
																   : 0.0);
			slider->set_max(parameter == 4 ? 1.0 : 4.0);
			slider->set_step(0.01);
			slider->set_flat(true);
			add_focusable(slider);
			slider->connect(SceneStringName(value_changed), callable_mp(this, &EnvironmentTonalCorrectionEditor::_parameter_changed).bind(range, parameter));
			Button *reset_button = memnew(Button);
			parameter_reset_buttons[range][parameter] = reset_button;
			parameter_row->add_child(reset_button);
			reset_button->set_text(TTR("Reset"));
			reset_button->set_tooltip_text(vformat(TTR("Reset %s to its neutral value."), parameter_labels[parameter]));
			reset_button->connect(SceneStringName(pressed), callable_mp(this, &EnvironmentTonalCorrectionEditor::_reset_control).bind(range, parameter));
			add_focusable(reset_button);
		}
		reset_buttons[range] = memnew(Button);
		page->add_child(reset_buttons[range]);
		reset_buttons[range]->set_text(TTR("Reset Range"));
		reset_buttons[range]->set_tooltip_text(TTR("Restore this range's color and five correction controls to neutral values."));
		reset_buttons[range]->connect(SceneStringName(pressed), callable_mp(this, &EnvironmentTonalCorrectionEditor::_reset_range).bind(range));
		add_focusable(reset_buttons[range]);
	}
}

bool EditorInspectorEnvironmentColorGradingPlugin::parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) {
	if (Object::cast_to<LightmapGI>(p_object)) {
		static const Vector<String> lightmap_properties = {
			"tonal_midtones_start",
			"tonal_shadows_end",
			"tonal_highlights_start",
			"tonal_midtones_end",
		};
		if (p_path == lightmap_properties[0]) {
			add_property_editor_for_multiple_properties(String(), lightmap_properties, memnew(EnvironmentTonalRangesEditor(true)));
			return true;
		}
		return lightmap_properties.has(p_path);
	}
	static const Vector<String> correction_properties = []() {
		Vector<String> result;
		for (int range = 0; range < 4; range++) {
			for (int parameter = 0; parameter < 5; parameter++) {
				result.push_back(tonal_correction_properties[range][parameter]);
			}
		}
		for (int range = 0; range < 4; range++) {
			result.push_back(tonal_correction_color_properties[range]);
		}
		return result;
	}();
	if (p_path == correction_properties[0]) {
		add_property_editor_for_multiple_properties(String(), correction_properties, memnew(EnvironmentTonalCorrectionEditor));
		return true;
	}
	if (correction_properties.has(p_path)) {
		return true;
	}
	if (p_path == "adjustment_black_level" || p_path == "adjustment_white_level") {
		EditorPropertyColor *editor = memnew(EditorPropertyColor);
		editor->setup(false);
		editor->set_live_changes_enabled(false);
		add_property_editor(p_path, editor);
		return true;
	}
	static const Vector<String> tonal_properties = {
		"adjustment_shadows_start",
		"adjustment_shadows_end",
		"adjustment_highlights_start",
		"adjustment_highlights_end",
	};
	if (p_path == tonal_properties[0]) {
		add_property_editor_for_multiple_properties(String(), tonal_properties, memnew(EnvironmentTonalRangesEditor));
		return true;
	}
	return tonal_properties.has(p_path);
}

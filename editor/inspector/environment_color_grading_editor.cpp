/**************************************************************************/
/*  environment_color_grading_editor.cpp                                 */
/**************************************************************************/

#include "environment_color_grading_editor.h"

#include "core/object/callable_mp.h"
#include "editor/gui/editor_spin_slider.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/resources/environment.h"
#include "servers/rendering/storage/environment_color_grading.h"

const char *EnvironmentColorGradingEditor::color_properties[TRACKBALL_COUNT] = {
	"adjustment_offset_color",
	"adjustment_shadows_color",
	"adjustment_midtones_color",
	"adjustment_highlights_color",
};

const char *EnvironmentColorGradingEditor::luminance_properties[TRACKBALL_COUNT] = {
	"adjustment_offset_luminance",
	"adjustment_shadows_luminance",
	"adjustment_midtones_luminance",
	"adjustment_highlights_luminance",
};

bool EnvironmentColorGradingTrackballControl::_is_vertical_layout() const {
	return get_size().x < 360.0 * EDSCALE;
}

Rect2 EnvironmentColorGradingTrackballControl::_get_trackball_rect(int p_index) const {
	if (_is_vertical_layout()) {
		const float row_height = 190.0 * EDSCALE;
		const float diameter = MAX(1.0f, MIN(get_size().x - 12.0 * EDSCALE, 132.0 * EDSCALE));
		return Rect2((get_size().x - diameter) * 0.5, row_height * p_index + 24.0 * EDSCALE, diameter, diameter);
	}

	const float column_width = get_size().x / TRACKBALL_COUNT;
	const float diameter = MIN(column_width - 4.0 * EDSCALE, 132.0 * EDSCALE);
	return Rect2(column_width * p_index + (column_width - diameter) * 0.5, 24.0 * EDSCALE, diameter, diameter);
}

void EnvironmentColorGradingTrackballControl::_set_trackball_from_position(int p_index, const Vector2 &p_position, bool p_changing) {
	Rect2 rect = _get_trackball_rect(p_index);
	Vector2 offset = p_position - rect.get_center();
	const float radius = rect.size.x * 0.46;
	float saturation = MIN(offset.length() / radius, 1.0f);
	// Make it easy to return precisely to the neutral value without requiring
	// pixel-perfect positioning.
	if (saturation < 0.08f) {
		saturation = 0.0f;
	}
	float hue = Math::fposmod(Math::atan2(offset.y, offset.x) / Math::TAU + 1.0, 1.0);
	Color color = Color::from_hsv(hue, saturation, 1.0);
	colors[p_index] = color;
	editor->set_trackball_color(p_index, color, p_changing);
	queue_redraw();
}

void EnvironmentColorGradingTrackballControl::_luminance_changed(double p_value, int p_index) {
	if (!updating) {
		double value = p_value;
		if (Math::abs(value - 1.0) < 0.05) {
			value = 1.0;
			if (!Math::is_equal_approx(p_value, value)) {
				updating = true;
				luminance_sliders[p_index]->set_value(value);
				updating = false;
			}
		}
		editor->set_trackball_luminance(p_index, value);
	}
}

void EnvironmentColorGradingTrackballControl::_reset_trackball(int p_index) {
	ERR_FAIL_INDEX(p_index, TRACKBALL_COUNT);

	colors[p_index] = Color(1.0, 1.0, 1.0);
	updating = true;
	luminance_sliders[p_index]->set_value(1.0);
	updating = false;

	editor->set_trackball_color(p_index, colors[p_index], false);
	editor->set_trackball_luminance(p_index, 1.0);
	queue_redraw();
}

void EnvironmentColorGradingTrackballControl::_notification(int p_what) {
	if (p_what == NOTIFICATION_RESIZED) {
		const String labels[TRACKBALL_COUNT] = { TTR("Offset"), TTR("Shadows"), TTR("Midtones"), TTR("Highlights") };
		Ref<Font> font = get_theme_font(SNAME("font"), SNAME("Label"));
		int font_size = get_theme_font_size(SNAME("font_size"), SNAME("Label"));
		const bool vertical = _is_vertical_layout();
		const float row_height = 190.0 * EDSCALE;
		const float item_width = vertical ? get_size().x : get_size().x / TRACKBALL_COUNT;
		set_custom_minimum_size(Size2(0, (vertical ? TRACKBALL_COUNT : 1) * row_height));
		for (int i = 0; i < TRACKBALL_COUNT; i++) {
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
		for (int i = 0; i < TRACKBALL_COUNT; i++) {
			reset_buttons[i]->set_button_icon(get_editor_theme_icon(SNAME("Reload")));
		}
		return;
	}
	if (p_what != NOTIFICATION_DRAW) {
		return;
	}

	const String labels[TRACKBALL_COUNT] = { TTR("Offset"), TTR("Shadows"), TTR("Midtones"), TTR("Highlights") };
	Ref<Font> font = get_theme_font(SNAME("font"), SNAME("Label"));
	int font_size = get_theme_font_size(SNAME("font_size"), SNAME("Label"));
	Color font_color = get_theme_color(SNAME("font_color"), SNAME("Label"));

	for (int i = 0; i < TRACKBALL_COUNT; i++) {
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
			for (int i = 0; i < TRACKBALL_COUNT; i++) {
				if (_get_trackball_rect(i).has_point(button->get_position())) {
					dragging_trackball = i;
					_set_trackball_from_position(i, button->get_position(), true);
					accept_event();
					return;
				}
			}
		} else if (dragging_trackball >= 0) {
			_set_trackball_from_position(dragging_trackball, button->get_position(), false);
			dragging_trackball = -1;
			accept_event();
		}
	}

	Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid() && dragging_trackball >= 0) {
		_set_trackball_from_position(dragging_trackball, motion->get_position(), true);
		accept_event();
	}
}

void EnvironmentColorGradingEditor::update_property() {
	Object *edited_object = get_edited_object();
	ERR_FAIL_NULL(edited_object);
	Color values[TRACKBALL_COUNT];
	float luminances[TRACKBALL_COUNT];
	for (int i = 0; i < TRACKBALL_COUNT; i++) {
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

void EnvironmentColorGradingTrackballControl::set_values(const Color p_colors[TRACKBALL_COUNT], const float p_luminances[TRACKBALL_COUNT]) {
	updating = true;
	for (int i = 0; i < TRACKBALL_COUNT; i++) {
		colors[i] = p_colors[i];
		luminance_sliders[i]->set_value(p_luminances[i]);
	}
	updating = false;
	queue_redraw();
}

EnvironmentColorGradingTrackballControl::EnvironmentColorGradingTrackballControl(EnvironmentColorGradingEditor *p_editor) {
	editor = p_editor;
	set_custom_minimum_size(Size2(0, 190.0 * EDSCALE));
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_mouse_filter(MOUSE_FILTER_STOP);
	set_clip_contents(true);
	for (int i = 0; i < TRACKBALL_COUNT; i++) {
		luminance_sliders[i] = memnew(HSlider);
		add_child(luminance_sliders[i]);
		luminance_sliders[i]->set_min(0.0);
		luminance_sliders[i]->set_max(2.0);
		luminance_sliders[i]->set_step(0.01);
		luminance_sliders[i]->set_value(1.0);
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

EnvironmentColorGradingEditor::EnvironmentColorGradingEditor() {
	set_draw_label(false);
	trackballs = memnew(EnvironmentColorGradingTrackballControl(this));
	add_child(trackballs);
	set_bottom_editor(trackballs);
}

bool EditorInspectorEnvironmentColorGradingPlugin::can_handle(Object *p_object) {
	return Object::cast_to<Environment>(p_object) != nullptr;
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
	if (p_handle == 0) {
		midtones_start = MIN(value, midtones_end);
	} else {
		midtones_end = MAX(value, midtones_start);
	}
	editor->set_tonal_cutoff(p_handle, p_handle == 0 ? midtones_start : midtones_end, p_changing);
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
	const float x1 = _value_to_x(midtones_start);
	const float x2 = _value_to_x(midtones_end);

	const Color shadow_color(0.30, 0.34, 0.42);
	const Color midtone_color(0.48, 0.49, 0.52);
	const Color highlight_color(0.77, 0.73, 0.61);
	const int steps = MAX(1, int(Math::ceil(bar.size.x)));
	for (int i = 0; i < steps; i++) {
		const float luma = (float(i) + 0.5f) / steps;
		const Vector3 weights = EnvironmentColorGrading::tonal_weights(luma, Vector2(midtones_start, midtones_end), softness);
		Color color = shadow_color * weights.x + midtone_color * weights.y + highlight_color * weights.z;
		color.a = 1.0f;
		draw_rect(Rect2(x0 + float(i) / steps * bar.size.x, bar.position.y, bar.size.x / steps, bar.size.y), color);
	}
	draw_rect(bar, Color(0.92, 0.92, 0.92, 0.3), false, 1.0f * EDSCALE);

	Vector2 h1(x1, bar.get_center().y);
	Vector2 h2(x2, bar.get_center().y);
	draw_circle(h1, handle_radius, Color(0.14, 0.14, 0.14));
	draw_circle(h1, handle_radius - 2.0f * EDSCALE, Color(0.95, 0.95, 0.95));
	draw_circle(h2, handle_radius, Color(0.14, 0.14, 0.14));
	draw_circle(h2, handle_radius - 2.0f * EDSCALE, Color(0.95, 0.95, 0.95));

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
			Vector2 h1(_value_to_x(midtones_start), bar.get_center().y);
			Vector2 h2(_value_to_x(midtones_end), bar.get_center().y);
			float d1 = h1.distance_to(button->get_position());
			float d2 = h2.distance_to(button->get_position());
			dragging_handle = d1 <= d2 ? 0 : 1;
			if (Math::is_equal_approx(midtones_start, midtones_end)) {
				// Coincident cuts can still be pulled apart in either direction.
				dragging_handle = button->get_position().x < h1.x ? 0 : 1;
			}
			const Vector2 handle_position = dragging_handle == 0 ? h1 : h2;
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

void EnvironmentTonalRangesControl::set_values(float p_midtones_start, float p_midtones_end, const Vector2 &p_softness) {
	softness = p_softness;
	midtones_start = p_midtones_start;
	midtones_end = p_midtones_end;
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
	const float start = edited_object->get("adjustment_midtones_start");
	const float end = edited_object->get("adjustment_midtones_end");
	const Vector2 softness(edited_object->get("adjustment_shadows_softness"), edited_object->get("adjustment_highlights_softness"));
	ranges_control->set_values(start, end, softness);
	softness_sliders[0]->set_value_no_signal(softness.x);
	softness_sliders[1]->set_value_no_signal(softness.y);
	cutoff_sliders[0]->set_value_no_signal(start);
	cutoff_sliders[1]->set_value_no_signal(end);
}

void EnvironmentTonalRangesEditor::_set_read_only(bool p_read_only) {
	for (int i = 0; i < 2; i++) {
		cutoff_sliders[i]->set_read_only(p_read_only);
		softness_sliders[i]->set_read_only(p_read_only);
	}
}

void EnvironmentTonalRangesEditor::_softness_changed(double p_value, int p_handle) {
	if (is_read_only()) {
		return;
	}
	const float value = CLAMP(float(p_value), 0.0f, 1.0f);
	emit_changed(p_handle == 0 ? "adjustment_shadows_softness" : "adjustment_highlights_softness", value);
	update_property();
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
	const float start = edited_object->get("adjustment_midtones_start");
	const float end = edited_object->get("adjustment_midtones_end");
	const float value = p_handle == 0 ? CLAMP(p_value, 0.0f, end) : CLAMP(p_value, start, 1.0f);
	// Change just the selected cutoff. Updating several interdependent properties
	// in sequence clipped values against stale limits and broke undo grouping.
	emit_changed(p_handle == 0 ? "adjustment_midtones_start" : "adjustment_midtones_end", value, StringName(), p_changing);
	cutoff_sliders[p_handle]->set_value_no_signal(value);
	const Vector2 softness(edited_object->get("adjustment_shadows_softness"), edited_object->get("adjustment_highlights_softness"));
	ranges_control->set_values(p_handle == 0 ? value : start, p_handle == 1 ? value : end, softness);
}

EnvironmentTonalRangesEditor::EnvironmentTonalRangesEditor() {
	set_draw_label(true);
	set_label(TTR("Tonal Transitions"));
	VBoxContainer *container = memnew(VBoxContainer);
	add_child(container);
	set_bottom_editor(container);
	ranges_control = memnew(EnvironmentTonalRangesControl(this));
	container->add_child(ranges_control);
	for (int i = 0; i < 2; i++) {
		cutoff_sliders[i] = memnew(EditorSpinSlider);
		cutoff_sliders[i]->set_label(i == 0 ? TTR("Shadows/Midtones") : TTR("Midtones/Highlights"));
		cutoff_sliders[i]->set_tooltip_text(TTR("Transition center in perceptual sRGB luma after brightness, contrast, saturation, offset and tint. Softness widens the transition equally on both sides without moving this center."));
		cutoff_sliders[i]->set_min(0.0);
		cutoff_sliders[i]->set_max(1.0);
		cutoff_sliders[i]->set_step(0.001);
		cutoff_sliders[i]->set_flat(true);
		container->add_child(cutoff_sliders[i]);
		add_focusable(cutoff_sliders[i]);
		cutoff_sliders[i]->connect(SceneStringName(value_changed), callable_mp(this, &EnvironmentTonalRangesEditor::_cutoff_changed).bind(i));
		softness_sliders[i] = memnew(EditorSpinSlider);
		softness_sliders[i]->set_label(i == 0 ? TTR("Shadow Softness") : TTR("Highlight Softness"));
		softness_sliders[i]->set_tooltip_text(TTR("Total transition width. A width of 0.1 around a center of 0.3 blends from 0.25 to 0.35. Zero makes an exact cutoff. Overlapping transitions are normalized."));
		softness_sliders[i]->set_min(0.0);
		softness_sliders[i]->set_max(1.0);
		softness_sliders[i]->set_step(0.001);
		softness_sliders[i]->set_flat(true);
		container->add_child(softness_sliders[i]);
		add_focusable(softness_sliders[i]);
		softness_sliders[i]->connect(SceneStringName(value_changed), callable_mp(this, &EnvironmentTonalRangesEditor::_softness_changed).bind(i));
	}
}

bool EditorInspectorEnvironmentColorGradingPlugin::parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) {
	static const Vector<String> properties = {
		"adjustment_offset_color",
		"adjustment_offset_luminance",
		"adjustment_shadows_color",
		"adjustment_shadows_luminance",
		"adjustment_midtones_color",
		"adjustment_midtones_luminance",
		"adjustment_highlights_color",
		"adjustment_highlights_luminance",
	};
	static const Vector<String> tonal_properties = {
		"adjustment_midtones_start",
		"adjustment_midtones_end",
		"adjustment_shadows_softness",
		"adjustment_highlights_softness",
	};
	if (p_path == properties[0]) {
		add_property_editor_for_multiple_properties(String(), properties, memnew(EnvironmentColorGradingEditor));
		return true;
	}
	if (p_path == tonal_properties[0]) {
		add_property_editor_for_multiple_properties(String(), tonal_properties, memnew(EnvironmentTonalRangesEditor));
		return true;
	}
	return properties.has(p_path) || tonal_properties.has(p_path);
}

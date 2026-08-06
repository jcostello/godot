/**************************************************************************/
/*  environment_color_grading_editor.cpp                                 */
/**************************************************************************/

#include "environment_color_grading_editor.h"

#include "core/object/callable_mp.h"
#include "editor/themes/editor_scale.h"
#include "scene/resources/environment.h"

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
	if (p_path == properties[0]) {
		add_property_editor_for_multiple_properties(String(), properties, memnew(EnvironmentColorGradingEditor));
		return true;
	}
	return properties.has(p_path);
}

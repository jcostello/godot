/**************************************************************************/
/*  environment_color_grading_editor.h                                  */
/**************************************************************************/

#pragma once

#include "editor/inspector/editor_inspector.h"
#include "scene/gui/button.h"
#include "scene/gui/slider.h"

class EnvironmentColorGradingEditor;
class EnvironmentTonalRangesEditor;

class EnvironmentColorGradingTrackballControl : public Control {
	GDCLASS(EnvironmentColorGradingTrackballControl, Control);

	static constexpr int TRACKBALL_COUNT = 4;
	EnvironmentColorGradingEditor *editor = nullptr;
	HSlider *luminance_sliders[TRACKBALL_COUNT] = {};
	Button *reset_buttons[TRACKBALL_COUNT] = {};
	Color colors[TRACKBALL_COUNT];
	int dragging_trackball = -1;
	bool updating = false;

	bool _is_vertical_layout() const;
	Rect2 _get_trackball_rect(int p_index) const;
	void _set_trackball_from_position(int p_index, const Vector2 &p_position, bool p_changing);
	void _luminance_changed(double p_value, int p_index);
	void _reset_trackball(int p_index);

protected:
	void _notification(int p_what);
	void gui_input(const Ref<InputEvent> &p_event) override;

public:
	void set_values(const Color p_colors[TRACKBALL_COUNT], const float p_luminances[TRACKBALL_COUNT]);
	EnvironmentColorGradingTrackballControl(EnvironmentColorGradingEditor *p_editor);
};

class EnvironmentColorGradingEditor : public EditorProperty {
	GDCLASS(EnvironmentColorGradingEditor, EditorProperty);

	friend class EnvironmentColorGradingTrackballControl;

	static constexpr int TRACKBALL_COUNT = 4;
	static const char *color_properties[TRACKBALL_COUNT];
	static const char *luminance_properties[TRACKBALL_COUNT];
	EnvironmentColorGradingTrackballControl *trackballs = nullptr;

public:
	void update_property() override;
	void set_trackball_color(int p_index, const Color &p_color, bool p_changing);
	void set_trackball_luminance(int p_index, float p_luminance);
	EnvironmentColorGradingEditor();
};

class EnvironmentTonalRangesControl : public Control {
	GDCLASS(EnvironmentTonalRangesControl, Control);

	EnvironmentTonalRangesEditor *editor = nullptr;
	float midtones_start = 0.45f;
	float midtones_end = 0.55f;
	int dragging_handle = -1;

	float _value_to_x(float p_value) const;
	float _x_to_value(float p_x) const;
	Rect2 _bar_rect() const;
	void _set_handles_from_position(int p_handle, const Vector2 &p_position, bool p_changing);

protected:
	void _notification(int p_what);
	void gui_input(const Ref<InputEvent> &p_event) override;

public:
	void set_values(float p_midtones_start, float p_midtones_end);
	EnvironmentTonalRangesControl(EnvironmentTonalRangesEditor *p_editor);
};

class EnvironmentTonalRangesEditor : public EditorProperty {
	GDCLASS(EnvironmentTonalRangesEditor, EditorProperty);

	friend class EnvironmentTonalRangesControl;

	EnvironmentTonalRangesControl *ranges_control = nullptr;

public:
	void update_property() override;
	void set_tonal_ranges(float p_midtones_start, float p_midtones_end, bool p_changing);
	EnvironmentTonalRangesEditor();
};

class EditorInspectorEnvironmentColorGradingPlugin : public EditorInspectorPlugin {
	GDCLASS(EditorInspectorEnvironmentColorGradingPlugin, EditorInspectorPlugin);

public:
	bool can_handle(Object *p_object) override;
	bool parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide = false) override;
};

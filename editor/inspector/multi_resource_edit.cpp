/**************************************************************************/
/*  multi_resource_edit.cpp                                               */
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

#include "multi_resource_edit.h"

#include "core/io/resource_loader.h"
#include "core/math/math_fieldwise.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"

void MultiResourceEdit::_ensure_resources_loaded() const {
	if (resources_loaded) {
		return;
	}
	resources_loaded = true;
	loaded_resources.clear();
	for (const String &path : resource_paths) {
		Ref<Resource> resource = ResourceLoader::load(path);
		if (resource.is_valid()) {
			resource->connect(CoreStringName(property_list_changed), callable_mp(const_cast<MultiResourceEdit *>(this), &MultiResourceEdit::_queue_notify_property_list_changed));
			loaded_resources.push_back(resource);
		}
	}
}

bool MultiResourceEdit::_set(const StringName &p_name, const Variant &p_value) {
	return _set_impl(p_name, p_value, "");
}

bool MultiResourceEdit::_set_impl(const StringName &p_name, const Variant &p_value, const String &p_field, bool p_undo_redo) {
	String name = p_name;
	if (name == "scripts") {
		name = "script";
	} else if (name.begins_with("Metadata/")) {
		name = name.replace_first("Metadata/", "metadata/");
	}

	_ensure_resources_loaded();
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (p_undo_redo) {
		undo_redo->create_action(vformat(TTR("Set %s on %d resources"), name, loaded_resources.size()), UndoRedo::MERGE_ENDS);
	}

	for (const Ref<Resource> &resource : loaded_resources) {
		Variant new_value = p_field.is_empty() ? p_value : fieldwise_assign(resource->get(name), p_value, p_field);
		if (p_undo_redo) {
			undo_redo->add_do_property(resource.ptr(), name, new_value);
			undo_redo->add_undo_property(resource.ptr(), name, resource->get(name));
		} else {
			resource->set(name, new_value);
		}
	}

	if (p_undo_redo) {
		undo_redo->commit_action();
	}
	return true;
}

bool MultiResourceEdit::_get(const StringName &p_name, Variant &r_ret) const {
	String name = p_name;
	if (name == "scripts") {
		name = "script";
	} else if (name.begins_with("Metadata/")) {
		name = name.replace_first("Metadata/", "metadata/");
	}

	_ensure_resources_loaded();
	for (const Ref<Resource> &resource : loaded_resources) {
		bool found = false;
		r_ret = resource->get(name, &found);
		if (found) {
			return true;
		}
	}
	return false;
}

void MultiResourceEdit::_get_property_list(List<PropertyInfo> *p_list) const {
	HashMap<String, PLData> usage;
	List<PLData *> data_list;
	_ensure_resources_loaded();

	for (const Ref<Resource> &resource : loaded_resources) {
		List<PropertyInfo> properties;
		resource->get_property_list(&properties, true);
		for (PropertyInfo property : properties) {
			if (property.name == "script") {
				continue;
			} else if (property.name.begins_with("metadata/")) {
				property.name = property.name.replace_first("metadata/", "Metadata/");
			}

			PLData *usage_data = usage.getptr(property.name);
			if (!usage_data) {
				PLData data;
				data.info = property;
				HashMap<String, PLData>::Iterator iterator = usage.insert(property.name, data);
				usage_data = &iterator->value;
				data_list.push_back(usage_data);
			}
			if (usage_data->info == property) {
				usage_data->uses++;
			}
		}
	}

	for (const PLData *data : data_list) {
		if (data->uses == loaded_resources.size()) {
			p_list->push_back(data->info);
		}
	}
	p_list->push_back(PropertyInfo(Variant::OBJECT, "scripts", PROPERTY_HINT_RESOURCE_TYPE, Script::get_class_static()));
}

String MultiResourceEdit::_get_editor_name() const {
	return vformat(TTR("%s (%d Selected)"), get_edited_class_name(), get_resource_count());
}

bool MultiResourceEdit::_property_can_revert(const StringName &p_name) const {
	_ensure_resources_loaded();
	return !loaded_resources.is_empty() && ClassDB::has_property(get_edited_class_name(), p_name);
}

bool MultiResourceEdit::_property_get_revert(const StringName &p_name, Variant &r_property) const {
	_ensure_resources_loaded();
	for (const Ref<Resource> &resource : loaded_resources) {
		if (resource->property_can_revert(p_name)) {
			r_property = resource->property_get_revert(p_name);
		} else {
			r_property = ClassDB::class_get_default_property_value(resource->get_class_name(), p_name);
		}
		return true;
	}
	return false;
}

void MultiResourceEdit::_queue_notify_property_list_changed() {
	if (notify_property_list_changed_pending) {
		return;
	}
	notify_property_list_changed_pending = true;
	callable_mp(this, &MultiResourceEdit::_notify_property_list_changed).call_deferred();
}

void MultiResourceEdit::_notify_property_list_changed() {
	notify_property_list_changed_pending = false;
	notify_property_list_changed();
}

void MultiResourceEdit::add_resource(const String &p_path) {
	ERR_FAIL_COND(resources_loaded);
	resource_paths.push_back(p_path);
}

int MultiResourceEdit::get_resource_count() const {
	return resource_paths.size();
}

String MultiResourceEdit::get_resource_path(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, get_resource_count(), String());
	return resource_paths[p_index];
}

Ref<Resource> MultiResourceEdit::get_resource(int p_index) const {
	_ensure_resources_loaded();
	ERR_FAIL_INDEX_V(p_index, loaded_resources.size(), Ref<Resource>());
	return loaded_resources[p_index];
}

StringName MultiResourceEdit::get_edited_class_name() const {
	_ensure_resources_loaded();
	if (loaded_resources.is_empty()) {
		return SNAME("Resource");
	}

	StringName class_name = loaded_resources[0]->get_class_name();
	bool check_again = true;
	while (check_again) {
		check_again = false;
		if (class_name.is_empty() || class_name == SNAME("Resource")) {
			return SNAME("Resource");
		}
		for (const Ref<Resource> &resource : loaded_resources) {
			const StringName resource_class = resource->get_class_name();
			if (class_name == resource_class || ClassDB::is_parent_class(resource_class, class_name)) {
				continue;
			}
			class_name = ClassDB::get_parent_class(class_name);
			check_again = true;
			break;
		}
	}
	return class_name;
}

void MultiResourceEdit::set_property_field(const StringName &p_property, const Variant &p_value, const String &p_field) {
	const Variant::Type type = p_value.get_type();
	_set_impl(p_property, p_value, type == Variant::ARRAY || type == Variant::DICTIONARY ? String() : p_field);
}

void MultiResourceEdit::_bind_methods() {
	ClassDB::bind_method("_hide_script_from_inspector", &MultiResourceEdit::_hide_script_from_inspector);
	ClassDB::bind_method("_hide_metadata_from_inspector", &MultiResourceEdit::_hide_metadata_from_inspector);
	ClassDB::bind_method("_get_editor_name", &MultiResourceEdit::_get_editor_name);
}

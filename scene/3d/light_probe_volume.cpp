/**************************************************************************/
/*  light_probe_volume.cpp                                                */
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

#include "light_probe_volume.h"

#include "light_probe_geometry.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/lightmap_gi.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/rendering_server.h"

Dictionary LightProbeVolumeData::get_layout() const {
	Dictionary result;
	result["positions"] = positions;
	result["ids"] = probe_ids;
	result["next_id"] = next_id;
	result["grid_origins"] = grid_origins.duplicate();
	return result;
}

void LightProbeVolumeData::set_layout(const Dictionary &p_layout) {
	ERR_FAIL_COND(!p_layout.has("positions") || !p_layout.has("ids") || !p_layout.has("next_id"));
	ERR_FAIL_COND(p_layout["positions"].get_type() != Variant::PACKED_VECTOR3_ARRAY || p_layout["ids"].get_type() != Variant::PACKED_INT32_ARRAY || p_layout["next_id"].get_type() != Variant::INT);
	const PackedVector3Array new_positions = p_layout["positions"];
	const PackedInt32Array new_ids = p_layout["ids"];
	const int64_t new_next_id = p_layout["next_id"];
	ERR_FAIL_COND(new_positions.size() != new_ids.size() || new_positions.size() > MAX_PROBES);
	ERR_FAIL_COND(new_next_id < 0 || new_next_id > INT32_MAX);
	HashMap<int, int> new_indices;
	new_indices.reserve(new_ids.size());
	for (int i = 0; i < new_ids.size(); i++) {
		ERR_FAIL_COND(!new_positions[i].is_finite() || new_ids[i] < 0 || new_ids[i] >= new_next_id || new_indices.has(new_ids[i]));
		new_indices.insert(new_ids[i], i);
	}
	Dictionary new_origins;
	if (p_layout.has("grid_origins")) {
		ERR_FAIL_COND(p_layout["grid_origins"].get_type() != Variant::DICTIONARY);
		const Dictionary supplied_origins = p_layout["grid_origins"];
		for (const Variant &key : supplied_origins.keys()) {
			ERR_FAIL_COND(key.get_type() != Variant::INT || supplied_origins[key].get_type() != Variant::VECTOR3);
			const int64_t id = key;
			const Vector3 origin = supplied_origins[key];
			ERR_FAIL_COND(id < 0 || id >= INT32_MAX || !origin.is_finite());
			if (new_indices.has(int(id))) {
				new_origins[id] = origin;
			}
		}
	}
	if (positions == new_positions && probe_ids == new_ids && grid_origins == new_origins) {
		next_id = MAX(next_id, new_next_id);
		return;
	}
	positions = new_positions;
	probe_ids = new_ids;
	probe_indices = std::move(new_indices);
	grid_origins = new_origins;
	// Undo restores placement, never the ID allocator or revision clock.
	next_id = MAX(next_id, new_next_id);
	revision++;
	emit_changed();
}

int LightProbeVolumeData::find_probe(int p_id) const {
	const int *index = probe_indices.getptr(p_id);
	return index ? *index : -1;
}

bool LightProbeVolumeData::is_custom_probe(int p_id) const {
	const int index = find_probe(p_id);
	if (index < 0) {
		return false;
	}
	if (!grid_origins.has(p_id)) {
		return true;
	}
	const Vector3 origin = grid_origins[p_id];
	return origin != positions[index];
}

bool LightProbeVolumeData::has_custom_probes() const {
	for (int id : probe_ids) {
		if (is_custom_probe(id)) {
			return true;
		}
	}
	return false;
}

Vector3 LightProbeVolumeData::get_probe_position(int p_id) const {
	const int index = find_probe(p_id);
	ERR_FAIL_COND_V(index < 0, Vector3());
	return positions[index];
}

void LightProbeVolumeData::set_probe_position(int p_id, Vector3 p_position) {
	ERR_FAIL_COND(!p_position.is_finite());
	const int index = find_probe(p_id);
	ERR_FAIL_COND(index < 0);
	if (positions[index] == p_position) {
		return;
	}
	positions.set(index, p_position);
	revision++;
	emit_changed();
}

int LightProbeVolumeData::add_probe(Vector3 p_position) {
	ERR_FAIL_COND_V(!p_position.is_finite() || positions.size() >= MAX_PROBES || next_id >= INT32_MAX, -1);
	const int id = next_id++;
	probe_indices.insert(id, positions.size());
	positions.push_back(p_position);
	probe_ids.push_back(id);
	revision++;
	emit_changed();
	return id;
}

void LightProbeVolumeData::remove_probe(int p_id) {
	const int index = find_probe(p_id);
	ERR_FAIL_COND(index < 0);
	positions.remove_at(index);
	probe_ids.remove_at(index);
	probe_indices.erase(p_id);
	grid_origins.erase(p_id);
	for (int i = index; i < probe_ids.size(); i++) {
		probe_indices[probe_ids[i]] = i;
	}
	revision++;
	emit_changed();
}

void LightProbeVolumeData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_layout"), &LightProbeVolumeData::get_layout);
	ClassDB::bind_method(D_METHOD("set_layout", "layout"), &LightProbeVolumeData::set_layout);
	ClassDB::bind_method(D_METHOD("get_positions"), &LightProbeVolumeData::get_positions);
	ClassDB::bind_method(D_METHOD("get_probe_ids"), &LightProbeVolumeData::get_probe_ids);
	ClassDB::bind_method(D_METHOD("get_probe_count"), &LightProbeVolumeData::get_probe_count);
	ClassDB::bind_method(D_METHOD("find_probe", "id"), &LightProbeVolumeData::find_probe);
	ClassDB::bind_method(D_METHOD("is_custom_probe", "id"), &LightProbeVolumeData::is_custom_probe);
	ClassDB::bind_method(D_METHOD("has_custom_probes"), &LightProbeVolumeData::has_custom_probes);
	ClassDB::bind_method(D_METHOD("get_probe_position", "id"), &LightProbeVolumeData::get_probe_position);
	ClassDB::bind_method(D_METHOD("set_probe_position", "id", "position"), &LightProbeVolumeData::set_probe_position);
	ClassDB::bind_method(D_METHOD("add_probe", "position"), &LightProbeVolumeData::add_probe);
	ClassDB::bind_method(D_METHOD("remove_probe", "id"), &LightProbeVolumeData::remove_probe);
	ClassDB::bind_method(D_METHOD("get_revision"), &LightProbeVolumeData::get_revision);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "_layout", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_layout", "get_layout");
	BIND_CONSTANT(MAX_PROBES);
}

LightProbeVolumeData::LightProbeVolumeData() {
	set_local_to_scene(true);
}

LightProbeVolume::ValidationStatus LightProbeVolume::get_probe_validation_status(int p_id) const {
	if (probe_data.is_null() || probe_data->find_probe(p_id) < 0) {
		return VALIDATION_UNCHECKED;
	}
	const int *status = validation_results.getptr(p_id);
	return status ? ValidationStatus(*status) : VALIDATION_UNCHECKED;
}

String LightProbeVolume::get_probe_validation_reason(int p_id) const {
	switch (get_probe_validation_status(p_id)) {
		case VALIDATION_UNCHECKED:
			return RTR("Not validated since placement changed.");
		case VALIDATION_VALID:
			return validation_stale ? RTR("Valid at the last check; bake again to refresh.") : RTR("Valid for baking.");
		case VALIDATION_INSIDE_GEOMETRY:
			return RTR("Inside closed geometry.");
		case VALIDATION_TOO_CLOSE:
			return RTR("On or too close to a static surface.");
		case VALIDATION_AMBIGUOUS:
			return RTR("Interior test was ambiguous; excluded from baking.");
	}
	return String();
}

String LightProbeVolume::get_validation_summary() const {
	const int total = probe_data.is_valid() ? probe_data->get_probe_count() : 0;
	int valid = 0;
	int inside = 0;
	int close = 0;
	int ambiguous = 0;
	int checked = 0;
	if (probe_data.is_valid()) {
		for (int id : probe_data->get_probe_ids()) {
			const ValidationStatus status = get_probe_validation_status(id);
			checked += status != VALIDATION_UNCHECKED;
			valid += status == VALIDATION_VALID;
			inside += status == VALIDATION_INSIDE_GEOMETRY;
			close += status == VALIDATION_TOO_CLOSE;
			ambiguous += status == VALIDATION_AMBIGUOUS;
		}
	}
	const String summary = vformat(RTR("Probes: %d | Valid: %d | Inside: %d | Near surface: %d | Ambiguous: %d | Unchecked: %d"), total, valid, inside, close, ambiguous, total - checked);
	return validation_stale ? summary + " | " + RTR("Last validation; rebake to refresh") : summary;
}

void LightProbeVolume::clear_validation() {
	validation_results.clear();
	validation_stale = false;
	emit_signal(SNAME("validation_changed"));
}

void LightProbeVolume::mark_validation_stale() {
	validation_stale = !validation_results.is_empty();
	update_gizmos();
	update_configuration_warnings();
	emit_signal(SNAME("validation_changed"));
}

void LightProbeVolume::apply_validation(const PackedInt32Array &p_ids, const PackedInt32Array &p_results) {
	ERR_FAIL_COND(probe_data.is_null() || p_ids.size() != p_results.size());
	ERR_FAIL_COND(p_ids != probe_data->get_probe_ids());
	HashMap<int, int> results;
	for (int i = 0; i < p_ids.size(); i++) {
		ERR_FAIL_COND(p_results[i] < VALIDATION_VALID || p_results[i] > VALIDATION_AMBIGUOUS);
		results.insert(p_ids[i], p_results[i]);
	}
	validation_results = std::move(results);
	validation_stale = false;
	update_gizmos();
	update_configuration_warnings();
	emit_signal(SNAME("validation_changed"));
}

void LightProbeVolume::_data_changed() {
	// Keep the last result by stable ID, including for Undo/Redo. Geometry is
	// revalidated at the next bake; edits must not hide unrelated invalid probes.
	mark_validation_stale();
	if (LightProbeSystem *system = ObjectDB::get_instance<LightProbeSystem>(registered_system)) {
		system->_placement_changed();
	}
}

void LightProbeVolume::set_data(const Ref<LightProbeVolumeData> &p_data) {
	if (probe_data == p_data) {
		return;
	}
	const Callable changed = callable_mp(this, &LightProbeVolume::_data_changed);
	if (probe_data.is_valid()) {
		probe_data->disconnect_changed(changed);
	}
	clear_validation();
	probe_data = p_data;
	if (probe_data.is_valid()) {
		probe_data->connect_changed(changed);
	}
	_data_changed();
}

void LightProbeVolume::set_size(Vector3 p_size) {
	ERR_FAIL_COND(!p_size.is_finite() || p_size.x <= 0 || p_size.y <= 0 || p_size.z <= 0);
	size = p_size;
	update_gizmos();
	update_configuration_warnings();
}

void LightProbeVolume::set_spacing(real_t p_spacing) {
	ERR_FAIL_COND(!Math::is_finite(p_spacing) || p_spacing <= 0);
	spacing = p_spacing;
	update_gizmos();
	update_configuration_warnings();
}

void LightProbeVolume::set_enabled(bool p_enabled) {
	if (enabled == p_enabled) {
		return;
	}
	enabled = p_enabled;
	_data_changed();
}

LightProbeSystem *LightProbeVolume::get_system() const {
	for (Node *parent = get_parent(); parent; parent = parent->get_parent()) {
		if (LightProbeSystem *system = Object::cast_to<LightProbeSystem>(parent)) {
			return system;
		}
	}
	return nullptr;
}

void LightProbeVolume::_update_system() {
	LightProbeSystem *previous = ObjectDB::get_instance<LightProbeSystem>(registered_system);
	LightProbeSystem *system = is_inside_tree() ? get_system() : nullptr;
	if (previous == system) {
		if (system) {
			system->_placement_changed();
		}
		return;
	}
	if (previous) {
		previous->_unregister_volume(this);
	}
	registered_system = system ? system->get_instance_id() : ObjectID();
	if (system) {
		system->_register_volume(this);
	}
	update_configuration_warnings();
}

Dictionary LightProbeVolume::create_grid_layout(bool p_preserve_custom) const {
	Vector3i intervals;
	uint64_t count = 1;
	for (int axis = 0; axis < 3; axis++) {
		const double steps = Math::ceil(double(size[axis]) / spacing);
		ERR_FAIL_COND_V(steps >= double(LightProbeVolumeData::MAX_PROBES), Dictionary());
		// Thin axes contribute one centered position instead of two boundaries.
		intervals[axis] = size[axis] < spacing ? 0 : int(steps);
		count *= uint64_t(intervals[axis] + 1);
		ERR_FAIL_COND_V(count > LightProbeVolumeData::MAX_PROBES, Dictionary());
	}
	HashMap<Vector3, int> previous_ids;
	HashSet<Vector3> preserved_positions;
	HashSet<Vector3> moved_origins;
	PackedVector3Array positions;
	PackedInt32Array ids;
	Dictionary origins;
	int64_t next_id = 0;
	if (probe_data.is_valid()) {
		const PackedVector3Array old_positions = probe_data->get_positions();
		const PackedInt32Array old_ids = probe_data->get_probe_ids();
		const Dictionary old_layout = probe_data->get_layout();
		const Dictionary old_origins = old_layout["grid_origins"];
		for (int i = 0; i < old_positions.size(); i++) {
			if (p_preserve_custom && probe_data->is_custom_probe(old_ids[i])) {
				positions.push_back(old_positions[i]);
				ids.push_back(old_ids[i]);
				preserved_positions.insert(old_positions[i]);
				if (old_origins.has(old_ids[i])) {
					origins[old_ids[i]] = old_origins[old_ids[i]];
					const Vector3 origin = old_origins[old_ids[i]];
					moved_origins.insert(origin);
				}
			} else {
				previous_ids.insert(old_positions[i], old_ids[i]);
			}
		}
		next_id = probe_data->get_next_id();
	}
	for (int z = 0; z <= intervals.z; z++) {
		for (int y = 0; y <= intervals.y; y++) {
			for (int x = 0; x <= intervals.x; x++) {
				const Vector3 point(
						intervals.x == 0 ? 0 : -size.x * 0.5 + size.x * x / intervals.x,
						intervals.y == 0 ? 0 : -size.y * 0.5 + size.y * y / intervals.y,
						intervals.z == 0 ? 0 : -size.z * 0.5 + size.z * z / intervals.z);
				// A moved probe replaces its original grid slot. Do not regenerate
				// a second probe at that slot or duplicate a preserved custom point.
				if (preserved_positions.has(point) || moved_origins.has(point)) {
					continue;
				}
				ERR_FAIL_COND_V(positions.size() >= LightProbeVolumeData::MAX_PROBES, Dictionary());
				int id;
				if (const int *previous = previous_ids.getptr(point)) {
					id = *previous;
				} else {
					ERR_FAIL_COND_V(next_id >= INT32_MAX, Dictionary());
					id = next_id++;
				}
				positions.push_back(point);
				ids.push_back(id);
				origins[id] = point;
			}
		}
	}
	Dictionary result;
	result["positions"] = positions;
	result["ids"] = ids;
	result["next_id"] = next_id;
	result["grid_origins"] = origins;
	return result;
}

Dictionary LightProbeVolume::create_filtered_grid_layout(Node *p_geometry_root, bool p_preserve_custom, Lightmapper::BakeStepFunc p_step, void *p_userdata) const {
	ERR_FAIL_COND_V(!is_inside_tree() || !p_geometry_root, Dictionary());
	const Vector3 generation_size = size;
	const real_t generation_spacing = spacing;
	Dictionary layout = create_grid_layout(p_preserve_custom);
	if (layout.is_empty()) {
		return Dictionary();
	}
	const Ref<LightProbeVolumeData> original_data = probe_data;
	const uint64_t original_revision = original_data.is_valid() ? original_data->get_revision() : 0;
	const Transform3D transform = get_global_transform();
	ERR_FAIL_COND_V(!transform.is_finite() || Math::is_zero_approx(transform.basis.determinant()), Dictionary());
	LightProbeSystem *system = get_system();
	const real_t clearance = system ? system->get_surface_clearance() : real_t(0.05);
	LightProbeGeometry geometry;
	if (geometry.build(p_geometry_root, p_step, p_userdata) != OK) {
		return Dictionary();
	}
	const PackedVector3Array candidates = layout["positions"];
	const PackedInt32Array candidate_ids = layout["ids"];
	Dictionary origins = layout["grid_origins"];
	PackedVector3Array positions;
	PackedInt32Array ids;
	int rejected = 0;
	for (int i = 0; i < candidates.size(); i++) {
		if (p_step && i % 64 == 0 && p_step(0.2 + 0.79 * double(i) / MAX(candidates.size(), 1), RTR("Filtering probe grid against geometry"), p_userdata, false)) {
			return Dictionary();
		}
		const int id = candidate_ids[i];
		const bool preserve = p_preserve_custom && original_data.is_valid() && original_data->is_custom_probe(id);
		if (!preserve && geometry.validate(transform.xform(candidates[i]), clearance) != VALIDATION_VALID) {
			origins.erase(id);
			rejected++;
			continue;
		}
		positions.push_back(candidates[i]);
		ids.push_back(id);
	}
	// Progress callbacks may pump the editor. Never commit a stale snapshot.
	if (size != generation_size || spacing != generation_spacing || probe_data != original_data || (original_data.is_valid() && original_data->get_revision() != original_revision) || get_global_transform() != transform || get_system() != system || (system && system->get_surface_clearance() != clearance)) {
		return Dictionary();
	}
	layout["positions"] = positions;
	layout["ids"] = ids;
	layout["grid_origins"] = origins;
	print_line(vformat("Light probe grid: candidates=%d, kept=%d, rejected_by_geometry=%d, preserved_custom=%s.", candidates.size(), positions.size(), rejected, p_preserve_custom ? "yes" : "no"));
	return layout;
}

void LightProbeVolume::generate_grid(bool p_preserve_custom) {
	Dictionary layout;
	if (is_inside_tree()) {
		Node *root = get_tree()->get_edited_scene_root();
		if (!root) {
			root = get_tree()->get_current_scene();
		}
		if (!root) {
			root = this;
			while (root->get_parent()) {
				root = root->get_parent();
			}
		}
		layout = create_filtered_grid_layout(root, p_preserve_custom);
	} else {
		layout = create_grid_layout(p_preserve_custom);
	}
	if (layout.is_empty()) {
		return;
	}
	if (probe_data.is_null()) {
		Ref<LightProbeVolumeData> new_data;
		new_data.instantiate();
		set_data(new_data);
	}
	probe_data->set_layout(layout);
}

PackedStringArray LightProbeVolume::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (enabled && !get_system()) {
		warnings.push_back(RTR("Place this volume below a LightProbeSystem in the scene hierarchy."));
	}
	if (enabled && is_inside_tree() && Math::is_zero_approx(get_global_transform().basis.determinant())) {
		warnings.push_back(RTR("The probe volume transform must be invertible."));
	}
	if (enabled && (probe_data.is_null() || probe_data->get_probe_count() == 0)) {
		warnings.push_back(RTR("This volume has no probe positions. Use Generate Grid or add individual probes."));
	}
	if (probe_data.is_valid()) {
		for (int id : probe_data->get_probe_ids()) {
			if (get_probe_validation_status(id) > VALIDATION_VALID) {
				warnings.push_back(RTR("Red probes failed the last validation. Results remain visible after edits and are checked again at the next bake.") + "\n" + get_validation_summary());
				break;
			}
		}
	}
	return warnings;
}

void LightProbeVolume::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		// Register before the system's deferred layout check. Otherwise a scene
		// load can compare the saved bake against an incomplete volume list.
		_update_system();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		if (LightProbeSystem *system = ObjectDB::get_instance<LightProbeSystem>(registered_system)) {
			system->_unregister_volume(this);
		}
		registered_system = ObjectID();
	} else if (p_what == NOTIFICATION_PATH_RENAMED) {
		_update_system();
	} else if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
		_data_changed();
	}
}

void LightProbeVolume::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_probe_validation_status", "id"), &LightProbeVolume::get_probe_validation_status);
	ClassDB::bind_method(D_METHOD("get_probe_validation_reason", "id"), &LightProbeVolume::get_probe_validation_reason);
	ClassDB::bind_method(D_METHOD("get_validation_summary"), &LightProbeVolume::get_validation_summary);
	BIND_ENUM_CONSTANT(VALIDATION_UNCHECKED);
	BIND_ENUM_CONSTANT(VALIDATION_VALID);
	BIND_ENUM_CONSTANT(VALIDATION_INSIDE_GEOMETRY);
	BIND_ENUM_CONSTANT(VALIDATION_TOO_CLOSE);
	BIND_ENUM_CONSTANT(VALIDATION_AMBIGUOUS);
	ADD_SIGNAL(MethodInfo("validation_changed"));
	ClassDB::bind_method(D_METHOD("set_data", "data"), &LightProbeVolume::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &LightProbeVolume::get_data);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &LightProbeVolume::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &LightProbeVolume::get_size);
	ClassDB::bind_method(D_METHOD("set_spacing", "spacing"), &LightProbeVolume::set_spacing);
	ClassDB::bind_method(D_METHOD("get_spacing"), &LightProbeVolume::get_spacing);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &LightProbeVolume::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &LightProbeVolume::is_enabled);
	ClassDB::bind_method(D_METHOD("get_system"), &LightProbeVolume::get_system);
	ClassDB::bind_method(D_METHOD("create_grid_layout", "preserve_custom"), &LightProbeVolume::create_grid_layout, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("generate_grid", "preserve_custom"), &LightProbeVolume::generate_grid, DEFVAL(false));
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_RANGE, "0.01,10000,0.01,or_greater,suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spacing", PROPERTY_HINT_RANGE, "0.01,1000,0.01,or_greater,suffix:m"), "set_spacing", "get_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "data", PROPERTY_HINT_RESOURCE_TYPE, "LightProbeVolumeData", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_ALWAYS_DUPLICATE | PROPERTY_USAGE_EDITOR_INSTANTIATE_OBJECT), "set_data", "get_data");
}

LightProbeVolume::LightProbeVolume() {
	set_notify_transform(true);
}

LightProbeVolume::~LightProbeVolume() {
	if (probe_data.is_valid()) {
		probe_data->disconnect_changed(callable_mp(this, &LightProbeVolume::_data_changed));
	}
}

void LightProbeSystem::set_surface_clearance(real_t p_clearance) {
	ERR_FAIL_COND(!Math::is_finite(p_clearance) || p_clearance < 0);
	if (surface_clearance == p_clearance) {
		return;
	}
	surface_clearance = p_clearance;
	for (ObjectID id : volumes) {
		if (LightProbeVolume *volume = ObjectDB::get_instance<LightProbeVolume>(id)) {
			volume->mark_validation_stale();
		}
	}
	_placement_changed();
}

Error LightProbeSystem::prepare_bake(LightProbeGeometry &p_geometry, const Transform3D &p_bake_transform, Vector<Vector3> &r_points, Lightmapper::BakeStepFunc p_step, void *p_userdata) {
	ERR_FAIL_COND_V(!is_inside_tree() || !p_bake_transform.is_finite() || Math::is_zero_approx(p_bake_transform.basis.determinant()), ERR_INVALID_DATA);
	const Dictionary layout = get_probe_layout();
	ERR_FAIL_COND_V(layout.is_empty(), ERR_INVALID_DATA);
	const PackedVector3Array positions = layout["positions"];
	const PackedInt32Array ids = layout["ids"];
	const PackedInt32Array source_indices = layout["volume_indices"];
	const TypedArray<NodePath> paths = layout["volumes"];
	const Transform3D transform = get_global_transform();
	const Transform3D bake_inverse = p_bake_transform.affine_inverse();
	const uint64_t expected_revision = revision;
	Vector<PackedInt32Array> source_ids;
	Vector<PackedInt32Array> source_results;
	source_ids.resize(paths.size());
	source_results.resize(paths.size());
	Vector<Vector3> accepted;
	accepted.reserve(positions.size());
	int rejected = 0;
	for (int i = 0; i < positions.size(); i++) {
		if (p_step && i % 64 == 0 && p_step(0.15 + 0.04 * double(i) / MAX(positions.size(), 1), vformat(RTR("Validating probes %d/%d"), i, positions.size()), p_userdata, false)) {
			return ERR_SKIP;
		}
		if (revision != expected_revision || transform != get_global_transform()) {
			return ERR_SKIP;
		}
		const Vector3 world = transform.xform(positions[i]);
		const LightProbeVolume::ValidationStatus status = p_geometry.validate(world, surface_clearance);
		source_ids.write[source_indices[i]].push_back(ids[i]);
		source_results.write[source_indices[i]].push_back(status);
		if (status == LightProbeVolume::VALIDATION_VALID) {
			accepted.push_back(bake_inverse.xform(world));
		} else {
			rejected++;
		}
	}
	if (revision != expected_revision || transform != get_global_transform()) {
		return ERR_SKIP;
	}
	// Publish complete validation only. Gizmo/status updates never edit layout.
	for (int i = 0; i < paths.size(); i++) {
		const NodePath path = paths[i];
		LightProbeVolume *volume = Object::cast_to<LightProbeVolume>(get_node_or_null(path));
		ERR_FAIL_NULL_V(volume, ERR_SKIP);
		if (volume->get_data().is_valid()) {
			volume->apply_validation(source_ids[i], source_results[i]);
		}
		if (revision != expected_revision || transform != get_global_transform()) {
			return ERR_SKIP;
		}
	}
	print_line(vformat("Light probe validation: system=%s, total=%d, accepted=%d, rejected=%d.", get_path(), positions.size(), accepted.size(), rejected));
	r_points = accepted;
	return OK;
}

void LightProbeSystem::_placement_changed() {
	revision++;
	if (!update_queued && is_inside_tree()) {
		update_queued = true;
		callable_mp(this, &LightProbeSystem::_emit_placement_changed).call_deferred();
	}
}

void LightProbeSystem::_emit_placement_changed() {
	update_queued = false;
	if (is_inside_tree()) {
		if (baked_data.is_valid() && !baked_data->is_stale() && has_layout_signature && _get_layout_signature() != baked_layout_signature) {
			baked_data->set_stale(true);
		}
		update_configuration_warnings();
		emit_signal(SNAME("placement_changed"));
	}
}

void LightProbeSystem::_register_volume(LightProbeVolume *p_volume) {
	volumes.insert(p_volume->get_instance_id());
	_placement_changed();
}

void LightProbeSystem::_unregister_volume(LightProbeVolume *p_volume) {
	volumes.erase(p_volume->get_instance_id());
	_placement_changed();
}

TypedArray<NodePath> LightProbeSystem::get_volumes() const {
	TypedArray<NodePath> result;
	if (!is_inside_tree()) {
		return result;
	}
	Vector<String> sorted_paths;
	for (ObjectID id : volumes) {
		const LightProbeVolume *volume = ObjectDB::get_instance<LightProbeVolume>(id);
		if (volume && volume->is_inside_tree() && volume->is_enabled() && volume->get_system() == this) {
			sorted_paths.push_back(String(get_path_to(volume)));
		}
	}
	sorted_paths.sort();
	for (const String &path : sorted_paths) {
		result.push_back(NodePath(path));
	}
	return result;
}

Dictionary LightProbeSystem::get_probe_layout() const {
	ERR_FAIL_COND_V(!is_inside_tree(), Dictionary());
	ERR_FAIL_COND_V(Math::is_zero_approx(get_global_transform().basis.determinant()), Dictionary());
	PackedVector3Array positions;
	PackedInt32Array ids;
	PackedInt32Array volume_indices;
	const Transform3D inverse = get_global_transform().affine_inverse();
	const TypedArray<NodePath> paths = get_volumes();
	for (int i = 0; i < paths.size(); i++) {
		const NodePath path = paths[i];
		const LightProbeVolume *volume = Object::cast_to<LightProbeVolume>(get_node_or_null(path));
		if (!volume || volume->get_data().is_null()) {
			continue;
		}
		const Transform3D transform = inverse * volume->get_global_transform();
		ERR_FAIL_COND_V(!transform.is_finite() || Math::is_zero_approx(transform.basis.determinant()), Dictionary());
		const PackedVector3Array local_positions = volume->get_data()->get_positions();
		const PackedInt32Array local_ids = volume->get_data()->get_probe_ids();
		for (int point = 0; point < local_positions.size(); point++) {
			positions.push_back(transform.xform(local_positions[point]));
			ids.push_back(local_ids[point]);
			volume_indices.push_back(i);
		}
	}
	Dictionary result;
	result["positions"] = positions;
	result["ids"] = ids;
	result["volumes"] = paths;
	result["volume_indices"] = volume_indices;
	result["revision"] = revision;
	return result;
}

PackedStringArray LightProbeSystem::get_configuration_warnings() const {
	PackedStringArray warnings = VisualInstance3D::get_configuration_warnings();
	if (!get_bake_source()) {
		warnings.push_back(RTR("LightProbeSystem must be a descendant of LightmapGI. Run the bake from that LightmapGI."));
	}
	if (is_inside_tree() && get_volumes().is_empty()) {
		warnings.push_back(RTR("This system has no enabled probe volumes. Add LightProbeVolume descendants to this system."));
	}
	if (is_inside_tree() && Math::is_zero_approx(get_global_transform().basis.determinant())) {
		warnings.push_back(RTR("The probe system transform must be invertible."));
	}
	if (baked_data.is_valid() && baked_data->is_stale()) {
		warnings.push_back(RTR("The probe lighting is out of date. Bake probes or lightmap + probes to refresh dynamic-object lighting."));
	}
	return warnings;
}

void LightProbeSystem::_notification(int p_what) {
#ifdef TOOLS_ENABLED
	if (p_what == NOTIFICATION_EDITOR_PRE_SAVE && baked_data.is_valid() && baked_data->get_path().get_extension() == "probebake") {
		// Embed legacy captures on the next scene save. Older editors cannot select
		// a loader for .probebake, but can preserve an unknown embedded resource.
		// Leave the original bake file untouched.
		baked_data->set_path(String());
	}
#endif
	if (p_what == NOTIFICATION_ENTER_TREE) {
		update_configuration_warnings();
		_placement_changed();
		callable_mp(this, &LightProbeSystem::_refresh_capture_instance).call_deferred();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		set_base(RID());
	} else if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
		_placement_changed();
		_refresh_capture_instance();
	} else if (p_what == NOTIFICATION_VISIBILITY_CHANGED || p_what == NOTIFICATION_ENTER_WORLD) {
		callable_mp(this, &LightProbeSystem::_refresh_capture_instance).call_deferred();
	}
}

void LightProbeSystem::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_light_data", "data"), &LightProbeSystem::set_light_data);
	ClassDB::bind_method(D_METHOD("get_light_data"), &LightProbeSystem::get_light_data);
	ClassDB::bind_method(D_METHOD("set_bake_state", "state"), &LightProbeSystem::set_bake_state);
	ClassDB::bind_method(D_METHOD("get_bake_state"), &LightProbeSystem::get_bake_state);
	ClassDB::bind_method(D_METHOD("clear_probe_cache"), &LightProbeSystem::clear_probe_cache);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "light_data", PROPERTY_HINT_RESOURCE_TYPE, "LightProbeData"), "set_light_data", "get_light_data");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "_bake_state", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_INTERNAL), "set_bake_state", "get_bake_state");
	ClassDB::bind_method(D_METHOD("get_bake_source"), &LightProbeSystem::get_bake_source);
	ClassDB::bind_method(D_METHOD("set_surface_clearance", "clearance"), &LightProbeSystem::set_surface_clearance);
	ClassDB::bind_method(D_METHOD("get_surface_clearance"), &LightProbeSystem::get_surface_clearance);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "surface_clearance", PROPERTY_HINT_RANGE, "0,1,0.001,or_greater,suffix:m"), "set_surface_clearance", "get_surface_clearance");
	ClassDB::bind_method(D_METHOD("get_volumes"), &LightProbeSystem::get_volumes);
	ClassDB::bind_method(D_METHOD("get_probe_layout"), &LightProbeSystem::get_probe_layout);
	ClassDB::bind_method(D_METHOD("get_revision"), &LightProbeSystem::get_revision);
	ADD_SIGNAL(MethodInfo("placement_changed"));
	ADD_SIGNAL(MethodInfo("bake_updated"));
}

LightProbeSystem::LightProbeSystem() {
	set_notify_transform(true);
	set_instance_use_identity_transform(true);
}

bool LightProbeSystem::_set(const StringName &p_name, const Variant &p_value) {
	// Older scenes stored an explicit source. Hierarchy is now authoritative.
	return p_name == "_bake_source";
}

LightmapGI *LightProbeSystem::get_bake_source() const {
	for (Node *ancestor = get_parent(); ancestor; ancestor = ancestor->get_parent()) {
		if (LightmapGI *source = Object::cast_to<LightmapGI>(ancestor)) {
			return source;
		}
	}
	return nullptr;
}

void LightProbeSystem::set_light_data(const Ref<LightProbeData> &p_data) {
	if (baked_data == p_data) {
		_baked_data_changed();
		return;
	}
	if (baked_data.is_valid()) {
		baked_data->disconnect_changed(callable_mp(this, &LightProbeSystem::_baked_data_changed));
		set_base(RID());
	}
	baked_data = p_data;
	if (baked_data.is_valid()) {
		baked_data->connect_changed(callable_mp(this, &LightProbeSystem::_baked_data_changed));
	}
	_baked_data_changed();
}

void LightProbeSystem::_baked_data_changed() {
	_refresh_capture_instance();
	update_configuration_warnings();
	for (const NodePath path : get_volumes()) {
		if (LightProbeVolume *volume = Object::cast_to<LightProbeVolume>(get_node_or_null(path))) {
			volume->update_gizmos();
		}
	}
	emit_signal(SNAME("bake_updated"));
}

Transform3D LightProbeSystem::get_capture_global_transform() const {
	return get_global_transform() * capture_transform;
}

void LightProbeSystem::set_capture_transform(const Transform3D &p_transform) {
	ERR_FAIL_COND(!p_transform.is_finite() || Math::is_zero_approx(p_transform.basis.determinant()));
	capture_transform = p_transform;
	_refresh_capture_instance();
}

void LightProbeSystem::_refresh_capture_instance() {
	if (!is_inside_tree()) {
		return;
	}
	const Transform3D transform = get_capture_global_transform();
	const bool has_capture = baked_data.is_valid() && is_visible_in_tree() && get_world_3d().is_valid() && get_bake_source() && transform.is_finite() && !Math::is_zero_approx(transform.basis.determinant());
	// Refresh bounds and cached SH even when retaining the resource RID/BVH.
	set_base(RID());
	if (!has_capture) {
		return;
	}
	const RID capture = baked_data->get_rid();
	if (baked_data->get_probe_count() > 0) {
		set_base(capture);
		RenderingServer::get_singleton()->instance_set_transform(get_instance(), transform);
	}
}

AABB LightProbeSystem::get_aabb() const {
	return baked_data.is_valid() ? capture_transform.xform(baked_data->get_bounds()) : AABB();
}

uint32_t LightProbeSystem::_get_layout_signature() const {
	Dictionary layout = get_probe_layout();
	if (layout.is_empty()) {
		return 0;
	}
	layout.erase("revision");
	layout["surface_clearance"] = surface_clearance;
	PackedVector3Array positions = layout["positions"];
	Transform3D to_capture = capture_transform.affine_inverse();
	if (const LightmapGI *source = get_bake_source()) {
		if (!Math::is_zero_approx(source->get_global_transform().basis.determinant())) {
			to_capture = source->get_global_transform().affine_inverse() * get_global_transform();
		}
	}
	for (int i = 0; i < positions.size(); i++) {
		positions.set(i, to_capture.xform(positions[i]).snapped(Vector3(0.0001, 0.0001, 0.0001)));
	}
	layout["positions"] = positions;
	return layout.hash();
}

void LightProbeSystem::mark_layout_baked() {
	baked_layout_signature = _get_layout_signature();
	has_layout_signature = true;
	_baked_data_changed();
}

Dictionary LightProbeSystem::get_bake_state() const {
	Dictionary state;
	state["capture_transform"] = capture_transform;
	state["layout_signature"] = baked_layout_signature;
	state["has_layout_signature"] = has_layout_signature;
	return state;
}

void LightProbeSystem::set_bake_state(const Dictionary &p_state) {
	const Transform3D transform = p_state.get("capture_transform", Transform3D());
	ERR_FAIL_COND(!transform.is_finite() || Math::is_zero_approx(transform.basis.determinant()));
	capture_transform = transform;
	baked_layout_signature = p_state.get("layout_signature", 0);
	has_layout_signature = p_state.get("has_layout_signature", false);
	_baked_data_changed();
}

void LightProbeSystem::clear_probe_cache() {
	if (baked_data.is_valid()) {
		baked_data->clear_cache();
	}
}

LightProbeSystem::~LightProbeSystem() {
	if (baked_data.is_valid()) {
		baked_data->disconnect_changed(callable_mp(this, &LightProbeSystem::_baked_data_changed));
	}
}

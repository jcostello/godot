/**************************************************************************/
/*  lightmapper_rd_geometry.cpp                                           */
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

#include "lightmapper_rd.h"

#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/os/mutex.h"
#include "core/string/print_string.h"
#include "servers/rendering/rendering_context_driver.h"

namespace {
struct CachedBLAS {
	RID blas;
	RID vertices;
	RID indices;
};
struct BakeGeometryCache {
	Mutex mutex;
	RenderingDevice *device = nullptr;
	ObjectID renderer;
	bool busy = false;
	HashMap<String, CachedBLAS> entries;
};
BakeGeometryCache geometry_cache;

void free_cached_blas(RenderingDevice *p_rd, const CachedBLAS &p_entry) {
	for (RID rid : { p_entry.blas, p_entry.indices, p_entry.vertices }) {
		if (rid.is_valid()) {
			p_rd->free_rid(rid);
		}
	}
}

struct BLASInput {
	Vector<float> positions;
	Vector<uint32_t> indices;
	uint32_t counts[2] = {};
};

String geometry_key(const BLASInput &p_input) {
	uint8_t digests[72];
	uint8_t digest[32];
	CryptoCore::sha256(reinterpret_cast<const uint8_t *>(p_input.positions.ptr()), p_input.positions.size() * sizeof(float), digests);
	CryptoCore::sha256(reinterpret_cast<const uint8_t *>(p_input.indices.ptr()), p_input.indices.size() * sizeof(uint32_t), digests + 32);
	memcpy(digests + 64, p_input.counts, sizeof(p_input.counts));
	CryptoCore::sha256(digests, sizeof(digests), digest);
	return String::hex_encode_buffer(digest, sizeof(digest));
}
} // namespace

RenderingDevice *LightmapperRD::take_cached_bake_device() {
	MutexLock lock(geometry_cache.mutex);
	RenderingDevice *renderer = RenderingDevice::get_singleton();
	if (geometry_cache.device && !geometry_cache.busy && renderer && renderer->get_instance_id() == geometry_cache.renderer) {
		geometry_cache.busy = true;
		return geometry_cache.device;
	}
	return nullptr;
}

void LightmapperRD::remember_bake_device(RenderingDevice *p_device) {
	MutexLock lock(geometry_cache.mutex);
	if (!geometry_cache.device) {
		geometry_cache.device = p_device;
		geometry_cache.renderer = RenderingDevice::get_singleton()->get_instance_id();
		geometry_cache.busy = true;
	}
}

void LightmapperRD::release_bake_device(RenderingDevice *p_device) {
	if (!p_device) {
		return;
	}
	MutexLock lock(geometry_cache.mutex);
	if (geometry_cache.device != p_device) {
		memdelete(p_device);
		return;
	}
	Vector<RID> keep;
	keep.resize(geometry_cache.entries.size() * 3);
	RID *keep_write = keep.ptrw();
	int keep_index = 0;
	for (const KeyValue<String, CachedBLAS> &entry : geometry_cache.entries) {
		keep_write[keep_index++] = entry.value.blas;
		keep_write[keep_index++] = entry.value.vertices;
		keep_write[keep_index++] = entry.value.indices;
	}
	if (p_device->local_device_clear_transient_resources(keep) != OK) {
		// A recording failure must not leave a partially active device reusable.
		geometry_cache.entries.clear();
		geometry_cache.device = nullptr;
		memdelete(p_device);
	}
	geometry_cache.busy = false;
}

void LightmapperRD::clear_bake_cache() {
	MutexLock lock(geometry_cache.mutex);
	ERR_FAIL_COND(geometry_cache.busy);
	if (geometry_cache.device) {
		geometry_cache.device->make_current();
		for (const KeyValue<String, CachedBLAS> &entry : geometry_cache.entries) {
			free_cached_blas(geometry_cache.device, entry.value);
		}
		geometry_cache.entries.clear();
		memdelete(geometry_cache.device);
		geometry_cache.device = nullptr;
		geometry_cache.renderer = ObjectID();
	}
}

LightmapperRD::BakeDeviceGuard::~BakeDeviceGuard() {
	if (*device) {
		LightmapperRD::release_bake_device(*device);
		*device = nullptr;
	}
	if (*context) {
		memdelete(*context);
		*context = nullptr;
	}
}

void LightmapperRD::RaytracingResources::clear(RenderingDevice *p_rd) {
	for (RID *rid : { &tlas, &triangle_map_buffer }) {
		if (rid->is_valid()) {
			p_rd->free_rid(*rid);
			*rid = RID();
		}
	}
	for (RID rid : owned_blases) {
		p_rd->free_rid(rid);
	}
	for (RID rid : owned_inputs) {
		p_rd->free_rid(rid);
	}
	owned_blases.clear();
	owned_inputs.clear();
}

void LightmapperRD::_create_raytracing_acceleration_structure(RenderingDevice *p_rd, Span<Triangle> p_triangles, RaytracingResources &r_resources) {
	MutexLock lock(geometry_cache.mutex);
	const bool persistent = geometry_cache.device == p_rd;
	if (!bool(GLOBAL_GET("rendering/lightmapping/bake_performance/use_hardware_raytracing")) || !p_rd->has_feature(RD::SUPPORTS_RAY_QUERY) || p_triangles.is_empty() || mesh_instances.size() > 0xFFFFFF) {
		if (persistent) {
			for (const KeyValue<String, CachedBLAS> &entry : geometry_cache.entries) {
				free_cached_blas(p_rd, entry.value);
			}
			geometry_cache.entries.clear();
		}
		return;
	}

	Vector<String> mesh_keys;
	Vector<Transform3D> transforms;
	HashMap<String, BLASInput> inputs;
	Vector<Vector<uint32_t>> sorted_triangles;
	mesh_keys.resize(mesh_instances.size());
	transforms.resize(mesh_instances.size());
	sorted_triangles.resize(mesh_instances.size());
	Vector<uint32_t *> triangle_map_writes;
	triangle_map_writes.resize(mesh_instances.size());
	for (int mesh = 0; mesh < mesh_instances.size(); mesh++) {
		const MeshData &data = mesh_instances[mesh].data;
		const bool local = data.rt_points.size() == data.points.size() && !Math::is_zero_approx(data.rt_transform.basis.determinant());
		const Vector<Vector3> &points = local ? data.rt_points : data.points;
		transforms.write[mesh] = local ? data.rt_transform : Transform3D();
		BLASInput input;
		input.positions.resize(points.size() * 3);
		float *position_write = input.positions.ptrw();
		for (int vertex = 0; vertex < points.size(); vertex++) {
			for (int axis = 0; axis < 3; axis++) {
				position_write[vertex * 3 + axis] = points[vertex][axis];
			}
		}
		const int triangle_count = points.size() / 3;
		input.indices.resize(triangle_count * 3);
		uint32_t *index_write = input.indices.ptrw();
		uint32_t index_count = 0;
		for (int group = 0; group < 2; group++) {
			for (int triangle = 0; triangle < triangle_count; triangle++) {
				const bool cutout = !data.alpha_scissor_threshold.is_empty() && data.alpha_scissor_threshold[triangle] >= 0.0f;
				if (cutout != (group == 1)) {
					continue;
				}
				for (int corner = 0; corner < 3; corner++) {
					index_write[index_count++] = triangle * 3 + corner;
				}
				input.counts[group] += 3;
			}
		}
		DEV_ASSERT(index_count == uint32_t(input.indices.size()));
		const String key = geometry_key(input);
		mesh_keys.write[mesh] = key;
		if (!inputs.has(key)) {
			// Cached BLAS still need indices/counts for the hit-to-atlas map,
			// but their CPU vertex upload is no longer needed after hashing.
			if (persistent && geometry_cache.entries.has(key)) {
				input.positions.clear();
			}
			inputs.insert(key, std::move(input));
		}
		sorted_triangles.write[mesh].resize(triangle_count);
		triangle_map_writes.write[mesh] = sorted_triangles.write[mesh].ptrw();
	}
	for (uint32_t triangle = 0; triangle < p_triangles.size(); triangle++) {
		const Triangle &data = p_triangles[triangle];
		triangle_map_writes[data.mesh - 1][data.source_triangle] = triangle;
	}

	// Keep only geometry used by this bake. Removed meshes and topology/cutout
	// changes evict stale entries before allocating their replacements.
	if (persistent) {
		Vector<String> stale;
		for (const KeyValue<String, CachedBLAS> &entry : geometry_cache.entries) {
			if (!inputs.has(entry.key)) {
				stale.push_back(entry.key);
			}
		}
		for (const String &key : stale) {
			free_cached_blas(p_rd, geometry_cache.entries[key]);
			geometry_cache.entries.erase(key);
		}
	}
	HashMap<String, CachedBLAS> transient_entries;
	HashMap<String, CachedBLAS> &entries = persistent ? geometry_cache.entries : transient_entries;
	const BitField<RD::BufferCreationBits> flags = RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT | RD::BUFFER_CREATION_AS_STORAGE_BIT;
	int built = 0;
	int reused = 0;
	for (const KeyValue<String, BLASInput> &entry : inputs) {
		if (entries.has(entry.key)) {
			reused++;
			continue;
		}
		const BLASInput &input = entry.value;
		CachedBLAS cached;
		cached.vertices = p_rd->vertex_buffer_create(input.positions.size() * sizeof(float), input.positions.span().reinterpret<uint8_t>(), flags);
		cached.indices = p_rd->index_buffer_create(input.indices.size(), RD::INDEX_BUFFER_FORMAT_UINT32, input.indices.span().reinterpret<uint8_t>(), false, flags);
		if (cached.vertices.is_valid()) {
			r_resources.owned_inputs.push_back(cached.vertices);
		}
		if (cached.indices.is_valid()) {
			r_resources.owned_inputs.push_back(cached.indices);
		}
		if (cached.vertices.is_null() || cached.indices.is_null()) {
			r_resources.clear(p_rd);
			WARN_PRINT("Lightmapper: per-mesh RT input allocation failed; using compute tracing.");
			return;
		}
		Vector<RD::AccelerationStructureGeometry> geometries;
		uint32_t offset = 0;
		for (int group = 0; group < 2; group++) {
			if (input.counts[group] == 0) {
				continue;
			}
			RD::AccelerationStructureGeometry geometry;
			if (group == 0) {
				geometry.flags = RD::ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT;
			}
			geometry.vertex_buffer = cached.vertices;
			geometry.vertex_stride = 3 * sizeof(float);
			geometry.vertex_count = input.positions.size() / 3;
			geometry.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
			geometry.index_buffer = cached.indices;
			geometry.index_offset = offset * sizeof(uint32_t);
			geometry.index_count = input.counts[group];
			geometries.push_back(geometry);
			offset += input.counts[group];
		}
		cached.blas = p_rd->blas_create(geometries.span(), RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
		if (cached.blas.is_valid()) {
			r_resources.owned_blases.push_back(cached.blas);
		}
		if (cached.blas.is_null() || p_rd->blas_build(cached.blas) != OK) {
			r_resources.clear(p_rd);
			WARN_PRINT("Lightmapper: per-mesh BLAS setup failed; using compute tracing.");
			return;
		}
		entries.insert(entry.key, cached);
		if (persistent) {
			r_resources.owned_blases.remove_at(r_resources.owned_blases.size() - 1);
			r_resources.owned_inputs.resize(r_resources.owned_inputs.size() - 2);
		}
		built++;
	}

	Vector<uint32_t> triangle_ids;
	triangle_ids.resize(mesh_instances.size() * 3 + p_triangles.size());
	uint32_t *triangle_id_write = triangle_ids.ptrw();
	uint32_t triangle_id_count = mesh_instances.size();
	Vector<RD::AccelerationStructureInstance> instances;
	instances.resize(mesh_instances.size());
	for (int mesh = 0; mesh < mesh_instances.size(); mesh++) {
		const BLASInput &input = inputs[mesh_keys[mesh]];
		const uint32_t base = triangle_id_count;
		triangle_id_write[mesh] = base;
		triangle_id_write[triangle_id_count++] = 0;
		triangle_id_write[triangle_id_count++] = 0;
		uint32_t offset = 0;
		uint32_t geometry = 0;
		for (int group = 0; group < 2; group++) {
			if (input.counts[group] == 0) {
				continue;
			}
			triangle_id_write[base + geometry++] = triangle_id_count;
			for (uint32_t index = 0; index < input.counts[group]; index += 3) {
				const uint32_t local_triangle = input.indices[offset + index] / 3;
				triangle_id_write[triangle_id_count++] = sorted_triangles[mesh][local_triangle];
			}
			offset += input.counts[group];
		}
		RD::AccelerationStructureInstance instance;
		instance.blas = entries[mesh_keys[mesh]].blas;
		instance.transform = transforms[mesh];
		instance.id = mesh;
		instance.flags = RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT;
		instances.write[mesh] = instance;
	}
	DEV_ASSERT(triangle_id_count == uint32_t(triangle_ids.size()));
	r_resources.triangle_map_buffer = p_rd->storage_buffer_create(triangle_ids.size() * sizeof(uint32_t), triangle_ids.span().reinterpret<uint8_t>());
	r_resources.tlas = p_rd->tlas_create(instances.size(), RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
	if (r_resources.triangle_map_buffer.is_null() || r_resources.tlas.is_null() || p_rd->tlas_build(r_resources.tlas, instances.span(), true) != OK) {
		r_resources.clear(p_rd);
		WARN_PRINT("Lightmapper: instanced TLAS setup failed; using compute tracing.");
		return;
	}
	print_line(vformat("Lightmapper RT geometry: %d instances, %d unique BLAS, %d built, %d reused from previous bake; persistent=%s.", instances.size(), inputs.size(), built, reused, persistent ? "yes" : "no"));
}

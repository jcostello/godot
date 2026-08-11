/**************************************************************************/
/*  lightmapper_rd.cpp                                                    */
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

#include "lightmap_seam.h"
#include "lm_blendseams.glsl.gen.h"
#include "lm_compute.glsl.gen.h"
#include "lm_raster.glsl.gen.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/math/dynamic_bvh.h"
#include "core/math/geometry_2d.h"
#include "core/math/geometry_3d.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "editor/file_system/editor_paths.h"
#include "editor/settings/editor_settings.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"

#if defined(VULKAN_ENABLED)
#include "drivers/vulkan/rendering_context_driver_vulkan.h"
#endif
#if defined(METAL_ENABLED)
#include "drivers/metal/rendering_context_driver_metal.h"
#endif

//uncomment this if you want to see textures from all the process saved
//#define DEBUG_TEXTURES

static uint32_t _float_to_ufloat_bits(float p_value, uint32_t p_mantissa_bits) {
	const uint32_t half = Math::make_half_float(MAX(p_value, 0.0f)) & 0x7fff;
	const uint32_t shift = 10 - p_mantissa_bits;
	// Round the half-float mantissa to nearest-even while retaining its exponent.
	return (half + ((1u << (shift - 1)) - 1u) + ((half >> shift) & 1u)) >> shift;
}

static Vector<uint8_t> _pack_r11g11b10f(const Ref<Image> &p_image) {
	Ref<Image> image = p_image;
	if (image->get_format() != Image::FORMAT_RGBAH) {
		image = p_image->duplicate();
		image->convert(Image::FORMAT_RGBAH);
	}
	const Vector<uint8_t> source = image->get_data();
	const uint16_t *source_half = reinterpret_cast<const uint16_t *>(source.ptr());
	Vector<uint8_t> packed;
	packed.resize(image->get_width() * image->get_height() * sizeof(uint32_t));
	uint32_t *destination = reinterpret_cast<uint32_t *>(packed.ptrw());
	for (int64_t i = 0; i < image->get_width() * image->get_height(); i++) {
		const float r = Math::half_to_float(source_half[i * 4 + 0]);
		const float g = Math::half_to_float(source_half[i * 4 + 1]);
		const float b = Math::half_to_float(source_half[i * 4 + 2]);
		destination[i] = _float_to_ufloat_bits(r, 6) | (_float_to_ufloat_bits(g, 6) << 11) | (_float_to_ufloat_bits(b, 5) << 22);
	}
	return packed;
}

static RenderingDevice *_create_lightmapper_device(RenderingContextDriver *&r_context_driver) {
	RenderingDevice *rd = RenderingServer::get_singleton()->create_local_rendering_device();
	if (rd != nullptr) {
		return rd;
	}

#if defined(METAL_ENABLED)
	r_context_driver = memnew(RenderingContextDriverMetal);
	rd = memnew(RenderingDevice);
#endif
#if defined(VULKAN_ENABLED)
	if (r_context_driver == nullptr) {
		r_context_driver = memnew(RenderingContextDriverVulkan);
		rd = memnew(RenderingDevice);
	}
#endif
	if (r_context_driver != nullptr && rd != nullptr) {
		Error err = r_context_driver->initialize();
		if (err == OK) {
			err = rd->initialize(r_context_driver);
		}
		if (err != OK) {
			memdelete(rd);
			memdelete(r_context_driver);
			rd = nullptr;
			r_context_driver = nullptr;
		}
	}
	return rd;
}

void LightmapperRD::add_mesh(const MeshData &p_mesh) {
	ERR_FAIL_COND(p_mesh.lightmap_size.x <= 0 || p_mesh.lightmap_size.y <= 0);
	ERR_FAIL_COND(p_mesh.points.is_empty());
	MeshInstance mi;
	mi.data = p_mesh;
	mesh_instances.push_back(mi);
}

void LightmapperRD::add_directional_light(const String &p_name, bool p_static, const Vector3 &p_direction, const Color &p_color, float p_energy, float p_indirect_energy, float p_angular_distance, float p_shadow_blur) {
	Light l;
	l.type = LIGHT_TYPE_DIRECTIONAL;
	l.direction[0] = p_direction.x;
	l.direction[1] = p_direction.y;
	l.direction[2] = p_direction.z;
	l.color[0] = p_color.r;
	l.color[1] = p_color.g;
	l.color[2] = p_color.b;
	l.energy = p_energy;
	l.indirect_energy = p_indirect_energy;
	l.static_bake = p_static;
	l.size = Math::tan(Math::deg_to_rad(p_angular_distance));
	l.shadow_blur = p_shadow_blur;
	lights.push_back(l);

	LightMetadata md;
	md.name = p_name;
	md.type = LIGHT_TYPE_DIRECTIONAL;
	light_metadata.push_back(md);
}

void LightmapperRD::add_omni_light(const String &p_name, bool p_static, const Vector3 &p_position, const Color &p_color, float p_energy, float p_indirect_energy, float p_range, float p_attenuation, float p_size, float p_shadow_blur) {
	Light l;
	l.type = LIGHT_TYPE_OMNI;
	l.position[0] = p_position.x;
	l.position[1] = p_position.y;
	l.position[2] = p_position.z;
	l.range = p_range;
	l.attenuation = p_attenuation;
	l.color[0] = p_color.r;
	l.color[1] = p_color.g;
	l.color[2] = p_color.b;
	l.energy = p_energy;
	l.indirect_energy = p_indirect_energy;
	l.static_bake = p_static;
	l.size = p_size;
	l.shadow_blur = p_shadow_blur;
	lights.push_back(l);

	LightMetadata md;
	md.name = p_name;
	md.type = LIGHT_TYPE_OMNI;
	light_metadata.push_back(md);
}

void LightmapperRD::add_spot_light(const String &p_name, bool p_static, const Vector3 &p_position, const Vector3 &p_direction, const Color &p_color, float p_energy, float p_indirect_energy, float p_range, float p_attenuation, float p_spot_angle, float p_spot_attenuation, float p_size, float p_shadow_blur) {
	Light l;
	l.type = LIGHT_TYPE_SPOT;
	l.position[0] = p_position.x;
	l.position[1] = p_position.y;
	l.position[2] = p_position.z;
	l.direction[0] = p_direction.x;
	l.direction[1] = p_direction.y;
	l.direction[2] = p_direction.z;
	l.range = p_range;
	l.attenuation = p_attenuation;
	l.cos_spot_angle = Math::cos(Math::deg_to_rad(p_spot_angle));
	l.inv_spot_attenuation = 1.0f / p_spot_attenuation;
	l.color[0] = p_color.r;
	l.color[1] = p_color.g;
	l.color[2] = p_color.b;
	l.energy = p_energy;
	l.indirect_energy = p_indirect_energy;
	l.static_bake = p_static;
	l.size = p_size;
	l.shadow_blur = p_shadow_blur;
	lights.push_back(l);

	LightMetadata md;
	md.name = p_name;
	md.type = LIGHT_TYPE_SPOT;
	light_metadata.push_back(md);
}

void LightmapperRD::add_area_light(const String &p_name, bool p_static, const Vector3 &p_position, const Vector3 &p_direction, const Color &p_color, float p_energy, float p_indirect_energy, float p_range, float p_attenuation, const Vector3 &p_area_width, const Vector3 &p_area_height, float p_size, float p_shadow_blur, const Rect2 &p_texture_rect, float p_max_mipmap) {
	Light l;
	l.type = LIGHT_TYPE_AREA;
	l.position[0] = p_position.x;
	l.position[1] = p_position.y;
	l.position[2] = p_position.z;
	l.direction[0] = p_direction.x;
	l.direction[1] = p_direction.y;
	l.direction[2] = p_direction.z;
	l.area_width[0] = p_area_width.x;
	l.area_width[1] = p_area_width.y;
	l.area_width[2] = p_area_width.z;
	l.area_height[0] = p_area_height.x;
	l.area_height[1] = p_area_height.y;
	l.area_height[2] = p_area_height.z;
	l.range = p_range;
	l.attenuation = p_attenuation;
	l.color[0] = p_color.r;
	l.color[1] = p_color.g;
	l.color[2] = p_color.b;
	l.energy = p_energy;
	l.indirect_energy = p_indirect_energy;
	l.static_bake = p_static;
	l.size = p_size;
	l.shadow_blur = p_shadow_blur;

	if (RenderingServer::get_singleton()->get_current_rendering_method() == "gl_compatibility") {
		// area light textures unsupported in compat
		l.area_texture_rect[0] = 0.0;
		l.area_texture_rect[1] = 0.0;
		l.area_texture_rect[2] = 0.0;
		l.area_texture_rect[3] = 0.0;
	} else {
		l.area_texture_rect[0] = p_texture_rect.position.x;
		l.area_texture_rect[1] = p_texture_rect.position.y;
		l.area_texture_rect[2] = p_texture_rect.size.x;
		l.area_texture_rect[3] = p_texture_rect.size.y;
	}
	l.cos_spot_angle = p_max_mipmap;
	lights.push_back(l);

	LightMetadata md;
	md.name = p_name;
	md.type = LIGHT_TYPE_AREA;
	light_metadata.push_back(md);
}

void LightmapperRD::add_area_light_atlas(const Vector2i &p_size, int p_mipmap_count, const PackedByteArray &p_atlas_data) {
	area_light_atlas.mipmap_count = p_mipmap_count;
	area_light_atlas.size = p_size;
	area_light_atlas.atlas_data = p_atlas_data;
}

void LightmapperRD::add_probe(const Vector3 &p_position) {
	Probe probe;
	probe.position[0] = p_position.x;
	probe.position[1] = p_position.y;
	probe.position[2] = p_position.z;
	probe.position[3] = 0;
	probe_positions.push_back(probe);
}

void LightmapperRD::_plot_triangle_into_triangle_index_list(int p_size, const Vector3i &p_ofs, const AABB &p_bounds, const Vector3 p_points[3], uint32_t p_triangle_index, LocalVector<TriangleSort> &p_triangles_sort, uint32_t p_grid_size) {
	int half_size = p_size / 2;

	for (int i = 0; i < 8; i++) {
		AABB aabb = p_bounds;
		aabb.size *= 0.5;
		Vector3i n = p_ofs;

		if (i & 1) {
			aabb.position.x += aabb.size.x;
			n.x += half_size;
		}
		if (i & 2) {
			aabb.position.y += aabb.size.y;
			n.y += half_size;
		}
		if (i & 4) {
			aabb.position.z += aabb.size.z;
			n.z += half_size;
		}

		{
			Vector3 qsize = aabb.size * 0.5; //quarter size, for fast aabb test

			if (!Geometry3D::triangle_box_overlap(aabb.position + qsize, qsize, p_points)) {
				//does not fit in child, go on
				continue;
			}
		}

		if (half_size == 1) {
			//got to the end
			TriangleSort ts;
			ts.cell_index = n.x + (n.y * p_grid_size) + (n.z * p_grid_size * p_grid_size);
			ts.triangle_index = p_triangle_index;
			ts.triangle_aabb.position = p_points[0];
			ts.triangle_aabb.size = Vector3();
			ts.triangle_aabb.expand_to(p_points[1]);
			ts.triangle_aabb.expand_to(p_points[2]);
			p_triangles_sort.push_back(ts);
		} else {
			_plot_triangle_into_triangle_index_list(half_size, n, aabb, p_points, p_triangle_index, p_triangles_sort, p_grid_size);
		}
	}
}

void LightmapperRD::_sort_triangle_clusters(uint32_t p_cluster_size, uint32_t p_cluster_index, uint32_t p_index_start, uint32_t p_count, LocalVector<TriangleSort> &p_triangle_sort, LocalVector<ClusterAABB> &p_cluster_aabb) {
	if (p_count == 0) {
		return;
	}

	// Compute AABB for all triangles in the range.
	SortArray<TriangleSort, TriangleSortAxis<0>> triangle_sorter_x;
	SortArray<TriangleSort, TriangleSortAxis<1>> triangle_sorter_y;
	SortArray<TriangleSort, TriangleSortAxis<2>> triangle_sorter_z;
	AABB cluster_aabb = p_triangle_sort[p_index_start].triangle_aabb;
	for (uint32_t i = 1; i < p_count; i++) {
		cluster_aabb.merge_with(p_triangle_sort[p_index_start + i].triangle_aabb);
	}

	if (p_count > p_cluster_size) {
		int longest_axis_index = cluster_aabb.get_longest_axis_index();
		switch (longest_axis_index) {
			case 0:
				triangle_sorter_x.sort(&p_triangle_sort[p_index_start], p_count);
				break;
			case 1:
				triangle_sorter_y.sort(&p_triangle_sort[p_index_start], p_count);
				break;
			case 2:
				triangle_sorter_z.sort(&p_triangle_sort[p_index_start], p_count);
				break;
			default:
				DEV_ASSERT(false && "Invalid axis returned by AABB.");
				break;
		}

		uint32_t left_cluster_count = Math::next_power_of_2(p_count / 2);
		left_cluster_count = MAX(left_cluster_count, p_cluster_size);
		left_cluster_count = MIN(left_cluster_count, p_count);
		_sort_triangle_clusters(p_cluster_size, p_cluster_index, p_index_start, left_cluster_count, p_triangle_sort, p_cluster_aabb);

		if (left_cluster_count < p_count) {
			uint32_t cluster_index_right = p_cluster_index + (left_cluster_count / p_cluster_size);
			_sort_triangle_clusters(p_cluster_size, cluster_index_right, p_index_start + left_cluster_count, p_count - left_cluster_count, p_triangle_sort, p_cluster_aabb);
		}
	} else {
		ClusterAABB &aabb = p_cluster_aabb[p_cluster_index];
		Vector3 aabb_end = cluster_aabb.get_end();
		aabb.min_bounds[0] = cluster_aabb.position.x;
		aabb.min_bounds[1] = cluster_aabb.position.y;
		aabb.min_bounds[2] = cluster_aabb.position.z;
		aabb.max_bounds[0] = aabb_end.x;
		aabb.max_bounds[1] = aabb_end.y;
		aabb.max_bounds[2] = aabb_end.z;
	}
}

Lightmapper::BakeError LightmapperRD::_blit_meshes_into_atlas(RenderingDevice *p_rd, RID &r_albedo_texture, RID &r_emission_texture, int p_max_texture_size, int p_denoiser_range, bool p_expand_for_oidn, AABB &bounds, Size2i &atlas_size, int &atlas_slices, float p_supersampling_factor, BakeStepFunc p_step_function, void *p_bake_userdata) {
	Vector<Size2i> sizes;
	const int oidn_padding = MAX(1, int(Math::ceil((p_denoiser_range + 3) * p_supersampling_factor)));

	for (int m_i = 0; m_i < mesh_instances.size(); m_i++) {
		MeshInstance &mi = mesh_instances.write[m_i];
		Size2i s = mi.data.lightmap_size;
		sizes.push_back(s);
		atlas_size = atlas_size.max(s + (p_expand_for_oidn ? Size2i(oidn_padding * 2, oidn_padding * 2) : Size2i(2, 2).maxi(p_denoiser_range) * p_supersampling_factor));
	}

	int max = Math::nearest_power_of_2_templated(atlas_size.width);
	max = MAX(max, Math::nearest_power_of_2_templated(atlas_size.height));

	if (max > p_max_texture_size) {
		return BAKE_ERROR_TEXTURE_EXCEEDS_MAX_SIZE;
	}

	if (p_step_function) {
		if (p_step_function(0.1, RTR("Determining optimal atlas size"), p_bake_userdata, true)) {
			return BAKE_ERROR_USER_ABORTED;
		}
	}

	atlas_size = Size2i(max, max);

	Size2i best_atlas_size;
	int best_atlas_slices = 0;
	int best_atlas_memory = 0x7FFFFFFF;
	Vector<Vector3i> best_atlas_offsets;

	// Determine best texture array atlas size by bruteforce fitting.
	while (atlas_size.x <= p_max_texture_size && atlas_size.y <= p_max_texture_size) {
		Vector<Vector2i> source_sizes;
		Vector<int> source_indices;
		source_sizes.resize(sizes.size());
		source_indices.resize(sizes.size());
		for (int i = 0; i < source_indices.size(); i++) {
			// Add padding between lightmaps.
			// Scale the padding if the lightmap will be downsampled at the end of the baking process
			// Otherwise the padding would be insufficient.
			source_sizes.write[i] = sizes[i] + (p_expand_for_oidn ? Vector2i(oidn_padding * 2, oidn_padding * 2) : Vector2i(2, 2).maxi(p_denoiser_range) * p_supersampling_factor);
			source_indices.write[i] = i;
		}
		Vector<Vector3i> atlas_offsets;
		atlas_offsets.resize(source_sizes.size());

		// Ensure the sizes can all fit into a single atlas layer.
		// This should always happen, and this check is only in place to prevent an infinite loop.
		for (int i = 0; i < source_sizes.size(); i++) {
			if (source_sizes[i] > atlas_size) {
				return BAKE_ERROR_ATLAS_TOO_SMALL;
			}
		}

		int slices = 0;

		while (source_sizes.size() > 0) {
			Vector<Vector3i> offsets = Geometry2D::partial_pack_rects(source_sizes, atlas_size);
			Vector<int> new_indices;
			Vector<Vector2i> new_sources;
			for (int i = 0; i < offsets.size(); i++) {
				Vector3i ofs = offsets[i];
				int sidx = source_indices[i];
				if (ofs.z > 0) {
					//valid
					ofs.z = slices;
					atlas_offsets.write[sidx] = ofs + Vector3i(p_expand_for_oidn ? oidn_padding : 1, p_expand_for_oidn ? oidn_padding : 1, 0);
				} else {
					new_indices.push_back(sidx);
					new_sources.push_back(source_sizes[i]);
				}
			}

			source_sizes = new_sources;
			source_indices = new_indices;
			slices++;
		}

		int mem_used = atlas_size.x * atlas_size.y * slices;
		if (mem_used < best_atlas_memory) {
			best_atlas_size = atlas_size;
			best_atlas_offsets = atlas_offsets;
			best_atlas_slices = slices;
			best_atlas_memory = mem_used;
		}

		if (atlas_size.width == atlas_size.height) {
			atlas_size.width *= 2;
		} else {
			atlas_size.height *= 2;
		}
	}
	atlas_size = best_atlas_size;
	atlas_slices = best_atlas_slices;

	if (p_step_function) {
		if (p_step_function(0.2, RTR("Blitting albedo and emission"), p_bake_userdata, true)) {
			return BAKE_ERROR_USER_ABORTED;
		}
	}

	// Assign UV positions before consuming the source images.
	for (int m_i = 0; m_i < mesh_instances.size(); m_i++) {
		MeshInstance &mi = mesh_instances.write[m_i];
		mi.offset.x = best_atlas_offsets[m_i].x;
		mi.offset.y = best_atlas_offsets[m_i].y;
		mi.slice = best_atlas_offsets[m_i].z;
	}

	RD::TextureFormat texture_format;
	texture_format.width = atlas_size.width;
	texture_format.height = atlas_size.height;
	texture_format.array_layers = atlas_slices;
	texture_format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	texture_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	texture_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	r_albedo_texture = p_rd->texture_create(texture_format, RD::TextureView());
	ERR_FAIL_COND_V(r_albedo_texture.is_null(), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);

	texture_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	texture_format.format = RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32;
	r_emission_texture = p_rd->texture_create(texture_format, RD::TextureView());
	ERR_FAIL_COND_V(r_emission_texture.is_null(), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);

	// Build one layer at a time. This avoids overlapping every source image with
	// every fully allocated atlas layer at the peak of CPU memory usage.
	int processed_meshes = 0;
	for (int slice = 0; slice < atlas_slices; slice++) {
		Ref<Image> albedo = Image::create_empty(atlas_size.width, atlas_size.height, false, Image::FORMAT_RGBA8);
		albedo->set_as_black();

		Ref<Image> emission = Image::create_empty(atlas_size.width, atlas_size.height, false, Image::FORMAT_RGBAH);
		emission->set_as_black();

		for (int m_i = 0; m_i < mesh_instances.size(); m_i++) {
			MeshInstance &mi = mesh_instances.write[m_i];
			if (mi.slice != slice) {
				continue;
			}
			if (mi.data.albedo_on_uv2.is_null() || mi.data.emission_on_uv2.is_null()) {
				ERR_FAIL_NULL_V(bake_material_func, BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
				if (p_step_function && p_step_function(0.1 + float(processed_meshes) / MAX(1, mesh_instances.size()) * 0.1, vformat(RTR("Preparing material %d/%d"), processed_meshes + 1, mesh_instances.size()), p_bake_userdata, false)) {
					return BAKE_ERROR_USER_ABORTED;
				}
				ERR_FAIL_COND_V(mi.data.material_index < 0, BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
				BakeError err = bake_material_func(mi.data.material_index, mi.data.lightmap_size, mi.data.albedo_on_uv2, mi.data.emission_on_uv2, bake_material_userdata);
				if (err != BAKE_OK) {
					return err;
				}
			}
			ERR_FAIL_COND_V(mi.data.albedo_on_uv2.is_null() || mi.data.albedo_on_uv2->is_empty(), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
			ERR_FAIL_COND_V(mi.data.emission_on_uv2.is_null() || mi.data.emission_on_uv2->is_empty(), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
			ERR_FAIL_COND_V(mi.data.albedo_on_uv2->get_size() != mi.data.lightmap_size, BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
			ERR_FAIL_COND_V(mi.data.emission_on_uv2->get_size() != mi.data.lightmap_size, BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
			albedo->blit_rect(mi.data.albedo_on_uv2, Rect2i(Vector2i(), mi.data.lightmap_size), mi.offset);
			emission->blit_rect(mi.data.emission_on_uv2, Rect2i(Vector2i(), mi.data.lightmap_size), mi.offset);
			mi.data.albedo_on_uv2.unref();
			mi.data.emission_on_uv2.unref();
			processed_meshes++;
		}

#ifdef DEBUG_TEXTURES
		albedo->save_png("res://0_albedo_" + itos(slice) + ".png");
		emission->save_png("res://0_emission_" + itos(slice) + ".png");
#endif
		ERR_FAIL_COND_V(p_rd->texture_update(r_albedo_texture, slice, albedo->get_data()) != OK, BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
		ERR_FAIL_COND_V(p_rd->texture_update(r_emission_texture, slice, _pack_r11g11b10f(emission)) != OK, BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
	}

	return BAKE_OK;
}

void LightmapperRD::_create_acceleration_structures(RenderingDevice *rd, Size2i atlas_size, int atlas_slices, AABB &bounds, int grid_size, uint32_t p_cluster_size, Vector<Probe> &p_probe_positions, GenerateProbes p_generate_probes, Vector<int> &slice_triangle_count, Vector<int> &slice_seam_count, Vector<Vector<int>> &r_slice_seam_sources, RID &vertex_buffer, RID &triangle_buffer, RID &lights_buffer, RID &r_triangle_indices_buffer, RID &r_cluster_indices_buffer, RID &r_cluster_aabbs_buffer, RID &probe_positions_buffer, RID &grid_texture, RID &seams_buffer, BakeStepFunc p_step_function, void *p_bake_userdata) {
	HashMap<Vertex, uint32_t, VertexHash> vertex_map;

	//fill triangles array and vertex array
	LocalVector<Triangle> triangles;
	LocalVector<Vertex> vertex_array;
	LocalVector<Seam> seams;
	LocalVector<LightmapSeamEdge> mesh_edges;

	slice_triangle_count.resize(atlas_slices);
	slice_seam_count.resize(atlas_slices);
	r_slice_seam_sources.resize(atlas_slices);

	for (int i = 0; i < atlas_slices; i++) {
		slice_triangle_count.write[i] = 0;
		slice_seam_count.write[i] = 0;
		r_slice_seam_sources.write[i].clear();
	}

	bounds = AABB();

	// Each record draws one side, so the other side may be on a different atlas slice.
	auto add_seam = [&](LightmapSeamEdge p_a, LightmapSeamEdge p_b) {
		for (int side = 0; side < 2; side++) {
			Seam seam;
			for (int endpoint = 0; endpoint < 2; endpoint++) {
				seam.dst_uv[endpoint * 2] = p_a.uv[endpoint].x;
				seam.dst_uv[endpoint * 2 + 1] = p_a.uv[endpoint].y;
				seam.src_uv[endpoint * 2] = p_b.uv[endpoint].x;
				seam.src_uv[endpoint * 2 + 1] = p_b.uv[endpoint].y;
				for (int axis = 0; axis < 3; axis++) {
					seam.normal[endpoint][axis] = p_a.normal[endpoint][axis];
				}
			}
			seam.slice = p_a.slice;
			seam.src_slice = p_b.slice;
			seam.mesh = p_a.mesh + 1; // Zero identifies unused atlas texels.
			seam.src_mesh = p_b.mesh + 1;
			seam.opposite_uv[0] = p_b.opposite_uv.x;
			seam.opposite_uv[1] = p_b.opposite_uv.y;
			seam.opposite_uv[2] = p_a.opposite_uv.x;
			seam.opposite_uv[3] = p_a.opposite_uv.y;
			for (int axis = 0; axis < 3; axis++) {
				seam.source_normal[0][axis] = p_b.normal[0][axis];
				seam.source_normal[1][axis] = p_b.normal[1][axis];
				seam.source_normal[2][axis] = p_b.opposite_normal[axis];
			}
			Transform2D uv_transform;
			LightmapSeamEdge adjacent = p_b;
			adjacent.mesh = p_a.mesh + 1;
			Vector2 overlap;
			Vector2 other_overlap;
			const bool internal_seam = p_a.mesh == p_b.mesh;
			const bool valid_transform = internal_seam ? p_a.get_uv_transform(p_b, uv_transform, true) : p_a.get_overlap(adjacent, overlap, other_overlap) && p_a.get_uv_transform(p_b, uv_transform);
			if (valid_transform) {
				for (int row = 0; row < 2; row++) {
					for (int column = 0; column < 3; column++) {
						seam.uv_transform[row][column] = uv_transform[column][row];
					}
				}
				seam.uv_transform[0][3] = 1.0f;
				Vector<int> &sources = r_slice_seam_sources.write[p_a.slice];
				if (!sources.has(p_b.slice)) {
					sources.push_back(p_b.slice);
				}
			}
			seams.push_back(seam);
			slice_seam_count.write[p_a.slice]++;
			SWAP(p_a, p_b);
		}
	};

	auto make_edge = [](const Edge &p_edge, const EdgeUV2 &p_uv, uint32_t p_mesh, uint32_t p_slice) {
		LightmapSeamEdge edge;
		edge.position[0] = p_edge.a;
		edge.position[1] = p_edge.b;
		edge.normal[0] = p_edge.na;
		edge.normal[1] = p_edge.nb;
		edge.uv[0] = p_uv.a;
		edge.uv[1] = p_uv.b;
		edge.opposite_vertex = p_uv.opposite_vertex;
		edge.opposite_normal = p_uv.opposite_normal;
		edge.opposite_uv = p_uv.opposite_uv;
		edge.mesh = p_mesh;
		edge.slice = p_slice;
		return edge;
	};

	for (int m_i = 0; m_i < mesh_instances.size(); m_i++) {
		if (p_step_function) {
			float p = float(m_i + 1) / MAX(1, mesh_instances.size()) * 0.1;
			p_step_function(0.3 + p, vformat(RTR("Plotting mesh into acceleration structure %d/%d"), m_i + 1, mesh_instances.size()), p_bake_userdata, false);
		}

		HashMap<Edge, EdgeUV2, EdgeHash> edges;

		MeshInstance &mi = mesh_instances.write[m_i];

		Vector2 uv_scale = Vector2(mi.data.lightmap_size) / Vector2(atlas_size);
		Vector2 uv_offset = Vector2(mi.offset) / Vector2(atlas_size);
		if (m_i == 0) {
			bounds.position = mi.data.points[0];
		}

		for (int i = 0; i < mi.data.points.size(); i += 3) {
			Vector3 vtxs[3] = { mi.data.points[i + 0], mi.data.points[i + 1], mi.data.points[i + 2] };
			Vector2 uvs[3] = { mi.data.uv2[i + 0] * uv_scale + uv_offset, mi.data.uv2[i + 1] * uv_scale + uv_offset, mi.data.uv2[i + 2] * uv_scale + uv_offset };
			Vector3 normal[3] = { mi.data.normal[i + 0], mi.data.normal[i + 1], mi.data.normal[i + 2] };

			AABB taabb;
			Triangle t;
			t.slice = mi.slice;
			for (int k = 0; k < 3; k++) {
				bounds.expand_to(vtxs[k]);

				Vertex v;
				v.position[0] = vtxs[k].x;
				v.position[1] = vtxs[k].y;
				v.position[2] = vtxs[k].z;
				v.uv[0] = uvs[k].x;
				v.uv[1] = uvs[k].y;
				v.normal_xy[0] = normal[k].x;
				v.normal_xy[1] = normal[k].y;
				v.normal_z = normal[k].z;

				uint32_t *indexptr = vertex_map.getptr(v);

				if (indexptr) {
					t.indices[k] = *indexptr;
				} else {
					uint32_t new_index = vertex_map.size();
					t.indices[k] = new_index;
					vertex_map[v] = new_index;
					vertex_array.push_back(v);
				}

				if (k == 0) {
					taabb.position = vtxs[k];
				} else {
					taabb.expand_to(vtxs[k]);
				}
			}

			//compute seams that will need to be blended later
			for (int k = 0; k < 3; k++) {
				int n = (k + 1) % 3;

				Edge edge(vtxs[k], vtxs[n], normal[k], normal[n]);
				EdgeUV2 uv2(uvs[k], uvs[n]);
				uv2.opposite_vertex = vtxs[(k + 2) % 3];
				uv2.opposite_normal = normal[(k + 2) % 3];
				uv2.opposite_uv = uvs[(k + 2) % 3];

				if (edge.b == edge.a) {
					continue; //degenerate, somehow
				}
				if (edge.b < edge.a) {
					SWAP(edge.a, edge.b);
					SWAP(edge.na, edge.nb);
					SWAP(uv2.a, uv2.b);
				}

				EdgeUV2 *euv2 = edges.getptr(edge);
				if (!euv2) {
					edges[edge] = uv2;
				} else {
					euv2->users++;
					if (*euv2 == uv2) {
						continue; // seam shared UV space, no need to blend
					}
					if (euv2->seam_found) {
						continue; //bad geometry
					}

					add_seam(make_edge(edge, uv2, m_i, mi.slice), make_edge(edge, *euv2, m_i, mi.slice));
					euv2->seam_found = true;
				}
			}

			t.min_bounds[0] = taabb.position.x;
			t.min_bounds[1] = taabb.position.y;
			t.min_bounds[2] = taabb.position.z;
			t.max_bounds[0] = taabb.position.x + MAX(taabb.size.x, 0.0001);
			t.max_bounds[1] = taabb.position.y + MAX(taabb.size.y, 0.0001);
			t.max_bounds[2] = taabb.position.z + MAX(taabb.size.z, 0.0001);

			t.cull_mode = RSE::CULL_MODE_BACK;

			RID material = mi.data.material[i];
			if (material.is_valid()) {
				t.cull_mode = RSG::material_storage->material_get_cull_mode(material);
			}
			t.mesh = m_i + 1;
			triangles.push_back(t);
			slice_triangle_count.write[t.slice]++;
		}

		// Internal edges (including UV seams) have already been handled above.
		// Keep unmatched edges with their normals, so hard edges of closed modules
		// can still connect to a coplanar face belonging to another mesh.
		for (const KeyValue<Edge, EdgeUV2> &E : edges) {
			if (E.value.users != 1) {
				continue;
			}
			mesh_edges.push_back(make_edge(E.key, E.value, m_i, mi.slice));
		}
	}

	{
		// Query only nearby edges instead of comparing every pair in the scene.
		// The vector is complete before inserting pointers into the BVH.
		DynamicBVH edge_bvh;
		for (LightmapSeamEdge &edge : mesh_edges) {
			auto stitch_edge = [&](void *p_data) {
				const LightmapSeamEdge &other = *static_cast<LightmapSeamEdge *>(p_data);
				Vector2 interval;
				Vector2 other_interval;
				if (edge.get_overlap(other, interval, other_interval)) {
					LightmapSeamEdge a = edge;
					LightmapSeamEdge b = other;
					for (int endpoint = 0; endpoint < 2; endpoint++) {
						a.position[endpoint] = edge.position[0].lerp(edge.position[1], interval[endpoint]);
						a.uv[endpoint] = edge.uv[0].lerp(edge.uv[1], interval[endpoint]);
						a.normal[endpoint] = edge.normal[0].lerp(edge.normal[1], interval[endpoint]).normalized();
						b.position[endpoint] = other.position[0].lerp(other.position[1], other_interval[endpoint]);
						b.uv[endpoint] = other.uv[0].lerp(other.uv[1], other_interval[endpoint]);
						b.normal[endpoint] = other.normal[0].lerp(other.normal[1], other_interval[endpoint]).normalized();
					}
					add_seam(a, b);
				}
				return false; // Continue querying all overlapping segments (including T-junctions).
			};
			edge_bvh.aabb_query(edge.get_aabb(), stitch_edge);
			edge_bvh.insert(edge.get_aabb(), &edge);
			edge_bvh.optimize_incremental(1);
		}
	}

	//also consider probe positions for bounds
	for (int i = 0; i < p_probe_positions.size(); i++) {
		Vector3 pp(p_probe_positions[i].position[0], p_probe_positions[i].position[1], p_probe_positions[i].position[2]);
		bounds.expand_to(pp);
	}
	bounds.grow_by(0.1); //grow a bit to avoid numerical error

	triangles.sort(); //sort by slice
	seams.sort();
	for (Vector<int> &sources : r_slice_seam_sources) {
		// Preserve source order when equally close margins overlap.
		sources.sort();
	}

	if (p_step_function) {
		p_step_function(0.4, RTR("Optimizing acceleration structure"), p_bake_userdata, true);
	}

	//fill list of triangles in grid
	LocalVector<TriangleSort> triangle_sort;
	for (uint32_t i = 0; i < triangles.size(); i++) {
		const Triangle &t = triangles[i];
		Vector3 face[3] = {
			Vector3(vertex_array[t.indices[0]].position[0], vertex_array[t.indices[0]].position[1], vertex_array[t.indices[0]].position[2]),
			Vector3(vertex_array[t.indices[1]].position[0], vertex_array[t.indices[1]].position[1], vertex_array[t.indices[1]].position[2]),
			Vector3(vertex_array[t.indices[2]].position[0], vertex_array[t.indices[2]].position[1], vertex_array[t.indices[2]].position[2])
		};
		_plot_triangle_into_triangle_index_list(grid_size, Vector3i(), bounds, face, i, triangle_sort, grid_size);
	}
	//sort it
	triangle_sort.sort();

	LocalVector<uint32_t> cluster_indices;
	LocalVector<ClusterAABB> cluster_aabbs;
	Vector<uint32_t> triangle_indices;
	triangle_indices.resize(triangle_sort.size());
	Vector<uint32_t> grid_indices;
	grid_indices.resize(grid_size * grid_size * grid_size * 2);
	memset(grid_indices.ptrw(), 0, grid_indices.size() * sizeof(uint32_t));

	{
		// Fill grid with cell indices.
		uint32_t last_cell = 0xFFFFFFFF;
		uint32_t *giw = grid_indices.ptrw();
		uint32_t cluster_count = 0;
		uint32_t solid_cell_count = 0;
		for (uint32_t i = 0; i < triangle_sort.size(); i++) {
			uint32_t cell = triangle_sort[i].cell_index;
			if (cell != last_cell) {
				giw[cell * 2 + 1] = solid_cell_count;
				solid_cell_count++;
			}

			if ((giw[cell * 2] % p_cluster_size) == 0) {
				// Add an extra cluster every time the triangle counter reaches a multiple of the cluster size.
				cluster_count++;
			}

			giw[cell * 2]++;
			last_cell = cell;
		}

		// Build fixed-size triangle clusters for all the cells to speed up the traversal. A cell can hold multiple clusters that each contain a fixed
		// amount of triangles and an AABB. The tracer will check against the AABBs first to know whether it needs to visit the cell's triangles.
		//
		// The building algorithm will divide the triangles recursively contained inside each cell, sorting by the longest axis of the AABB on each step.
		//
		// - If the amount of triangles is less or equal to the cluster size, the AABB will be stored and the algorithm stops.
		//
		// - The division by two is increased to the next power of two of half the amount of triangles (with cluster size as the minimum value) to
		//   ensure the first half always fills the cluster.

		cluster_indices.resize(solid_cell_count * 2);
		cluster_aabbs.resize(cluster_count);

		uint32_t i = 0;
		uint32_t cluster_index = 0;
		uint32_t solid_cell_index = 0;
		uint32_t *tiw = triangle_indices.ptrw();
		while (i < triangle_sort.size()) {
			cluster_indices[solid_cell_index * 2] = cluster_index;
			cluster_indices[solid_cell_index * 2 + 1] = i;

			uint32_t cell = triangle_sort[i].cell_index;
			uint32_t triangle_count = giw[cell * 2];
			uint32_t cell_cluster_count = (triangle_count + p_cluster_size - 1) / p_cluster_size;
			_sort_triangle_clusters(p_cluster_size, cluster_index, i, triangle_count, triangle_sort, cluster_aabbs);

			for (uint32_t j = 0; j < triangle_count; j++) {
				tiw[i + j] = triangle_sort[i + j].triangle_index;
			}

			i += triangle_count;
			cluster_index += cell_cluster_count;
			solid_cell_index++;
		}
	}
#if 0
	for (int i = 0; i < grid_size; i++) {
		for (int j = 0; j < grid_size; j++) {
			for (int k = 0; k < grid_size; k++) {
				uint32_t index = i * (grid_size * grid_size) + j * grid_size + k;
				grid_indices.write[index * 2] = float(i) / grid_size * 255;
				grid_indices.write[index * 2 + 1] = float(j) / grid_size * 255;
			}
		}
	}
#endif

#if 0
	for (int i = 0; i < grid_size; i++) {
		Vector<uint8_t> grid_usage;
		grid_usage.resize(grid_size * grid_size);
		for (int j = 0; j < grid_usage.size(); j++) {
			uint32_t ofs = i * grid_size * grid_size + j;
			uint32_t count = grid_indices[ofs * 2];
			grid_usage.write[j] = count > 0 ? 255 : 0;
		}

		Ref<Image> img = Image::create_from_data(grid_size, grid_size, false, Image::FORMAT_L8, grid_usage);
		img->save_png("res://grid_layer_" + itos(1000 + i).substr(1, 3) + ".png");
	}
#endif

	/*****************************/
	/*** CREATE GPU STRUCTURES ***/
	/*****************************/

	lights.sort();
	light_metadata.sort();

	static_assert(sizeof(Seam) == 176); // Includes three source-normal vec4s; matches GLSL std430.
	static_assert(sizeof(RasterSeamsPushConstant) == 28);

	{ //buffers
		vertex_buffer = rd->storage_buffer_create(vertex_array.size() * sizeof(Vertex), vertex_array.span().reinterpret<uint8_t>());

		triangle_buffer = rd->storage_buffer_create(triangles.size() * sizeof(Triangle), triangles.span().reinterpret<uint8_t>());

		r_triangle_indices_buffer = rd->storage_buffer_create(triangle_indices.size() * sizeof(uint32_t), triangle_indices.span().reinterpret<uint8_t>());

		r_cluster_indices_buffer = rd->storage_buffer_create(cluster_indices.size() * sizeof(uint32_t), cluster_indices.span().reinterpret<uint8_t>());

		r_cluster_aabbs_buffer = rd->storage_buffer_create(cluster_aabbs.size() * sizeof(ClusterAABB), cluster_aabbs.span().reinterpret<uint8_t>());

		// Even when there are no lights, the buffer must exist.
		static const Light empty_lights[1];
		Span<uint8_t> lb = (lights.is_empty() ? Span(empty_lights) : lights.span()).reinterpret<uint8_t>();
		lights_buffer = rd->storage_buffer_create(lb.size(), lb);

		// Even when there are no seams, the buffer must exist.
		static const Seam empty_seams[1];
		Span<uint8_t> sb = (seams.is_empty() ? Span(empty_seams) : seams.span()).reinterpret<uint8_t>();
		seams_buffer = rd->storage_buffer_create(sb.size(), sb);

		// Even when there are no probes, the buffer must exist.
		static const Probe empty_probes[1];
		Span<uint8_t> pb = (p_probe_positions.is_empty() ? Span(empty_probes) : p_probe_positions.span()).reinterpret<uint8_t>();
		probe_positions_buffer = rd->storage_buffer_create(pb.size(), pb);
	}

	{ //grid

		RD::TextureFormat tf;
		tf.width = grid_size;
		tf.height = grid_size;
		tf.depth = grid_size;
		tf.texture_type = RD::TEXTURE_TYPE_3D;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;

		Vector<Vector<uint8_t>> texdata;
		texdata.resize(1);
		//grid and indices
		tf.format = RD::DATA_FORMAT_R32G32_UINT;
		texdata.write[0] = grid_indices.to_byte_array();
		grid_texture = rd->texture_create(tf, RD::TextureView(), texdata);
	}
}

void LightmapperRD::_raster_geometry_slice(RenderingDevice *rd, Size2i atlas_size, int p_slice, int grid_size, AABB bounds, float p_bias, const Vector<int> &p_slice_triangle_count, RID position_tex, RID unocclude_tex, RID normal_tex, RID mesh_tex, RID raster_depth_buffer, RID rasterize_shader, RID raster_base_uniform) {
	RID mesh_slice_tex = rd->texture_create_shared_from_slice(RD::TextureView(), mesh_tex, p_slice, 0);
	Vector<RID> fb;
	fb.push_back(position_tex);
	fb.push_back(normal_tex);
	fb.push_back(unocclude_tex);
	fb.push_back(mesh_slice_tex);
	fb.push_back(raster_depth_buffer);
	RID framebuffer = rd->framebuffer_create(fb);

	RD::PipelineDepthStencilState ds;
	ds.enable_depth_test = true;
	ds.enable_depth_write = true;
	ds.depth_compare_operator = RD::COMPARE_OP_LESS; //so it does render same pixel twice

	RID raster_pipeline = rd->render_pipeline_create(rasterize_shader, rd->framebuffer_get_format(framebuffer), RD::INVALID_FORMAT_ID, RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), ds, RD::PipelineColorBlendState::create_disabled(4), 0);
	RID raster_pipeline_wire;
	{
		RD::PipelineRasterizationState rw;
		rw.wireframe = true;
		raster_pipeline_wire = rd->render_pipeline_create(rasterize_shader, rd->framebuffer_get_format(framebuffer), RD::INVALID_FORMAT_ID, RD::RENDER_PRIMITIVE_TRIANGLES, rw, RD::PipelineMultisampleState(), ds, RD::PipelineColorBlendState::create_disabled(4), 0);
	}

	uint32_t triangle_offset = 0;
	for (int i = 0; i < p_slice; i++) {
		triangle_offset += p_slice_triangle_count[i];
	}
	Vector<Color> clear_colors;
	clear_colors.push_back(Color(0, 0, 0, 0));
	clear_colors.push_back(Color(0, 0, 0, 0));
	clear_colors.push_back(Color(0, 0, 0, 0));
	clear_colors.push_back(Color(0, 0, 0, 0));

	{
		RasterPushConstant raster_push_constant;
		raster_push_constant.atlas_size[0] = atlas_size.x;
		raster_push_constant.atlas_size[1] = atlas_size.y;
		raster_push_constant.base_triangle = triangle_offset;
		raster_push_constant.to_cell_offset[0] = bounds.position.x;
		raster_push_constant.to_cell_offset[1] = bounds.position.y;
		raster_push_constant.to_cell_offset[2] = bounds.position.z;
		raster_push_constant.bias = p_bias;
		raster_push_constant.to_cell_size[0] = (1.0 / bounds.size.x) * float(grid_size);
		raster_push_constant.to_cell_size[1] = (1.0 / bounds.size.y) * float(grid_size);
		raster_push_constant.to_cell_size[2] = (1.0 / bounds.size.z) * float(grid_size);
		raster_push_constant.grid_size[0] = grid_size;
		raster_push_constant.grid_size[1] = grid_size;
		raster_push_constant.grid_size[2] = grid_size;

		raster_push_constant.uv_offset[0] = 0.0f;
		raster_push_constant.uv_offset[1] = 0.0f;

		RD::DrawListID draw_list = rd->draw_list_begin(framebuffer, RD::DRAW_CLEAR_ALL, clear_colors, 1.0f, 0, Rect2(), RDD::BreadcrumbMarker::LIGHTMAPPER_PASS);
		//draw opaque
		rd->draw_list_bind_render_pipeline(draw_list, raster_pipeline);
		rd->draw_list_bind_uniform_set(draw_list, raster_base_uniform, 0);
		rd->draw_list_set_push_constant(draw_list, &raster_push_constant, sizeof(RasterPushConstant));
		rd->draw_list_draw(draw_list, false, 1, p_slice_triangle_count[p_slice] * 3);
		//draw wire
		rd->draw_list_bind_render_pipeline(draw_list, raster_pipeline_wire);
		rd->draw_list_bind_uniform_set(draw_list, raster_base_uniform, 0);
		rd->draw_list_set_push_constant(draw_list, &raster_push_constant, sizeof(RasterPushConstant));
		rd->draw_list_draw(draw_list, false, 1, p_slice_triangle_count[p_slice] * 3);

		rd->draw_list_end();
	}
	rd->free_rid(raster_pipeline_wire);
	rd->free_rid(raster_pipeline);
	rd->free_rid(framebuffer);
	rd->free_rid(mesh_slice_tex);
}

LightmapperRD::BakeError LightmapperRD::_prepare_oidn_margins(RenderingDevice *p_rd, RID p_base_uniform, RID p_mesh_tex, RID p_normal_tex, RID p_unocclude_tex, const Size2i &p_atlas_size, const Vector<int> &p_slice_seam_count, const Vector<Vector<int>> &p_slice_seam_sources, int p_range, RID p_padded_normal_tex, const std::function<void(int)> &p_prepare_geometry_slice, const std::function<BakeError(int, RID)> &p_process_slice, BakeStepFunc p_step_function, void *p_bake_userdata) {
	Ref<RDShaderFile> shader_file;
	shader_file.instantiate();
	Error error = shader_file->parse_versions_from_text(lm_blendseams_shader_glsl);
	ERR_FAIL_COND_V(error != OK, BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
	RID shader = p_rd->shader_create_from_spirv(shader_file->get_spirv_stages("margins"));
	ERR_FAIL_COND_V(shader.is_null(), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);

	RID margin;
	RID depth;
	RID framebuffer;
	auto cleanup = [&]() {
		if (framebuffer.is_valid()) {
			p_rd->free_rid(framebuffer);
		}
		if (margin.is_valid()) {
			p_rd->free_rid(margin);
		}
		if (depth.is_valid()) {
			p_rd->free_rid(depth);
		}
		// Also frees the dependent pipeline and uniform set.
		p_rd->free_rid(shader);
	};

	RD::TextureFormat format;
	format.width = p_atlas_size.width;
	format.height = p_atlas_size.height;
	format.array_layers = 1;
	format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	format.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	format.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	margin = p_rd->texture_create(format, RD::TextureView());
	format.format = RD::DATA_FORMAT_D32_SFLOAT;
	format.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	depth = p_rd->texture_create(format, RD::TextureView());
	if (margin.is_null() || depth.is_null()) {
		cleanup();
		return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
	}

	Vector<RD::Uniform> uniforms;
	const RID textures[3] = { p_mesh_tex, p_normal_tex, p_unocclude_tex };
	for (int i = 0; i < 3; i++) {
		RD::Uniform uniform;
		uniform.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		uniform.binding = i;
		uniform.append_id(textures[i]);
		uniforms.push_back(uniform);
	}
	RID uniform_set = p_rd->uniform_set_create(uniforms, shader, 1);
	Vector<RID> attachments;
	attachments.push_back(margin);
	attachments.push_back(p_padded_normal_tex);
	attachments.push_back(depth);
	framebuffer = p_rd->framebuffer_create(attachments);
	if (uniform_set.is_null() || framebuffer.is_null()) {
		cleanup();
		return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
	}
	RD::PipelineDepthStencilState ds;
	ds.enable_depth_test = true;
	ds.enable_depth_write = true;
	ds.depth_compare_operator = RD::COMPARE_OP_LESS;
	RID pipeline = p_rd->render_pipeline_create(shader, p_rd->framebuffer_get_format(framebuffer), RD::INVALID_FORMAT_ID, RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), ds, RD::PipelineColorBlendState::create_disabled(2), 0);
	if (pipeline.is_null()) {
		cleanup();
		return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
	}

	uint32_t offset = 0;
	for (int slice = 0; slice < p_slice_seam_count.size(); slice++) {
		if (p_step_function && p_step_function(0.8, vformat(RTR("Preparing denoiser context %d/%d"), slice + 1, p_slice_seam_count.size()), p_bake_userdata, true)) {
			cleanup();
			return BAKE_ERROR_USER_ABORTED;
		}
		p_prepare_geometry_slice(slice);
		if (p_rd->texture_copy(p_normal_tex, p_padded_normal_tex, Vector3(), Vector3(), Vector3(p_atlas_size.width, p_atlas_size.height, 1), 0, 0, 0, 0) != OK) {
			cleanup();
			return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
		}
		int geometry_slice = slice;
		// Clear only between destinations. Keep depth across all their sources so
		// the closest validated seam wins, and preserve original normal coverage.
		Vector<Color> clear_colors;
		clear_colors.push_back(Color(0, 0, 0, 0));
		clear_colors.push_back(Color(0, 0, 0, 0));
		p_rd->draw_list_begin(framebuffer, RD::DRAW_CLEAR_COLOR_0 | RD::DRAW_CLEAR_DEPTH, clear_colors, 1.0f);
		p_rd->draw_list_end();
		for (int source_slice : p_slice_seam_sources[slice]) {
			if (p_step_function && p_step_function(0.8, vformat(RTR("Preparing denoiser context %d/%d"), slice + 1, p_slice_seam_count.size()), p_bake_userdata, false)) {
				cleanup();
				return BAKE_ERROR_USER_ABORTED;
			}
			if (geometry_slice != source_slice) {
				p_prepare_geometry_slice(source_slice);
				geometry_slice = source_slice;
			}
			RasterSeamsPushConstant params;
			params.base_index = offset;
			params.slice = slice;
			params.pad = source_slice;
			params.blend = p_range;
			RD::DrawListID draw_list = p_rd->draw_list_begin(framebuffer);
			p_rd->draw_list_bind_render_pipeline(draw_list, pipeline);
			p_rd->draw_list_bind_uniform_set(draw_list, p_base_uniform, 0);
			p_rd->draw_list_bind_uniform_set(draw_list, uniform_set, 1);
			p_rd->draw_list_set_push_constant(draw_list, &params, sizeof(params));
			p_rd->draw_list_draw(draw_list, false, 1, p_slice_seam_count[slice] * 6);
			p_rd->draw_list_end();
		}
		BakeError process_error = p_process_slice(slice, margin);
		if (process_error != BAKE_OK) {
			cleanup();
			return process_error;
		}
		offset += p_slice_seam_count[slice];
	}
	cleanup();
	return BAKE_OK;
}

static Vector<RD::Uniform> dilate_or_denoise_common_uniforms(RID &p_source_light_tex, RID &p_dest_light_tex) {
	Vector<RD::Uniform> uniforms;
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.binding = 0;
		u.append_id(p_dest_light_tex);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.binding = 1;
		u.append_id(p_source_light_tex);
		uniforms.push_back(u);
	}

	return uniforms;
}

LightmapperRD::BakeError LightmapperRD::_dilate(RenderingDevice *rd, Ref<RDShaderFile> &compute_shader, RID &compute_base_uniform_set, PushConstant &push_constant, RID &source_light_tex, RID &dest_light_tex, const Size2i &atlas_size, int atlas_slices) {
	RID compute_shader_dilate = rd->shader_create_from_spirv(compute_shader->get_spirv_stages("dilate"));
	ERR_FAIL_COND_V(compute_shader_dilate.is_null(), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES); //internal check, should not happen
	RID compute_shader_dilate_pipeline = rd->compute_pipeline_create(compute_shader_dilate);
	push_constant.region_ofs[0] = 0;
	push_constant.region_ofs[1] = 0;
	const Vector3i group_size(Math::division_round_up(atlas_size.x, 8), Math::division_round_up(atlas_size.y, 8), 1);
	RID dest_slice = rd->texture_create_shared_from_slice(RD::TextureView(), dest_light_tex, 0, 0, 1, RD::TEXTURE_SLICE_2D_ARRAY, 1);

	for (int i = 0; i < atlas_slices; i++) {
		RID source_slice = rd->texture_create_shared_from_slice(RD::TextureView(), source_light_tex, i, 0, 1, RD::TEXTURE_SLICE_2D_ARRAY, 1);
		Vector<RD::Uniform> uniforms = dilate_or_denoise_common_uniforms(source_slice, dest_slice);
		RID uniform_set = rd->uniform_set_create(uniforms, compute_shader_dilate, 1);
		RD::ComputeListID compute_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(compute_list, compute_shader_dilate_pipeline);
		rd->compute_list_bind_uniform_set(compute_list, compute_base_uniform_set, 0);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set, 1);
		push_constant.atlas_slice = 0;
		rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		rd->compute_list_dispatch(compute_list, group_size.x, group_size.y, group_size.z);
		rd->compute_list_end();
		rd->texture_copy(dest_light_tex, source_light_tex, Vector3(), Vector3(), Vector3(atlas_size.width, atlas_size.height, 1), 0, 0, 0, i);
		rd->free_rid(uniform_set);
		rd->free_rid(source_slice);
	}
	rd->free_rid(dest_slice);
	rd->free_rid(compute_shader_dilate);

#ifdef DEBUG_TEXTURES
	for (int i = 0; i < atlas_slices; i++) {
		Vector<uint8_t> s = rd->texture_get_data(source_light_tex, i);
		Ref<Image> img = Image::create_from_data(atlas_size.width, atlas_size.height, false, Image::FORMAT_RGBAH, s);
		img->convert(Image::FORMAT_RGBA8);
		img->save_png("res://5_dilated_" + itos(i) + ".png");
	}
#endif
	return BAKE_OK;
}

LightmapperRD::BakeError LightmapperRD::_pad_oidn_slice(RenderingDevice *p_rd, Ref<RDShaderFile> &p_compute_shader, const RID &p_compute_base_uniform_set, PushConstant &p_push_constant, RID p_source_tex, RID p_dest_tex, RID p_margin_tex, RID p_mesh_tex, const Size2i &p_atlas_size, uint32_t p_source_slice, uint32_t p_dest_slice, uint32_t p_geometry_slice, uint32_t p_coefficient_count) {
	// Other destinations still read the original source coefficient.
	ERR_FAIL_COND_V(p_source_tex == p_dest_tex, BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
	Vector<RD::Uniform> uniforms = dilate_or_denoise_common_uniforms(p_source_tex, p_dest_tex);
	const RID guide_textures[2] = { p_margin_tex, p_mesh_tex };
	for (int i = 0; i < 2; i++) {
		RD::Uniform uniform;
		uniform.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		uniform.binding = 2 + i;
		uniform.append_id(guide_textures[i]);
		uniforms.push_back(uniform);
	}

	RID shader = p_rd->shader_create_from_spirv(p_compute_shader->get_spirv_stages(p_rd->texture_get_format(p_dest_tex).format == RD::DATA_FORMAT_R8G8B8A8_UNORM ? "pad_oidn_shadowmask" : "pad_oidn"));
	ERR_FAIL_COND_V(shader.is_null(), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
	RID pipeline = p_rd->compute_pipeline_create(shader);
	RID uniform_set = p_rd->uniform_set_create(uniforms, shader, 1);
	if (pipeline.is_null() || uniform_set.is_null()) {
		p_rd->free_rid(shader);
		return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
	}
	const uint32_t previous_ray_to = p_push_constant.ray_to;
	p_push_constant.ray_to = p_coefficient_count;
	p_push_constant.atlas_slice = p_source_slice;
	p_push_constant.output_slice = p_dest_slice;
	p_push_constant.geometry_slice = p_geometry_slice;
	p_push_constant.material_slice = p_source_slice % p_coefficient_count;
	p_push_constant.region_ofs[0] = 0;
	p_push_constant.region_ofs[1] = 0;
	const Vector3i group_size(Math::division_round_up(p_atlas_size.x, 8), Math::division_round_up(p_atlas_size.y, 8), 1);

	RD::ComputeListID compute_list = p_rd->compute_list_begin();
	p_rd->compute_list_bind_compute_pipeline(compute_list, pipeline);
	p_rd->compute_list_bind_uniform_set(compute_list, p_compute_base_uniform_set, 0);
	p_rd->compute_list_bind_uniform_set(compute_list, uniform_set, 1);
	p_rd->compute_list_set_push_constant(compute_list, &p_push_constant, sizeof(PushConstant));
	p_rd->compute_list_dispatch(compute_list, group_size.x, group_size.y, group_size.z);
	p_rd->compute_list_end();
	p_push_constant.ray_to = previous_ray_to;
	p_rd->free_rid(shader);
	return BAKE_OK;
}

LightmapperRD::BakeError LightmapperRD::_pack_l1(RenderingDevice *rd, Ref<RDShaderFile> &compute_shader, RID &compute_base_uniform_set, PushConstant &push_constant, RID &source_light_tex, RID &dest_light_tex, const Size2i &atlas_size, int atlas_slices) {
	RID compute_shader_pack = rd->shader_create_from_spirv(compute_shader->get_spirv_stages("pack_coeffs"));
	ERR_FAIL_COND_V(compute_shader_pack.is_null(), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES); //internal check, should not happen
	RID compute_shader_pack_pipeline = rd->compute_pipeline_create(compute_shader_pack);
	push_constant.region_ofs[0] = 0;
	push_constant.region_ofs[1] = 0;
	const Vector3i group_size(Math::division_round_up(atlas_size.x, 8), Math::division_round_up(atlas_size.y, 8), 1);

	for (int i = 0; i < atlas_slices; i++) {
		RID source_slice = rd->texture_create_shared_from_slice(RD::TextureView(), source_light_tex, i * 4, 0, 1, RD::TEXTURE_SLICE_2D_ARRAY, 4);
		Vector<RD::Uniform> uniforms = dilate_or_denoise_common_uniforms(source_slice, dest_light_tex);
		RID uniform_set = rd->uniform_set_create(uniforms, compute_shader_pack, 1);
		RD::ComputeListID compute_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(compute_list, compute_shader_pack_pipeline);
		rd->compute_list_bind_uniform_set(compute_list, compute_base_uniform_set, 0);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set, 1);
		push_constant.atlas_slice = 0;
		rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		rd->compute_list_dispatch(compute_list, group_size.x, group_size.y, group_size.z);
		rd->compute_list_end();
		for (int coefficient = 0; coefficient < 4; coefficient++) {
			rd->texture_copy(dest_light_tex, source_light_tex, Vector3(), Vector3(), Vector3(atlas_size.width, atlas_size.height, 1), 0, 0, coefficient, i * 4 + coefficient);
		}
		rd->free_rid(uniform_set);
		rd->free_rid(source_slice);
	}
	rd->free_rid(compute_shader_pack);

	return BAKE_OK;
}

Error LightmapperRD::_store_pfm(const Vector<uint8_t> &p_data, const Size2i &p_atlas_size, const String &p_name, bool p_shadowmask) {
	Ref<Image> img = Image::create_from_data(p_atlas_size.width, p_atlas_size.height, false, p_shadowmask ? Image::FORMAT_RGBA8 : Image::FORMAT_RGBAH, p_data);
	ERR_FAIL_COND_V(img.is_null() || img->is_empty(), ERR_INVALID_DATA);
	img->convert(Image::FORMAT_RGBF);
	Vector<uint8_t> data_float = img->get_data();

	Error err = OK;
	Ref<FileAccess> file = FileAccess::open(p_name, FileAccess::WRITE, &err);
	ERR_FAIL_COND_V_MSG(err, err, vformat("Can't save PFN at path: '%s'.", p_name));
	file->store_line("PF");
	file->store_line(vformat("%d %d", img->get_width(), img->get_height()));
	file->store_line("-1.0");
	ERR_FAIL_COND_V(!file->store_buffer(data_float), ERR_FILE_CANT_WRITE);
	file->close();

	return OK;
}

Ref<Image> LightmapperRD::_read_pfm(const String &p_name, bool p_shadowmask) {
	Error err = OK;
	Ref<FileAccess> file = FileAccess::open(p_name, FileAccess::READ, &err);
	ERR_FAIL_COND_V_MSG(err, Ref<Image>(), vformat("Can't load PFM at path: '%s'.", p_name));
	ERR_FAIL_COND_V(file->get_line() != "PF", Ref<Image>());

	Vector<String> new_size = file->get_line().split(" ");
	ERR_FAIL_COND_V(new_size.size() != 2, Ref<Image>());
	int new_width = new_size[0].to_int();
	int new_height = new_size[1].to_int();

	float endian = file->get_line().to_float();
	Vector<uint8_t> new_data = file->get_buffer(file->get_length() - file->get_position());
	file->close();

	if (unlikely(endian > 0.0)) {
		uint32_t count = new_data.size() / 4;
		uint16_t *dst = (uint16_t *)new_data.ptrw();
		for (uint32_t j = 0; j < count; j++) {
			dst[j * 4] = BSWAP32(dst[j * 4]);
		}
	}
	Ref<Image> img = Image::create_from_data(new_width, new_height, false, Image::FORMAT_RGBF, new_data);
	ERR_FAIL_COND_V(img.is_null() || img->is_empty(), Ref<Image>());
	img->convert(p_shadowmask ? Image::FORMAT_RGBA8 : Image::FORMAT_RGBAH);
	return img;
}

static bool _oidn_error_is_out_of_memory(const String &p_error) {
	const String error = p_error.to_lower().replace("_", " ").replace("-", " ");
	static constexpr const char *patterns[] = {
		"out of memory",
		"outofmemory",
		"not enough memory",
		"insufficient memory",
		"memory exhausted",
		"memory allocation",
		"failed to allocate",
		"allocation failed",
		"cannot allocate",
	};
	for (const char *pattern : patterns) {
		if (error.contains(pattern)) {
			return true;
		}
	}
	return false;
}

LightmapperRD::BakeError LightmapperRD::_denoise_oidn(const String &p_light_path, const String &p_normal_path, const Size2i &p_atlas_size, int p_atlas_slices, bool p_bake_sh, bool p_shadowmask, const String &p_exe, const String &p_device, BakeStepFunc p_step_function, void *p_bake_userdata) {
	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);

	// The command-line utility accepts only one image. Load the library from the
	// same OIDN installation and reuse one device, filter and set of buffers for
	// the complete batch. If the library is unavailable, retain the CLI fallback.
	struct OIDNBatchAPI {
		void *library = nullptr;
		void *(*new_device)(int) = nullptr;
		void (*commit_device)(void *) = nullptr;
		int (*get_device_error)(void *, const char **) = nullptr;
		void (*release_device)(void *) = nullptr;
		void *(*new_buffer)(void *, size_t) = nullptr;
		void (*write_buffer)(void *, size_t, size_t, const void *) = nullptr;
		void (*read_buffer)(void *, size_t, size_t, void *) = nullptr;
		void (*release_buffer)(void *) = nullptr;
		void *(*new_filter)(void *, const char *) = nullptr;
		void (*set_filter_image)(void *, const char *, void *, int, size_t, size_t, size_t, size_t, size_t) = nullptr;
		void (*set_filter_bool)(void *, const char *, bool) = nullptr;
		void (*commit_filter)(void *) = nullptr;
		void (*execute_filter)(void *) = nullptr;
		void (*release_filter)(void *) = nullptr;

		~OIDNBatchAPI() {
			if (library != nullptr) {
				OS::get_singleton()->close_dynamic_library(library);
			}
		}
	} api;

	Vector<String> library_candidates;
	const String oidn_root = p_exe.get_base_dir().get_base_dir();
#ifdef WINDOWS_ENABLED
	library_candidates.push_back(p_exe.get_base_dir().path_join("OpenImageDenoise.dll"));
#elif defined(MACOS_ENABLED)
	library_candidates.push_back(oidn_root.path_join("lib/libOpenImageDenoise.2.dylib"));
	library_candidates.push_back(oidn_root.path_join("lib/libOpenImageDenoise.dylib"));
#else
	library_candidates.push_back(oidn_root.path_join("lib/libOpenImageDenoise.so.2"));
	library_candidates.push_back(oidn_root.path_join("lib/libOpenImageDenoise.so"));
#endif
	for (const String &library_path : library_candidates) {
		if (FileAccess::exists(library_path) && OS::get_singleton()->open_dynamic_library(library_path, api.library) == OK) {
			break;
		}
	}

	auto load_symbol = [&](const char *p_name, auto &r_function) -> bool {
		void *symbol = nullptr;
		if (api.library == nullptr || OS::get_singleton()->get_dynamic_library_symbol_handle(api.library, p_name, symbol) != OK) {
			return false;
		}
		r_function = reinterpret_cast<std::remove_reference_t<decltype(r_function)>>(symbol);
		return true;
	};
	const bool batch_api_available =
			load_symbol("oidnNewDevice", api.new_device) &&
			load_symbol("oidnCommitDevice", api.commit_device) &&
			load_symbol("oidnGetDeviceError", api.get_device_error) &&
			load_symbol("oidnReleaseDevice", api.release_device) &&
			load_symbol("oidnNewBuffer", api.new_buffer) &&
			load_symbol("oidnWriteBuffer", api.write_buffer) &&
			load_symbol("oidnReadBuffer", api.read_buffer) &&
			load_symbol("oidnReleaseBuffer", api.release_buffer) &&
			load_symbol("oidnNewFilter", api.new_filter) &&
			load_symbol("oidnSetFilterImage", api.set_filter_image) &&
			load_symbol("oidnSetFilterBool", api.set_filter_bool) &&
			load_symbol("oidnCommitFilter", api.commit_filter) &&
			load_symbol("oidnExecuteFilter", api.execute_filter) &&
			load_symbol("oidnReleaseFilter", api.release_filter);

	if (batch_api_available) {
		int device_type = 0;
		if (p_device == "cpu") {
			device_type = 1;
		} else if (p_device == "sycl") {
			device_type = 2;
		} else if (p_device == "cuda") {
			device_type = 3;
		} else if (p_device == "hip") {
			device_type = 4;
		} else if (p_device == "metal") {
			device_type = 5;
		}

		void *device = api.new_device(device_type);
		if (device != nullptr) {
			api.commit_device(device);
		}
		const int64_t pixel_count = int64_t(p_atlas_size.width) * p_atlas_size.height;
		const size_t image_bytes = size_t(pixel_count) * 3 * sizeof(float);
		void *color_buffer = device != nullptr ? api.new_buffer(device, image_bytes) : nullptr;
		void *normal_buffer = device != nullptr ? api.new_buffer(device, image_bytes) : nullptr;
		void *output_buffer = device != nullptr ? api.new_buffer(device, image_bytes) : nullptr;
		void *filter = device != nullptr ? api.new_filter(device, "RTLightmap") : nullptr;
		bool batch_ok = device != nullptr && color_buffer != nullptr && normal_buffer != nullptr && output_buffer != nullptr && filter != nullptr;
		String batch_error;
		if (batch_ok) {
			constexpr int OIDN_FORMAT_FLOAT3 = 3;
			api.set_filter_image(filter, "color", color_buffer, OIDN_FORMAT_FLOAT3, p_atlas_size.width, p_atlas_size.height, 0, 0, 0);
			api.set_filter_image(filter, "normal", normal_buffer, OIDN_FORMAT_FLOAT3, p_atlas_size.width, p_atlas_size.height, 0, 0, 0);
			api.set_filter_image(filter, "output", output_buffer, OIDN_FORMAT_FLOAT3, p_atlas_size.width, p_atlas_size.height, 0, 0, 0);
			api.set_filter_bool(filter, "hdr", !p_shadowmask);
			api.commit_filter(filter);
			const char *error_message = nullptr;
			batch_ok = api.get_device_error(device, &error_message) == 0;
			if (!batch_ok && error_message != nullptr) {
				batch_error = error_message;
			}
		} else if (device != nullptr) {
			const char *error_message = nullptr;
			api.get_device_error(device, &error_message);
			if (error_message != nullptr) {
				batch_error = error_message;
			}
		}

		Vector<float> color_data;
		Vector<float> normal_data;
		Vector<float> output_data;
		color_data.resize(pixel_count * 3);
		normal_data.resize(pixel_count * 3);
		output_data.resize(pixel_count * 3);
		const int coefficient_count = p_bake_sh ? 4 : 1;
		bool batch_aborted = false;
		struct OIDNBackup {
			String path;
			int image_index = 0;
		};
		Vector<OIDNBackup> backups;
		auto remove_backups = [&]() {
			for (const OIDNBackup &backup : backups) {
				DirAccess::remove_absolute(backup.path);
			}
			backups.clear();
		};
		auto restore_backups = [&]() -> bool {
			for (const OIDNBackup &backup : backups) {
				Ref<FileAccess> output = FileAccess::open(p_light_path + itos(backup.image_index), FileAccess::WRITE);
				if (output.is_null() || !output->store_buffer(FileAccess::get_file_as_bytes(backup.path))) {
					return false;
				}
			}
			return true;
		};
		for (int slice = 0; slice < p_atlas_slices && batch_ok; slice++) {
			const Vector<uint8_t> normal_raw = FileAccess::get_file_as_bytes(p_normal_path + itos(slice));
			if (normal_raw.size() != pixel_count * 8) {
				batch_ok = false;
				break;
			}
			const uint16_t *normal_half = reinterpret_cast<const uint16_t *>(normal_raw.ptr());
			for (int64_t pixel = 0; pixel < pixel_count; pixel++) {
				for (int component = 0; component < 3; component++) {
					normal_data.write[pixel * 3 + component] = Math::half_to_float(normal_half[pixel * 4 + component]);
				}
			}
			api.write_buffer(normal_buffer, 0, image_bytes, normal_data.ptr());

			for (int coefficient = 0; coefficient < coefficient_count && batch_ok; coefficient++) {
				const int index = slice * coefficient_count + coefficient;
				if (p_step_function && p_step_function(0.8, vformat(RTR("Denoising %s image %d/%d"), p_shadowmask ? "shadowmask" : "lightmap", index + 1, p_atlas_slices * coefficient_count), p_bake_userdata, true)) {
					batch_aborted = true;
					batch_ok = false;
					break;
				}
				Vector<uint8_t> old_data = FileAccess::get_file_as_bytes(p_light_path + itos(index));
				const int component_size = p_shadowmask ? 1 : 2;
				if (old_data.size() != pixel_count * 4 * component_size) {
					batch_ok = false;
					break;
				}
				if (p_device != "cpu") {
					const String backup_path = EditorPaths::get_singleton()->get_cache_dir().path_join(vformat("oidn_gpu_backup_%d_%d", OS::get_singleton()->get_process_id(), index));
					Ref<FileAccess> backup = FileAccess::open(backup_path, FileAccess::WRITE);
					if (backup.is_null() || !backup->store_buffer(old_data)) {
						batch_ok = false;
						break;
					}
					backups.push_back({ backup_path, index });
				}
				if (p_shadowmask) {
					const uint8_t *source = old_data.ptr();
					for (int64_t pixel = 0; pixel < pixel_count; pixel++) {
						for (int component = 0; component < 3; component++) {
							color_data.write[pixel * 3 + component] = source[pixel * 4 + component] / 255.0f;
						}
					}
				} else {
					const uint16_t *source = reinterpret_cast<const uint16_t *>(old_data.ptr());
					for (int64_t pixel = 0; pixel < pixel_count; pixel++) {
						for (int component = 0; component < 3; component++) {
							color_data.write[pixel * 3 + component] = Math::half_to_float(source[pixel * 4 + component]);
						}
					}
				}
				api.write_buffer(color_buffer, 0, image_bytes, color_data.ptr());
				api.execute_filter(filter);
				const char *error_message = nullptr;
				if (api.get_device_error(device, &error_message) != 0) {
					batch_error = error_message != nullptr ? error_message : "unknown error";
					batch_ok = false;
					break;
				}
				api.read_buffer(output_buffer, 0, image_bytes, output_data.ptrw());
				if (p_shadowmask) {
					uint8_t *destination = old_data.ptrw();
					for (int64_t pixel = 0; pixel < pixel_count; pixel++) {
						for (int component = 0; component < 3; component++) {
							destination[pixel * 4 + component] = uint8_t(CLAMP(output_data[pixel * 3 + component] * 255.0f, 0.0f, 255.0f));
						}
					}
				} else {
					uint16_t *destination = reinterpret_cast<uint16_t *>(old_data.ptrw());
					for (int64_t pixel = 0; pixel < pixel_count; pixel++) {
						for (int component = 0; component < 3; component++) {
							destination[pixel * 4 + component] = Math::make_half_float(output_data[pixel * 3 + component]);
						}
					}
				}
				Ref<FileAccess> output = FileAccess::open(p_light_path + itos(index), FileAccess::WRITE);
				batch_ok = output.is_valid() && output->store_buffer(old_data);
			}
		}

		if (filter != nullptr) {
			api.release_filter(filter);
		}
		if (color_buffer != nullptr) {
			api.release_buffer(color_buffer);
		}
		if (normal_buffer != nullptr) {
			api.release_buffer(normal_buffer);
		}
		if (output_buffer != nullptr) {
			api.release_buffer(output_buffer);
		}
		if (device != nullptr) {
			api.release_device(device);
		}
		if (batch_ok) {
			remove_backups();
			return BAKE_OK;
		}
		const bool restored = restore_backups();
		remove_backups();
		if (!restored) {
			return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
		}
		if (!batch_aborted && p_device != "cpu" && _oidn_error_is_out_of_memory(batch_error)) {
			WARN_PRINT(vformat("OIDN ran out of memory on device '%s'; retrying the denoise batch on CPU.", p_device));
			return _denoise_oidn(p_light_path, p_normal_path, p_atlas_size, p_atlas_slices, p_bake_sh, p_shadowmask, p_exe, "cpu", p_step_function, p_bake_userdata);
		}
		if (!batch_aborted && !batch_error.is_empty()) {
			ERR_PRINT(vformat("OIDN batch denoiser failed: %s", batch_error));
		}
		return batch_aborted ? BAKE_ERROR_USER_ABORTED : BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
	}

	WARN_PRINT("OIDN library API could not be loaded; falling back to one oidnDenoise process per image.");

	for (int i = 0; i < p_atlas_slices; i++) {
		String fname_norm_in = EditorPaths::get_singleton()->get_cache_dir().path_join(vformat("temp_norm_%d.pfm", i));
		Error store_error = _store_pfm(FileAccess::get_file_as_bytes(p_normal_path + itos(i)), p_atlas_size, fname_norm_in, false);
		if (store_error != OK) {
			da->remove(fname_norm_in);
			return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
		}

		for (int j = 0; j < (p_bake_sh ? 4 : 1); j++) {
			int index = i * (p_bake_sh ? 4 : 1) + j;
			String fname_light_in = EditorPaths::get_singleton()->get_cache_dir().path_join(vformat("temp_light_%d.pfm", index));
			String fname_out = EditorPaths::get_singleton()->get_cache_dir().path_join(vformat("temp_denoised_%d.pfm", index));

			if (p_step_function && p_step_function(0.8, vformat(RTR("Denoising %s image %d/%d"), p_shadowmask ? "shadowmask" : "lightmap", index + 1, p_atlas_slices * (p_bake_sh ? 4 : 1)), p_bake_userdata, true)) {
				da->remove(fname_norm_in);
				return BAKE_ERROR_USER_ABORTED;
			}
			store_error = _store_pfm(FileAccess::get_file_as_bytes(p_light_path + itos(index)), p_atlas_size, fname_light_in, p_shadowmask);
			if (store_error != OK) {
				da->remove(fname_light_in);
				da->remove(fname_norm_in);
				return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
			}
			auto run_oidn = [&](const String &p_run_device, String &r_output, int &r_exitcode) {
				List<String> args;
				args.push_back("--device");
				args.push_back(p_run_device);
				args.push_back("--filter");
				args.push_back("RTLightmap");
				args.push_back(p_shadowmask ? "--ldr" : "--hdr");
				args.push_back(fname_light_in);
				args.push_back("--nrm");
				args.push_back(fname_norm_in);
				args.push_back("--output");
				args.push_back(fname_out);
				return OS::get_singleton()->execute(p_exe, args, &r_output, &r_exitcode, true);
			};

			String str;
			int exitcode = 0;
			Error err = run_oidn(p_device, str, exitcode);
			if ((err != OK || exitcode != 0) && p_device != "cpu" && _oidn_error_is_out_of_memory(str)) {
				WARN_PRINT(vformat("OIDN ran out of memory on device '%s'; retrying image %d on CPU.", p_device, index + 1));
				da->remove(fname_out);
				str.clear();
				exitcode = 0;
				err = run_oidn("cpu", str, exitcode);
			}

			da->remove(fname_light_in);

			if (err != OK || exitcode != 0) {
				da->remove(fname_out);
				da->remove(fname_norm_in);
				ERR_FAIL_V_MSG(BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES, vformat("OIDN denoiser failed, return code: %d. Output: %s", exitcode, str.strip_edges()));
			}

			Ref<Image> img = _read_pfm(fname_out, p_shadowmask);
			da->remove(fname_out);

			if (img.is_null() || img->is_empty()) {
				da->remove(fname_norm_in);
				ERR_FAIL_V(BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
			}

			Vector<uint8_t> old_data = FileAccess::get_file_as_bytes(p_light_path + itos(index));
			Vector<uint8_t> new_data = img->get_data();
			if (img->get_size() != p_atlas_size || new_data.size() != old_data.size()) {
				da->remove(fname_norm_in);
				ERR_FAIL_V(BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
			}
			img.unref(); // Avoid copy on write.

			// Preserve coverage, including the byte-sized shadowmask alpha.
			const int component_size = p_shadowmask ? 1 : 2;
			const uint8_t *src = old_data.ptr();
			uint8_t *dst = new_data.ptrw();
			for (int64_t k = 0; k < new_data.size(); k += 4 * component_size) {
				memcpy(dst + k + 3 * component_size, src + k + 3 * component_size, component_size);
			}
			Ref<FileAccess> output = FileAccess::open(p_light_path + itos(index), FileAccess::WRITE);
			if (output.is_null() || !output->store_buffer(new_data)) {
				da->remove(fname_norm_in);
				ERR_FAIL_V(BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
			}
		}
		da->remove(fname_norm_in);
	}
	return BAKE_OK;
}

LightmapperRD::BakeError LightmapperRD::_denoise_slice(RenderingDevice *p_rd, Ref<RDShaderFile> &p_compute_shader, const RID &p_compute_base_uniform_set, PushConstant &p_push_constant, RID p_source_light_tex, RID p_source_normal_tex, RID p_dest_light_tex, RID p_unocclude_tex, float p_denoiser_strength, int p_denoiser_range, const Size2i &p_atlas_size, int p_atlas_slice, int p_material_slice, int p_atlas_slices, bool p_bake_sh, BakeStepFunc p_step_function, void *p_bake_userdata) {
	RID denoise_params_buffer = p_rd->uniform_buffer_create(sizeof(DenoiseParams));
	DenoiseParams denoise_params;
	denoise_params.spatial_bandwidth = 5.0f;
	denoise_params.light_bandwidth = p_denoiser_strength;
	denoise_params.albedo_bandwidth = 1.0f;
	denoise_params.normal_bandwidth = 0.1f;
	denoise_params.filter_strength = 10.0f;
	denoise_params.half_search_window = p_denoiser_range;
	denoise_params.slice_count = p_bake_sh ? 4 : 1;
	p_rd->buffer_update(denoise_params_buffer, 0, sizeof(DenoiseParams), &denoise_params);

	const uint32_t coefficient_count = p_bake_sh ? 4 : 1;
	RID source_slice = p_rd->texture_create_shared_from_slice(RD::TextureView(), p_source_light_tex, p_atlas_slice * coefficient_count, 0, 1, RD::TEXTURE_SLICE_2D_ARRAY, coefficient_count);
	RID dest_slice = p_rd->texture_create_shared_from_slice(RD::TextureView(), p_dest_light_tex, 0, 0, 1, RD::TEXTURE_SLICE_2D_ARRAY, coefficient_count);
	Vector<RD::Uniform> uniforms = dilate_or_denoise_common_uniforms(source_slice, dest_slice);
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.binding = 2;
		u.append_id(p_source_normal_tex);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.binding = 3;
		u.append_id(p_unocclude_tex);
		uniforms.push_back(u);
	}
	{
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.binding = 4;
		u.append_id(denoise_params_buffer);
		uniforms.push_back(u);
	}

	RID compute_shader_denoise = p_rd->shader_create_from_spirv(p_compute_shader->get_spirv_stages("denoise"));
	ERR_FAIL_COND_V(compute_shader_denoise.is_null(), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);

	RID compute_shader_denoise_pipeline = p_rd->compute_pipeline_create(compute_shader_denoise);
	RID denoise_uniform_set = p_rd->uniform_set_create(uniforms, compute_shader_denoise, 1);

	// We denoise in fixed size regions and synchronize execution to avoid GPU timeouts.
	// We use a region with 1/4 the amount of pixels if we're denoising SH lightmaps, as
	// all four of them are denoised in the shader in one dispatch.
	const int user_region_size = Math::nearest_power_of_2_templated(int(GLOBAL_GET("rendering/lightmapping/bake_performance/region_size")));
	const int max_region_size = p_bake_sh ? user_region_size / 2 : user_region_size;
	int x_regions = Math::division_round_up(p_atlas_size.width, max_region_size);
	int y_regions = Math::division_round_up(p_atlas_size.height, max_region_size);
	{
		p_push_constant.atlas_slice = 0;
		p_push_constant.geometry_slice = 0;
		p_push_constant.material_slice = p_material_slice;

		for (int i = 0; i < x_regions; i++) {
			for (int j = 0; j < y_regions; j++) {
				int x = i * max_region_size;
				int y = j * max_region_size;
				int w = MIN((i + 1) * max_region_size, p_atlas_size.width) - x;
				int h = MIN((j + 1) * max_region_size, p_atlas_size.height) - y;
				p_push_constant.region_ofs[0] = x;
				p_push_constant.region_ofs[1] = y;

				RD::ComputeListID compute_list = p_rd->compute_list_begin();
				p_rd->compute_list_bind_compute_pipeline(compute_list, compute_shader_denoise_pipeline);
				p_rd->compute_list_bind_uniform_set(compute_list, p_compute_base_uniform_set, 0);
				p_rd->compute_list_bind_uniform_set(compute_list, denoise_uniform_set, 1);
				p_rd->compute_list_set_push_constant(compute_list, &p_push_constant, sizeof(PushConstant));
				p_rd->compute_list_dispatch(compute_list, Math::division_round_up(w, 8), Math::division_round_up(h, 8), 1);
				p_rd->compute_list_end();

				p_rd->submit();
				p_rd->sync();
			}
		}
		if (p_step_function) {
			int percent = (p_atlas_slice + 1) * 100 / p_atlas_slices;
			float p = float(p_atlas_slice) / p_atlas_slices * 0.1;
			if (p_step_function(0.8 + p, vformat(RTR("Denoising %d%%"), percent), p_bake_userdata, false)) {
				return BAKE_ERROR_USER_ABORTED;
			}
		}
	}
	for (uint32_t coefficient = 0; coefficient < coefficient_count; coefficient++) {
		p_rd->texture_copy(p_dest_light_tex, p_source_light_tex, Vector3(), Vector3(), Vector3(p_atlas_size.width, p_atlas_size.height, 1), 0, 0, coefficient, p_atlas_slice * coefficient_count + coefficient);
	}

	p_rd->free_rid(compute_shader_denoise);
	p_rd->free_rid(denoise_params_buffer);
	p_rd->free_rid(source_slice);
	p_rd->free_rid(dest_slice);

	return BAKE_OK;
}

LightmapperRD::BakeError LightmapperRD::bake(BakeQuality p_quality, bool p_use_denoiser, float p_denoiser_strength, int p_denoiser_range, int p_bounces, float p_bounce_indirect_energy, float p_bias, bool p_bake_ao, float p_ao_distance, float p_ao_strength, float p_ao_light_affect, int p_ao_samples, int p_max_texture_size, bool p_bake_sh, bool p_bake_shadowmask, bool p_texture_for_bounces, GenerateProbes p_generate_probes, const Ref<Image> &p_environment_panorama, const Basis &p_environment_transform, BakeStepFunc p_step_function, void *p_bake_userdata, float p_exposure_normalization, float p_supersampling_factor) {
	int denoiser = GLOBAL_GET("rendering/lightmapping/denoising/denoiser");
	String oidn_path = EDITOR_GET("filesystem/tools/oidn/oidn_denoise_path");
	static const char *oidn_devices[] = { "default", "cpu", "sycl", "cuda", "hip", "metal" };
	int oidn_device_index = GLOBAL_GET("rendering/lightmapping/denoising/oidn_device");
	String oidn_device = oidn_devices[CLAMP(oidn_device_index, 0, 5)];

	if (p_use_denoiser && denoiser == 1) {
		// OIDN (external).
		Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);

		if (da->dir_exists(oidn_path)) {
			if (OS::get_singleton()->get_name() == "Windows") {
				oidn_path = oidn_path.path_join("oidnDenoise.exe");
			} else {
				oidn_path = oidn_path.path_join("oidnDenoise");
			}
		}
		ERR_FAIL_COND_V_MSG(oidn_path.is_empty() || !da->file_exists(oidn_path), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES, "OIDN denoiser is selected in the project settings, but no or invalid OIDN executable path is configured in the editor settings.");
	}

	if (p_step_function) {
		p_step_function(0.0, RTR("Begin Bake"), p_bake_userdata, true);
	}
	lightmap_textures.clear();
	shadowmask_textures.clear();
	int grid_size = 128;
	RenderingContextDriver *rcd = nullptr;
	RenderingDevice *rd = _create_lightmapper_device(rcd);
	ERR_FAIL_NULL_V(rd, BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
	Error err;

	/* STEP 1: Fetch material textures and compute the bounds */

	AABB bounds;
	Size2i atlas_size;
	int atlas_slices;
	RID albedo_array_tex;
	RID emission_array_tex;

	BakeError bake_error = _blit_meshes_into_atlas(rd, albedo_array_tex, emission_array_tex, p_max_texture_size, p_denoiser_range, p_use_denoiser && denoiser == 1, bounds, atlas_size, atlas_slices, p_supersampling_factor, p_step_function, p_bake_userdata);
	if (bake_error != BAKE_OK) {
		if (albedo_array_tex.is_valid()) {
			rd->free_rid(albedo_array_tex);
		}
		if (emission_array_tex.is_valid()) {
			rd->free_rid(emission_array_tex);
		}
		memdelete(rd);
		memdelete(rcd);
		return bake_error;
	}

	// Find any directional light suitable for shadowmasking.
	if (p_bake_shadowmask) {
		bool found = false;
		for (int i = 0; i < lights.size(); i++) {
			if (lights[i].type == LightType::LIGHT_TYPE_DIRECTIONAL && !lights[i].static_bake) {
				found = true;
				break;
			}
		}

		if (!found) {
			p_bake_shadowmask = false;
			WARN_PRINT("Shadowmask disabled: no directional light with their bake mode set to dynamic exists.");
		}
	}

#ifdef DEBUG_TEXTURES
	if (area_light_atlas.mipmap_count > 1) {
		int64_t mip0size = 4 * area_light_atlas.size.width * area_light_atlas.size.height;
		int64_t start = 0;
		int64_t end = mip0size;
		for (int m = 0; m < area_light_atlas.mipmap_count; m++) {
			Ref<Image> img = Image::create_from_data(area_light_atlas.size.width / pow(2, m), area_light_atlas.size.height / pow(2, m), false, Image::FORMAT_RGBA8, area_light_atlas.atlas_data.slice(start, end));
			img->save_png("res://0_area_light_atlas_m" + itos(m) + ".png");
			start += mip0size / pow(4, m);
			end += mip0size / pow(4, m + 1);
		}
	}
#endif
	RID normal_tex;
	RID mesh_tex;
	RID oidn_normal_tex;
	RID position_tex;
	RID unocclude_tex;
	RID invalid_samples_tex;
	RID light_source_tex;
	RID light_accum_tex;
	RID light_accum_tex2;
	RID direct_light_tex;
	RID light_environment_tex;
	RID area_light_atlas_tex;
	RID shadowmask_tex;
	RID shadowmask_tex2;

#define FREE_TEXTURES \
	if (oidn_normal_tex.is_valid()) { \
		rd->free_rid(oidn_normal_tex); \
	} \
	if (albedo_array_tex.is_valid()) { \
		rd->free_rid(albedo_array_tex); \
	} \
	if (emission_array_tex.is_valid()) { \
		rd->free_rid(emission_array_tex); \
	} \
	if (normal_tex.is_valid()) { \
		rd->free_rid(normal_tex); \
	} \
	if (mesh_tex.is_valid()) { \
		rd->free_rid(mesh_tex); \
	} \
	if (position_tex.is_valid()) { \
		rd->free_rid(position_tex); \
	} \
	if (unocclude_tex.is_valid()) { \
		rd->free_rid(unocclude_tex); \
	} \
	if (invalid_samples_tex.is_valid()) { \
		rd->free_rid(invalid_samples_tex); \
	} \
	if (light_source_tex.is_valid()) { \
		rd->free_rid(light_source_tex); \
	} \
	if (light_accum_tex2.is_valid()) { \
		rd->free_rid(light_accum_tex2); \
	} \
	if (light_accum_tex.is_valid()) { \
		rd->free_rid(light_accum_tex); \
	} \
	if (light_environment_tex.is_valid()) { \
		rd->free_rid(light_environment_tex); \
	} \
	if (area_light_atlas_tex.is_valid()) { \
		rd->free_rid(area_light_atlas_tex); \
	} \
	if (direct_light_tex.is_valid()) { \
		rd->free_rid(direct_light_tex); \
	} \
	if (p_bake_shadowmask) { \
		if (shadowmask_tex.is_valid()) { \
			rd->free_rid(shadowmask_tex); \
		} \
		if (shadowmask_tex2.is_valid()) { \
			rd->free_rid(shadowmask_tex2); \
		} \
	}

	{ // create all textures
		RD::TextureFormat tf;
		tf.width = atlas_size.width;
		tf.height = atlas_size.height;
		tf.array_layers = 1;
		tf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;

		//this will be rastered to
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		normal_tex = rd->texture_create(tf, RD::TextureView());
		tf.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		position_tex = rd->texture_create(tf, RD::TextureView());
		unocclude_tex = rd->texture_create(tf, RD::TextureView());
		tf.format = RD::DATA_FORMAT_R32_UINT;
		tf.array_layers = atlas_slices;
		mesh_tex = rd->texture_create(tf, RD::TextureView());
		invalid_samples_tex = rd->texture_create(tf, RD::TextureView());
		rd->texture_clear(invalid_samples_tex, Color(0, 0, 0, 0), 0, 1, 0, atlas_slices);

		tf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;

		// Global outputs and ray-hit inputs use every atlas slice.
		tf.array_layers = atlas_slices;

		// shadowmask
		if (p_bake_shadowmask) {
			tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;

			shadowmask_tex = rd->texture_create(tf, RD::TextureView());
			rd->texture_clear(shadowmask_tex, Color(0, 0, 0, 0), 0, 1, 0, atlas_slices);

			shadowmask_tex2 = rd->texture_create(tf, RD::TextureView());
			rd->texture_clear(shadowmask_tex2, Color(0, 0, 0, 0), 0, 1, 0, atlas_slices);
		}

		// lightmap
		// Direct light is generated one slice at a time in a storage-compatible
		// format, then packed into the global read-only lookup texture.
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tf.array_layers = 1;

		light_source_tex = rd->texture_create(tf, RD::TextureView());
		rd->texture_clear(light_source_tex, Color(0, 0, 0, 0), 0, 1, 0, 1);

		// Accumulation only writes the current atlas slice. Keep one reusable set
		// of coefficients while tracing and stage completed slices for later.
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tf.array_layers = p_bake_sh ? 4 : 1;
		light_accum_tex = rd->texture_create(tf, RD::TextureView());
		rd->texture_clear(light_accum_tex, Color(0, 0, 0, 0), 0, 1, 0, tf.array_layers);
		direct_light_tex = rd->texture_create(tf, RD::TextureView());
		rd->texture_clear(direct_light_tex, Color(0, 0, 0, 0), 0, 1, 0, tf.array_layers);
		// Direct and indirect passes require this binding, but do not write it.
		// Keep a one-layer placeholder until post-processing needs real scratch.
		RD::TextureFormat placeholder_format = tf;
		placeholder_format.array_layers = 1;
		light_accum_tex2 = rd->texture_create(placeholder_format, RD::TextureView());

		//env
		{
			Ref<Image> panorama_tex;
			if (p_environment_panorama.is_valid()) {
				panorama_tex = p_environment_panorama;
				panorama_tex->convert(Image::FORMAT_RGBAF);
			} else {
				panorama_tex.instantiate();
				panorama_tex->initialize_data(8, 8, false, Image::FORMAT_RGBAF);
				panorama_tex->fill(Color(0, 0, 0, 1));
			}

			RD::TextureFormat tfp;
			tfp.width = panorama_tex->get_width();
			tfp.height = panorama_tex->get_height();
			tfp.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
			tfp.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;

			Vector<Vector<uint8_t>> tdata;
			tdata.push_back(panorama_tex->get_data());
			light_environment_tex = rd->texture_create(tfp, RD::TextureView(), tdata);

#ifdef DEBUG_TEXTURES
			panorama_tex->save_exr("res://0_panorama.exr", false);
#endif
		}

		// area lights
		{
			RD::TextureFormat tformat;
			tformat.width = area_light_atlas.size.width;
			tformat.height = area_light_atlas.size.height;
			tformat.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
			tformat.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
			tformat.mipmaps = area_light_atlas.mipmap_count;

			// now fill mipmap with data from Vector<Ref<Image>> area_light_atlas.images
			Vector<Vector<uint8_t>> tdata;
			tdata.push_back(area_light_atlas.atlas_data);
			area_light_atlas_tex = rd->texture_create(tformat, RD::TextureView(), tdata);
		}
	}

	/* STEP 2: create the acceleration structure for the GPU*/

	Vector<int> slice_triangle_count;
	RID bake_parameters_buffer;
	RID vertex_buffer;
	RID triangle_buffer;
	RID lights_buffer;
	RID triangle_indices_buffer;
	RID cluster_indices_buffer;
	RID cluster_aabbs_buffer;
	RID grid_texture;
	RID seams_buffer;
	RID probe_positions_buffer;

	Vector<int> slice_seam_count;
	Vector<Vector<int>> slice_seam_sources;

#define FREE_BUFFERS \
	rd->free_rid(bake_parameters_buffer); \
	rd->free_rid(vertex_buffer); \
	rd->free_rid(triangle_buffer); \
	rd->free_rid(lights_buffer); \
	rd->free_rid(triangle_indices_buffer); \
	rd->free_rid(cluster_indices_buffer); \
	rd->free_rid(cluster_aabbs_buffer); \
	rd->free_rid(grid_texture); \
	rd->free_rid(seams_buffer); \
	rd->free_rid(probe_positions_buffer);

	const uint32_t cluster_size = 16;
	_create_acceleration_structures(rd, atlas_size, atlas_slices, bounds, grid_size, cluster_size, probe_positions, p_generate_probes, slice_triangle_count, slice_seam_count, slice_seam_sources, vertex_buffer, triangle_buffer, lights_buffer, triangle_indices_buffer, cluster_indices_buffer, cluster_aabbs_buffer, probe_positions_buffer, grid_texture, seams_buffer, p_step_function, p_bake_userdata);

	// The index of the directional light used for shadowmasking.
	int shadowmask_light_idx = -1;

	// Find the directional light index in the sorted lights array.
	if (p_bake_shadowmask) {
		int shadowmask_lights_count = 0;

		for (int i = 0; i < lights.size(); i++) {
			if (lights[i].type == LightType::LIGHT_TYPE_DIRECTIONAL && !lights[i].static_bake) {
				if (shadowmask_light_idx < 0) {
					shadowmask_light_idx = i;
				}
				shadowmask_lights_count += 1;
			}
		}

		if (shadowmask_lights_count > 1) {
			WARN_PRINT(
					vformat("%d directional lights detected for shadowmask baking. Only %s will be used.",
							shadowmask_lights_count, light_metadata[shadowmask_light_idx].name));
		}
	}

	// Create global bake parameters buffer.
	BakeParameters bake_parameters;
	bake_parameters.world_size[0] = bounds.size.x;
	bake_parameters.world_size[1] = bounds.size.y;
	bake_parameters.world_size[2] = bounds.size.z;
	bake_parameters.bias = p_bias;
	bake_parameters.to_cell_offset[0] = bounds.position.x;
	bake_parameters.to_cell_offset[1] = bounds.position.y;
	bake_parameters.to_cell_offset[2] = bounds.position.z;
	bake_parameters.grid_size = grid_size;
	bake_parameters.to_cell_size[0] = (1.0 / bounds.size.x) * float(grid_size);
	bake_parameters.to_cell_size[1] = (1.0 / bounds.size.y) * float(grid_size);
	bake_parameters.to_cell_size[2] = (1.0 / bounds.size.z) * float(grid_size);
	bake_parameters.light_count = lights.size();
	bake_parameters.env_transform[0] = p_environment_transform.rows[0][0];
	bake_parameters.env_transform[1] = p_environment_transform.rows[1][0];
	bake_parameters.env_transform[2] = p_environment_transform.rows[2][0];
	bake_parameters.env_transform[3] = 0.0f;
	bake_parameters.env_transform[4] = p_environment_transform.rows[0][1];
	bake_parameters.env_transform[5] = p_environment_transform.rows[1][1];
	bake_parameters.env_transform[6] = p_environment_transform.rows[2][1];
	bake_parameters.env_transform[7] = 0.0f;
	bake_parameters.env_transform[8] = p_environment_transform.rows[0][2];
	bake_parameters.env_transform[9] = p_environment_transform.rows[1][2];
	bake_parameters.env_transform[10] = p_environment_transform.rows[2][2];
	bake_parameters.env_transform[11] = 0.0f;
	bake_parameters.atlas_size[0] = atlas_size.width;
	bake_parameters.atlas_size[1] = atlas_size.height;
	bake_parameters.exposure_normalization = p_exposure_normalization;
	bake_parameters.bounces = p_bounces;
	bake_parameters.bounce_indirect_energy = p_bounce_indirect_energy;
	bake_parameters.shadowmask_light_idx = shadowmask_light_idx;
	// Same number of rays for transparency regardless of quality (it's more of a retry rather than shooting new ones).
	bake_parameters.transparency_rays = GLOBAL_GET("rendering/lightmapping/bake_performance/max_transparency_rays");
	bake_parameters.supersampling_factor = p_supersampling_factor;
	bake_parameters.ao_distance = p_ao_distance;
	bake_parameters.ao_strength = p_ao_strength;
	bake_parameters.ao_light_affect = p_ao_light_affect;

	bake_parameters_buffer = rd->uniform_buffer_create(sizeof(BakeParameters));
	rd->buffer_update(bake_parameters_buffer, 0, sizeof(BakeParameters), &bake_parameters);

	if (p_step_function) {
		if (p_step_function(0.47, RTR("Preparing shaders"), p_bake_userdata, true)) {
			FREE_TEXTURES
			FREE_BUFFERS
			memdelete(rd);
			memdelete(rcd);
			return BAKE_ERROR_USER_ABORTED;
		}
	}

	//shaders
	Ref<RDShaderFile> raster_shader;
	raster_shader.instantiate();
	err = raster_shader->parse_versions_from_text(lm_raster_shader_glsl);
	if (err != OK) {
		raster_shader->print_errors("raster_shader");

		FREE_TEXTURES
		FREE_BUFFERS

		memdelete(rd);

		memdelete(rcd);
	}
	ERR_FAIL_COND_V(err != OK, BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);

	RID rasterize_shader = rd->shader_create_from_spirv(raster_shader->get_spirv_stages());

	ERR_FAIL_COND_V(rasterize_shader.is_null(), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES); //this is a bug check, though, should not happen

	RID sampler;
	{
		RD::SamplerState s;
		s.mag_filter = RD::SAMPLER_FILTER_LINEAR;
		s.min_filter = RD::SAMPLER_FILTER_LINEAR;
		s.max_lod = 0;

		sampler = rd->sampler_create(s);
	}

	RID area_light_atlas_sampler;
	{
		RD::SamplerState s;
		s.mag_filter = RD::SAMPLER_FILTER_LINEAR;
		s.min_filter = RD::SAMPLER_FILTER_LINEAR;
		s.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		s.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;

		area_light_atlas_sampler = rd->sampler_create(s);
	}

	Vector<RD::Uniform> base_uniforms;
	{
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
			u.binding = 0;
			u.append_id(bake_parameters_buffer);
			base_uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 1;
			u.append_id(vertex_buffer);
			base_uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 2;
			u.append_id(triangle_buffer);
			base_uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 3;
			u.append_id(triangle_indices_buffer);
			base_uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 4;
			u.append_id(lights_buffer);
			base_uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 5;
			u.append_id(seams_buffer);
			base_uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 6;
			u.append_id(probe_positions_buffer);
			base_uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.binding = 7;
			u.append_id(grid_texture);
			base_uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.binding = 8;
			u.append_id(albedo_array_tex);
			base_uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			u.binding = 9;
			u.append_id(emission_array_tex);
			base_uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			u.binding = 10;
			u.append_id(sampler);
			base_uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			u.binding = 11;
			u.append_id(area_light_atlas_sampler);
			base_uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 12;
			u.append_id(cluster_indices_buffer);
			base_uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 13;
			u.append_id(cluster_aabbs_buffer);
			base_uniforms.push_back(u);
		}
	}

	RID raster_base_uniform = rd->uniform_set_create(base_uniforms, rasterize_shader, 0);
	RID raster_depth_buffer;
	{
		RD::TextureFormat tf;
		tf.width = atlas_size.width;
		tf.height = atlas_size.height;
		tf.depth = 1;
		tf.texture_type = RD::TEXTURE_TYPE_2D;
		tf.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		tf.format = RD::DATA_FORMAT_D32_SFLOAT;
		tf.is_discardable = true;

		raster_depth_buffer = rd->texture_create(tf, RD::TextureView());
	}

	rd->submit();
	rd->sync();

	/* STEP 3: Geometry is rasterized into reusable one-layer textures as each
	 * atlas slice is processed. */

#define FREE_RASTER_RESOURCES \
	rd->free_rid(rasterize_shader); \
	rd->free_rid(sampler); \
	rd->free_rid(area_light_atlas_sampler); \
	rd->free_rid(raster_depth_buffer);

	/* Plot direct light */

	Ref<RDShaderFile> compute_shader;
	String defines = "";
	defines += "\n#define CLUSTER_SIZE " + uitos(cluster_size) + "\n";

	if (p_bake_sh) {
		defines += "\n#define USE_SH_LIGHTMAPS\n";
	}

	if (p_texture_for_bounces) {
		defines += "\n#define USE_LIGHT_TEXTURE_FOR_BOUNCES\n";
	}

	if (p_bake_shadowmask) {
		defines += "\n#define USE_SHADOWMASK\n";
	}

	compute_shader.instantiate();
	err = compute_shader->parse_versions_from_text(lm_compute_shader_glsl, defines);
	if (err != OK) {
		FREE_TEXTURES
		FREE_BUFFERS
		FREE_RASTER_RESOURCES
		memdelete(rd);

		memdelete(rcd);

		compute_shader->print_errors("compute_shader");
	}
	ERR_FAIL_COND_V(err != OK, BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);

	// Unoccluder
	RID compute_shader_unocclude = rd->shader_create_from_spirv(compute_shader->get_spirv_stages("unocclude"));
	ERR_FAIL_COND_V(compute_shader_unocclude.is_null(), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES); // internal check, should not happen
	RID compute_shader_unocclude_pipeline = rd->compute_pipeline_create(compute_shader_unocclude);

	// Direct light
	RID compute_shader_primary = rd->shader_create_from_spirv(compute_shader->get_spirv_stages("primary"));
	ERR_FAIL_COND_V(compute_shader_primary.is_null(), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES); // internal check, should not happen
	RID compute_shader_primary_pipeline = rd->compute_pipeline_create(compute_shader_primary);

	// Indirect light
	RID compute_shader_secondary = rd->shader_create_from_spirv(compute_shader->get_spirv_stages("secondary"));
	ERR_FAIL_COND_V(compute_shader_secondary.is_null(), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES); //internal check, should not happen
	RID compute_shader_secondary_pipeline = rd->compute_pipeline_create(compute_shader_secondary);

	// Light probes
	RID compute_shader_light_probes = rd->shader_create_from_spirv(compute_shader->get_spirv_stages("light_probes"));
	ERR_FAIL_COND_V(compute_shader_light_probes.is_null(), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES); //internal check, should not happen
	RID compute_shader_light_probes_pipeline = rd->compute_pipeline_create(compute_shader_light_probes);

	RID compute_shader_ao;
	RID compute_shader_ao_pipeline;
	if (p_bake_ao) {
		compute_shader_ao = rd->shader_create_from_spirv(compute_shader->get_spirv_stages("ambient_occlusion"));
		ERR_FAIL_COND_V(compute_shader_ao.is_null(), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);
		compute_shader_ao_pipeline = rd->compute_pipeline_create(compute_shader_ao);
	}

	RID compute_base_uniform_set = rd->uniform_set_create(base_uniforms, compute_shader_primary, 0);
	auto create_dummy_material_texture = [&](RD::DataFormat p_format) -> RID {
		RD::TextureFormat format;
		format.width = 1;
		format.height = 1;
		format.array_layers = 1;
		format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT;
		format.format = p_format;
		return rd->texture_create(format, RD::TextureView());
	};
	auto set_base_texture = [&](uint32_t p_binding, RID p_texture) {
		for (RD::Uniform &uniform : base_uniforms) {
			if (uniform.binding == p_binding) {
				uniform.set_id(0, p_texture);
				return;
			}
		}
	};
	auto free_base_uniform_sets = [&]() {
		if (compute_base_uniform_set.is_valid()) {
			rd->free_rid(compute_base_uniform_set);
			compute_base_uniform_set = RID();
		}
		if (raster_base_uniform.is_valid()) {
			rd->free_rid(raster_base_uniform);
			raster_base_uniform = RID();
		}
	};
	auto recreate_base_uniform_sets = [&]() -> bool {
		free_base_uniform_sets();
		raster_base_uniform = rd->uniform_set_create(base_uniforms, rasterize_shader, 0);
		compute_base_uniform_set = rd->uniform_set_create(base_uniforms, compute_shader_primary, 0);
		return raster_base_uniform.is_valid() && compute_base_uniform_set.is_valid();
	};

#define FREE_COMPUTE_RESOURCES \
	rd->free_rid(compute_shader_unocclude); \
	rd->free_rid(compute_shader_primary); \
	rd->free_rid(compute_shader_secondary); \
	rd->free_rid(compute_shader_light_probes); \
	if (p_bake_ao) { \
		rd->free_rid(compute_shader_ao); \
	}

	const Vector3i geometry_group_size(Math::division_round_up(atlas_size.x, 8), Math::division_round_up(atlas_size.y, 8), 1);
	rd->submit();
	rd->sync();

	if (p_step_function) {
		if (p_step_function(0.49, RTR("Un-occluding geometry"), p_bake_userdata, true)) {
			FREE_TEXTURES
			FREE_BUFFERS
			FREE_RASTER_RESOURCES
			FREE_COMPUTE_RESOURCES
			memdelete(rd);
			memdelete(rcd);
			return BAKE_ERROR_USER_ABORTED;
		}
	}

	PushConstant push_constant = {};
	push_constant.denoiser_range = p_use_denoiser ? p_denoiser_range : 1.0;

	/* Raster and unocclude one atlas slice into the reusable geometry textures. */
	auto prepare_geometry_slice = [&](int p_slice) {
		_raster_geometry_slice(rd, atlas_size, p_slice, grid_size, bounds, p_bias, slice_triangle_count, position_tex, unocclude_tex, normal_tex, mesh_tex, raster_depth_buffer, rasterize_shader, raster_base_uniform);
		Vector<RD::Uniform> uniforms;
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.binding = 0;
			u.append_id(position_tex);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.binding = 1;
			u.append_id(unocclude_tex);
			uniforms.push_back(u);
		}

		RID unocclude_uniform_set = rd->uniform_set_create(uniforms, compute_shader_unocclude, 1);

		RD::ComputeListID compute_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(compute_list, compute_shader_unocclude_pipeline);
		rd->compute_list_bind_uniform_set(compute_list, compute_base_uniform_set, 0);
		rd->compute_list_bind_uniform_set(compute_list, unocclude_uniform_set, 1);

		push_constant.atlas_slice = p_slice;
		push_constant.geometry_slice = 0;
		// Geometry preparation covers the whole atlas, not the last ray tile.
		push_constant.region_ofs[0] = 0;
		push_constant.region_ofs[1] = 0;
		rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		rd->compute_list_dispatch(compute_list, geometry_group_size.x, geometry_group_size.y, geometry_group_size.z);
		rd->compute_list_end(); //done
		rd->free_rid(unocclude_uniform_set);
	};

	if (p_step_function) {
		if (p_step_function(0.5, RTR("Plot direct lighting"), p_bake_userdata, true)) {
			FREE_TEXTURES
			FREE_BUFFERS
			FREE_RASTER_RESOURCES
			FREE_COMPUTE_RESOURCES
			memdelete(rd);
			memdelete(rcd);
			return BAKE_ERROR_USER_ABORTED;
		}
	}

	const int max_region_size = Math::nearest_power_of_2_templated(int(GLOBAL_GET("rendering/lightmapping/bake_performance/region_size")));
	const int x_regions = Math::division_round_up(atlas_size.width, max_region_size);
	const int y_regions = Math::division_round_up(atlas_size.height, max_region_size);

	// Set ray count to the quality used for direct light and bounces.
	switch (p_quality) {
		case BAKE_QUALITY_LOW: {
			push_constant.ray_count = GLOBAL_GET("rendering/lightmapping/bake_quality/low_quality_ray_count");
		} break;
		case BAKE_QUALITY_MEDIUM: {
			push_constant.ray_count = GLOBAL_GET("rendering/lightmapping/bake_quality/medium_quality_ray_count");
		} break;
		case BAKE_QUALITY_HIGH: {
			push_constant.ray_count = GLOBAL_GET("rendering/lightmapping/bake_quality/high_quality_ray_count");
		} break;
		case BAKE_QUALITY_ULTRA: {
			push_constant.ray_count = GLOBAL_GET("rendering/lightmapping/bake_quality/ultra_quality_ray_count");
		} break;
	}

	push_constant.ray_count = CLAMP(push_constant.ray_count, 16u, 8192u);
	const uint32_t lighting_ray_count = push_constant.ray_count;
	const int light_coefficients = p_bake_sh ? 4 : 1;
	struct StagedLightFiles {
		Vector<String> paths;
		Vector<String> source_paths;
		Vector<String> direct_paths;
		~StagedLightFiles() {
			for (const String &path : direct_paths) {
				DirAccess::remove_absolute(path);
			}
			for (const String &path : paths) {
				DirAccess::remove_absolute(path);
			}
			for (const String &path : source_paths) {
				DirAccess::remove_absolute(path);
			}
		}
	} staged_light_files;
	staged_light_files.paths.resize(atlas_slices * light_coefficients);
	const String staged_light_prefix = EditorPaths::get_singleton()->get_cache_dir().path_join(vformat("lightmap_accum_%d_", OS::get_singleton()->get_process_id()));
	for (int layer = 0; layer < staged_light_files.paths.size(); layer++) {
		staged_light_files.paths.write[layer] = staged_light_prefix + itos(layer);
	}
	staged_light_files.source_paths.resize(atlas_slices);
	const String staged_source_prefix = EditorPaths::get_singleton()->get_cache_dir().path_join(vformat("lightmap_source_%d_", OS::get_singleton()->get_process_id()));
	for (int slice = 0; slice < atlas_slices; slice++) {
		staged_light_files.source_paths.write[slice] = staged_source_prefix + itos(slice);
	}
	if (p_bake_ao) {
		staged_light_files.direct_paths.resize(atlas_slices * light_coefficients);
		for (int layer = 0; layer < staged_light_files.direct_paths.size(); layer++) {
			staged_light_files.direct_paths.write[layer] = staged_light_prefix + "direct_" + itos(layer);
		}
	}
	auto store_staged_light_layer = [&](int p_layer, const Vector<uint8_t> &p_data) -> bool {
		Ref<FileAccess> file = FileAccess::open(staged_light_files.paths[p_layer], FileAccess::WRITE);
		return file.is_valid() && file->store_buffer(p_data);
	};
	auto load_staged_light_slice = [&](int p_slice, RID p_texture) -> bool {
		for (int coefficient = 0; coefficient < light_coefficients; coefficient++) {
			if (rd->texture_update(p_texture, coefficient, FileAccess::get_file_as_bytes(staged_light_files.paths[p_slice * light_coefficients + coefficient])) != OK) {
				return false;
			}
		}
		return true;
	};
	auto store_staged_light_slice = [&](int p_slice, RID p_texture) -> bool {
		for (int coefficient = 0; coefficient < light_coefficients; coefficient++) {
			if (!store_staged_light_layer(p_slice * light_coefficients + coefficient, rd->texture_get_data(p_texture, coefficient))) {
				return false;
			}
		}
		return true;
	};

	/* PRIMARY (direct) LIGHT PASS */
	{
		Vector<RD::Uniform> uniforms;
		{
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
				u.binding = 0;
				u.append_id(light_source_tex);
				uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 1;
				u.append_id(light_accum_tex2); // Will be unused.
				uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
				u.binding = 7;
				u.append_id(direct_light_tex);
				uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 2;
				u.append_id(position_tex);
				uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 3;
				u.append_id(normal_tex);
				uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
				u.binding = 4;
				u.append_id(light_accum_tex);
				uniforms.push_back(u);
			}

			if (p_bake_shadowmask) {
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
				u.binding = 5;
				u.append_id(shadowmask_tex);
				uniforms.push_back(u);
			}

			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 6;
				u.append_id(area_light_atlas_tex);
				uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 8;
				u.append_id(unocclude_tex);
				uniforms.push_back(u);
			}
		}

		RID light_uniform_set = rd->uniform_set_create(uniforms, compute_shader_primary, 1);

		int count = 0;
		for (int s = 0; s < atlas_slices; s++) {
			rd->texture_clear(light_source_tex, Color(0, 0, 0, 0), 0, 1, 0, 1);
			rd->texture_clear(light_accum_tex, Color(0, 0, 0, 0), 0, 1, 0, light_coefficients);
			prepare_geometry_slice(s);
			push_constant.atlas_slice = s;
			push_constant.geometry_slice = 0;
			push_constant.output_slice = 0;

			for (int i = 0; i < x_regions; i++) {
				for (int j = 0; j < y_regions; j++) {
					int x = i * max_region_size;
					int y = j * max_region_size;
					int w = MIN((i + 1) * max_region_size, atlas_size.width) - x;
					int h = MIN((j + 1) * max_region_size, atlas_size.height) - y;

					push_constant.region_ofs[0] = x;
					push_constant.region_ofs[1] = y;

					const Vector3i group_size(Math::division_round_up(w, 8), Math::division_round_up(h, 8), 1);
					RD::ComputeListID compute_list = rd->compute_list_begin();
					rd->compute_list_bind_compute_pipeline(compute_list, compute_shader_primary_pipeline);
					rd->compute_list_bind_uniform_set(compute_list, compute_base_uniform_set, 0);
					rd->compute_list_bind_uniform_set(compute_list, light_uniform_set, 1);
					rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
					rd->compute_list_dispatch(compute_list, group_size.x, group_size.y, group_size.z);
					rd->compute_list_end();

					rd->submit();
					rd->sync();

					count++;
					if (p_step_function) {
						int total = (atlas_slices * x_regions * y_regions);
						int percent = count * 100 / total;
						float p = float(count) / total * 0.1;
						if (p_step_function(0.5 + p, vformat(RTR("Plot direct lighting %d%%"), percent), p_bake_userdata, false)) {
							FREE_TEXTURES
							FREE_BUFFERS
							FREE_RASTER_RESOURCES
							FREE_COMPUTE_RESOURCES
							memdelete(rd);
							memdelete(rcd);
							return BAKE_ERROR_USER_ABORTED;
						}
					}
				}
			}
			for (int coefficient = 0; coefficient < light_coefficients; coefficient++) {
				if (!store_staged_light_layer(s * light_coefficients + coefficient, rd->texture_get_data(light_accum_tex, coefficient))) {
					FREE_TEXTURES
					FREE_BUFFERS
					FREE_RASTER_RESOURCES
					FREE_COMPUTE_RESOURCES
					memdelete(rd);
					memdelete(rcd);
					return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
				}
			}
			// Preserve direct lighting on disk while the reusable slice accumulates bounces.
			if (p_bake_ao) {
				for (int coefficient = 0; coefficient < light_coefficients; coefficient++) {
					Ref<FileAccess> file = FileAccess::open(staged_light_files.direct_paths[s * light_coefficients + coefficient], FileAccess::WRITE);
					if (file.is_null() || !file->store_buffer(rd->texture_get_data(direct_light_tex, coefficient))) {
						FREE_TEXTURES
						FREE_BUFFERS
						FREE_RASTER_RESOURCES
						FREE_COMPUTE_RESOURCES
						memdelete(rd);
						memdelete(rcd);
						return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
					}
				}
			}
			Ref<Image> source_image = Image::create_from_data(atlas_size.width, atlas_size.height, false, Image::FORMAT_RGBAH, rd->texture_get_data(light_source_tex, 0));
			Ref<FileAccess> source_file = FileAccess::open(staged_light_files.source_paths[s], FileAccess::WRITE);
			if (source_file.is_null() || !source_file->store_buffer(_pack_r11g11b10f(source_image))) {
				FREE_TEXTURES
				FREE_BUFFERS
				FREE_RASTER_RESOURCES
				FREE_COMPUTE_RESOURCES
				memdelete(rd);
				memdelete(rcd);
				return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
			}
		}
	}
	if (light_source_tex.is_valid()) {
		rd->free_rid(light_source_tex);
	}
	{
		RD::TextureFormat source_format;
		source_format.width = atlas_size.width;
		source_format.height = atlas_size.height;
		source_format.array_layers = atlas_slices;
		source_format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
		source_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		source_format.format = RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32;
		light_source_tex = rd->texture_create(source_format, RD::TextureView());
		if (light_source_tex.is_null()) {
			FREE_TEXTURES
			FREE_BUFFERS
			FREE_RASTER_RESOURCES
			FREE_COMPUTE_RESOURCES
			memdelete(rd);
			memdelete(rcd);
			return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
		}
		for (int slice = 0; slice < atlas_slices; slice++) {
			if (rd->texture_update(light_source_tex, slice, FileAccess::get_file_as_bytes(staged_light_files.source_paths[slice])) != OK) {
				FREE_TEXTURES
				FREE_BUFFERS
				FREE_RASTER_RESOURCES
				FREE_COMPUTE_RESOURCES
				memdelete(rd);
				memdelete(rcd);
				return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
			}
			DirAccess::remove_absolute(staged_light_files.source_paths[slice]);
		}
		staged_light_files.source_paths.clear();
	}

#ifdef DEBUG_TEXTURES

	if (p_bake_sh) {
		for (int i = 0; i < atlas_slices * 4; i++) {
			Vector<uint8_t> s = FileAccess::get_file_as_bytes(staged_light_files.paths[i]);
			Ref<Image> img = Image::create_from_data(atlas_size.width, atlas_size.height, false, Image::FORMAT_RGBAH, s);
			img->save_exr("res://2_light_primary_accum_" + itos(i) + ".exr", false);
		}
	}
#endif

	/* SECONDARY (indirect) LIGHT PASS(ES) */

	if (p_bounces > 0) {
		Vector<RD::Uniform> uniforms;
		{
			{
				// Unused.
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
				u.binding = 0;
				u.append_id(light_accum_tex2);
				uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 1;
				u.append_id(light_source_tex);
				uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 2;
				u.append_id(position_tex);
				uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 3;
				u.append_id(normal_tex);
				uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
				u.binding = 4;
				u.append_id(light_accum_tex);
				uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 5;
				u.append_id(light_environment_tex);
				uniforms.push_back(u);
			}

			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 6;
				u.append_id(area_light_atlas_tex);
				uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
				u.binding = 7;
				u.append_id(invalid_samples_tex);
				uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 8;
				u.append_id(unocclude_tex);
				uniforms.push_back(u);
			}
		}

		RID secondary_uniform_set;
		secondary_uniform_set = rd->uniform_set_create(uniforms, compute_shader_secondary, 1);

		const int max_rays = GLOBAL_GET("rendering/lightmapping/bake_performance/max_rays_per_pass");
		int ray_iterations = Math::division_round_up((int32_t)push_constant.ray_count, max_rays);

		if (p_step_function) {
			if (p_step_function(0.6, RTR("Integrate indirect lighting"), p_bake_userdata, true)) {
				FREE_TEXTURES
				FREE_BUFFERS
				FREE_RASTER_RESOURCES
				FREE_COMPUTE_RESOURCES
				memdelete(rd);
				memdelete(rcd);
				return BAKE_ERROR_USER_ABORTED;
			}
		}

		int count = 0;
		for (int s = 0; s < atlas_slices; s++) {
			for (int coefficient = 0; coefficient < light_coefficients; coefficient++) {
				if (rd->texture_update(light_accum_tex, coefficient, FileAccess::get_file_as_bytes(staged_light_files.paths[s * light_coefficients + coefficient])) != OK) {
					FREE_TEXTURES
					FREE_BUFFERS
					FREE_RASTER_RESOURCES
					FREE_COMPUTE_RESOURCES
					memdelete(rd);
					memdelete(rcd);
					return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
				}
			}
			prepare_geometry_slice(s);
			rd->texture_clear(invalid_samples_tex, Color(0, 0, 0, 0), 0, 1, s, 1);
			push_constant.atlas_slice = s;
			push_constant.geometry_slice = 0;
			push_constant.output_slice = 0;

			for (int i = 0; i < x_regions; i++) {
				for (int j = 0; j < y_regions; j++) {
					int x = i * max_region_size;
					int y = j * max_region_size;
					int w = MIN((i + 1) * max_region_size, atlas_size.width) - x;
					int h = MIN((j + 1) * max_region_size, atlas_size.height) - y;

					push_constant.region_ofs[0] = x;
					push_constant.region_ofs[1] = y;

					const Vector3i group_size(Math::division_round_up(w, 8), Math::division_round_up(h, 8), 1);

					for (int k = 0; k < ray_iterations; k++) {
						RD::ComputeListID compute_list = rd->compute_list_begin();
						rd->compute_list_bind_compute_pipeline(compute_list, compute_shader_secondary_pipeline);
						rd->compute_list_bind_uniform_set(compute_list, compute_base_uniform_set, 0);
						rd->compute_list_bind_uniform_set(compute_list, secondary_uniform_set, 1);

						push_constant.ray_from = k * max_rays;
						push_constant.ray_to = MIN((k + 1) * max_rays, int32_t(push_constant.ray_count));
						rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
						rd->compute_list_dispatch(compute_list, group_size.x, group_size.y, group_size.z);

						rd->compute_list_end();
						rd->submit();
						rd->sync();

						count++;
						if (p_step_function) {
							int total = (atlas_slices * x_regions * y_regions * ray_iterations);
							int percent = count * 100 / total;
							float p = float(count) / total * 0.1;
							if (p_step_function(0.6 + p, vformat(RTR("Integrate indirect lighting %d%%"), percent), p_bake_userdata, false)) {
								FREE_TEXTURES
								FREE_BUFFERS
								FREE_RASTER_RESOURCES
								FREE_COMPUTE_RESOURCES
								memdelete(rd);
								memdelete(rcd);
								return BAKE_ERROR_USER_ABORTED;
							}
						}
					}
				}
			}
			for (int coefficient = 0; coefficient < light_coefficients; coefficient++) {
				if (!store_staged_light_layer(s * light_coefficients + coefficient, rd->texture_get_data(light_accum_tex, coefficient))) {
					FREE_TEXTURES
					FREE_BUFFERS
					FREE_RASTER_RESOURCES
					FREE_COMPUTE_RESOURCES
					memdelete(rd);
					memdelete(rcd);
					return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
				}
			}
		}
	}

	/* LIGHTPROBES */

	RID light_probe_buffer;

	if (probe_positions.size()) {
		light_probe_buffer = rd->storage_buffer_create(sizeof(float) * 4 * 9 * probe_positions.size());

		if (p_step_function) {
			if (p_step_function(0.7, RTR("Baking light probes"), p_bake_userdata, true)) {
				FREE_TEXTURES
				FREE_BUFFERS
				FREE_RASTER_RESOURCES
				FREE_COMPUTE_RESOURCES
				if (probe_positions.size() > 0) {
					rd->free_rid(light_probe_buffer);
				}
				memdelete(rd);
				memdelete(rcd);
				return BAKE_ERROR_USER_ABORTED;
			}
		}

		Vector<RD::Uniform> uniforms;
		{
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
				u.binding = 0;
				u.append_id(light_probe_buffer);
				uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 1;
				u.append_id(light_source_tex);
				uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 2;
				u.append_id(light_environment_tex);
				uniforms.push_back(u);
			}

			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 6;
				u.append_id(area_light_atlas_tex);
				uniforms.push_back(u);
			}
		}
		RID light_probe_uniform_set = rd->uniform_set_create(uniforms, compute_shader_light_probes, 1);

		switch (p_quality) {
			case BAKE_QUALITY_LOW: {
				push_constant.ray_count = GLOBAL_GET("rendering/lightmapping/bake_quality/low_quality_probe_ray_count");
			} break;
			case BAKE_QUALITY_MEDIUM: {
				push_constant.ray_count = GLOBAL_GET("rendering/lightmapping/bake_quality/medium_quality_probe_ray_count");
			} break;
			case BAKE_QUALITY_HIGH: {
				push_constant.ray_count = GLOBAL_GET("rendering/lightmapping/bake_quality/high_quality_probe_ray_count");
			} break;
			case BAKE_QUALITY_ULTRA: {
				push_constant.ray_count = GLOBAL_GET("rendering/lightmapping/bake_quality/ultra_quality_probe_ray_count");
			} break;
		}

		push_constant.ray_count = CLAMP(push_constant.ray_count, 16u, 8192u);
		push_constant.probe_count = probe_positions.size();

		int max_rays = GLOBAL_GET("rendering/lightmapping/bake_performance/max_rays_per_probe_pass");
		int ray_iterations = Math::division_round_up((int32_t)push_constant.ray_count, max_rays);

		for (int i = 0; i < ray_iterations; i++) {
			RD::ComputeListID compute_list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(compute_list, compute_shader_light_probes_pipeline);
			rd->compute_list_bind_uniform_set(compute_list, compute_base_uniform_set, 0);
			rd->compute_list_bind_uniform_set(compute_list, light_probe_uniform_set, 1);

			push_constant.ray_from = i * max_rays;
			push_constant.ray_to = MIN((i + 1) * max_rays, int32_t(push_constant.ray_count));
			rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
			rd->compute_list_dispatch(compute_list, Math::division_round_up((int)probe_positions.size(), 64), 1, 1);

			rd->compute_list_end(); //done
			rd->submit();
			rd->sync();

			if (p_step_function) {
				int percent = i * 100 / ray_iterations;
				float p = float(i) / ray_iterations * 0.1;
				if (p_step_function(0.7 + p, vformat(RTR("Integrating light probes %d%%"), percent), p_bake_userdata, false)) {
					FREE_TEXTURES
					FREE_BUFFERS
					FREE_RASTER_RESOURCES
					FREE_COMPUTE_RESOURCES
					if (probe_positions.size() > 0) {
						rd->free_rid(light_probe_buffer);
					}
					memdelete(rd);
					memdelete(rcd);
					return BAKE_ERROR_USER_ABORTED;
				}
			}
		}
	}

	// Emission and direct-light lookup have no consumers after indirect lighting
	// and probes. Keep a tiny emission binding for post-processing shader layouts.
	free_base_uniform_sets();
	if (light_source_tex.is_valid()) {
		rd->free_rid(light_source_tex);
		light_source_tex = RID();
	}
	if (emission_array_tex.is_valid()) {
		rd->free_rid(emission_array_tex);
	}
	emission_array_tex = create_dummy_material_texture(RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32);
	set_base_texture(9, emission_array_tex);
	if (emission_array_tex.is_null() || !recreate_base_uniform_sets()) {
		FREE_TEXTURES
		FREE_BUFFERS
		FREE_RASTER_RESOURCES
		FREE_COMPUTE_RESOURCES
		if (probe_positions.size() > 0) {
			rd->free_rid(light_probe_buffer);
		}
		memdelete(rd);
		memdelete(rcd);
		return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
	}

#if 0
	for (int i = 0; i < probe_positions.size(); i++) {
		Ref<Image> img = Image::create_empty(6, 4, false, Image::FORMAT_RGB8);
		for (int j = 0; j < 6; j++) {
			Vector<uint8_t> s = rd->texture_get_data(lightprobe_tex, i * 6 + j);
			Ref<Image> img2 = Image::create_from_data(2, 2, false, Image::FORMAT_RGBAF, s);
			img2->convert(Image::FORMAT_RGB8);
			img->blit_rect(img2, Rect2i(0, 0, 2, 2), Point2i((j % 3) * 2, (j / 3) * 2));
		}
		img->save_png("res://3_light_probe_" + itos(i) + ".png");
	}
#endif

	// These stages can fail after all bake resources have been allocated.
	auto cleanup_denoise_resources = [&]() {
		FREE_TEXTURES
		FREE_BUFFERS
		FREE_RASTER_RESOURCES
		FREE_COMPUTE_RESOURCES
		if (probe_positions.size() > 0) {
			rd->free_rid(light_probe_buffer);
		}
		memdelete(rd);
		memdelete(rcd);
	};

	/* AMBIENT OCCLUSION */
	if (p_bake_ao) {
		if (p_step_function && p_step_function(0.79, RTR("Baking ambient occlusion"), p_bake_userdata, true)) {
			FREE_TEXTURES
			FREE_BUFFERS
			FREE_RASTER_RESOURCES
			FREE_COMPUTE_RESOURCES
			if (probe_positions.size() > 0) {
				rd->free_rid(light_probe_buffer);
			}
			memdelete(rd);
			memdelete(rcd);
			return BAKE_ERROR_USER_ABORTED;
		}

		Vector<RD::Uniform> uniforms;
		for (int binding = 0; binding < 4; binding++) {
			RD::Uniform u;
			u.uniform_type = binding == 0 || binding == 3 ? RD::UNIFORM_TYPE_IMAGE : RD::UNIFORM_TYPE_TEXTURE;
			u.binding = binding;
			u.append_id(binding == 0 ? light_accum_tex : (binding == 1 ? position_tex : (binding == 2 ? normal_tex : direct_light_tex)));
			uniforms.push_back(u);
		}
		RID ao_uniform_set = rd->uniform_set_create(uniforms, compute_shader_ao, 1);
		push_constant.ray_count = CLAMP((uint32_t)p_ao_samples, 1u, 1024u);
		push_constant.region_ofs[0] = 0;
		push_constant.region_ofs[1] = 0;
		const Vector3i group_size(Math::division_round_up(atlas_size.x, 8), Math::division_round_up(atlas_size.y, 8), 1);
		for (int s = 0; s < atlas_slices; s++) {
			if (!load_staged_light_slice(s, light_accum_tex)) {
				cleanup_denoise_resources();
				return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
			}
			for (int coefficient = 0; coefficient < light_coefficients; coefficient++) {
				if (rd->texture_update(direct_light_tex, coefficient, FileAccess::get_file_as_bytes(staged_light_files.direct_paths[s * light_coefficients + coefficient])) != OK) {
					cleanup_denoise_resources();
					return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
				}
			}
			prepare_geometry_slice(s);
			push_constant.geometry_slice = 0;
			push_constant.output_slice = 0;
			push_constant.atlas_slice = s;
			RD::ComputeListID compute_list = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(compute_list, compute_shader_ao_pipeline);
			rd->compute_list_bind_uniform_set(compute_list, compute_base_uniform_set, 0);
			rd->compute_list_bind_uniform_set(compute_list, ao_uniform_set, 1);
			rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
			rd->compute_list_dispatch(compute_list, group_size.x, group_size.y, group_size.z);
			rd->compute_list_end();
			rd->submit();
			rd->sync();
			if (!store_staged_light_slice(s, light_accum_tex)) {
				cleanup_denoise_resources();
				return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
			}
		}
		rd->free_rid(ao_uniform_set);
	}
	rd->free_rid(direct_light_tex);
	direct_light_tex = RID();

	/* DENOISE */

	// Replace the placeholder with one reusable post-processing slice.
	if (light_accum_tex2.is_valid()) {
		rd->free_rid(light_accum_tex2);
	}
	{
		RD::TextureFormat scratch_format = rd->texture_get_format(light_accum_tex);
		scratch_format.array_layers = p_bake_sh ? 4 : 1;
		scratch_format.usage_bits |= RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		light_accum_tex2 = rd->texture_create(scratch_format, RD::TextureView());
		if (light_accum_tex2.is_null()) {
			cleanup_denoise_resources();
			return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
		}
	}

	if (p_bake_sh) {
		for (int slice = 0; slice < atlas_slices; slice++) {
			if (!load_staged_light_slice(slice, light_accum_tex)) {
				cleanup_denoise_resources();
				return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
			}
			BakeError error = _pack_l1(rd, compute_shader, compute_base_uniform_set, push_constant, light_accum_tex, light_accum_tex2, atlas_size, 1);
			if (unlikely(error != BAKE_OK) || !store_staged_light_slice(slice, light_accum_tex)) {
				cleanup_denoise_resources();
				return error != BAKE_OK ? error : BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
			}
		}
	}

	if (p_use_denoiser) {
		if (p_step_function) {
			if (p_step_function(0.8, RTR("Denoising"), p_bake_userdata, true)) {
				FREE_TEXTURES
				FREE_BUFFERS
				FREE_RASTER_RESOURCES
				FREE_COMPUTE_RESOURCES
				if (probe_positions.size() > 0) {
					rd->free_rid(light_probe_buffer);
				}
				memdelete(rd);
				memdelete(rcd);
				return BAKE_ERROR_USER_ABORTED;
			}
		}

		{
			BakeError error = BAKE_OK;
			if (denoiser == 1) {
				// These resources have finished their part of the bake. Releasing them
				// before starting the external process leaves substantially more VRAM
				// available when OIDN runs on the same physical GPU.
				auto free_texture_before_oidn = [&](RID &p_texture) {
					if (p_texture.is_valid()) {
						rd->free_rid(p_texture);
						p_texture = RID();
					}
				};

				// Spill the large arrays outside the importable project tree. Keep the
				// shared shader resources for dilation and seam blending after OIDN.
				struct StoredTexture {
					RID *rid;
					bool preserve;
					RD::TextureFormat format;
					String path;
				};
				Vector<StoredTexture> stored_textures;
				Vector<String> temporary_files;
				const String cache_prefix = EditorPaths::get_singleton()->get_cache_dir().path_join(vformat("oidn_bake_%d_", OS::get_singleton()->get_process_id()));
				auto remove_temporary_files = [&]() {
					for (const String &path : temporary_files) {
						DirAccess::remove_absolute(path);
					}
				};
				auto store_texture = [&](RID &p_texture, const String &p_name, bool p_preserve = true) -> bool {
					if (p_texture.is_null()) {
						return true;
					}
					StoredTexture stored = { &p_texture, p_preserve, rd->texture_get_format(p_texture), cache_prefix + p_name };
					for (uint32_t layer = 0; p_preserve && layer < stored.format.array_layers; layer++) {
						const String path = stored.path + itos(layer);
						temporary_files.push_back(path);
						Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
						if (file.is_null() || !file->store_buffer(rd->texture_get_data(p_texture, layer))) {
							return false;
						}
					}
					stored_textures.push_back(stored);
					free_texture_before_oidn(p_texture);
					return true;
				};
				RD::TextureFormat normal_format = rd->texture_get_format(normal_tex);
				normal_format.array_layers = 1;
				oidn_normal_tex = rd->texture_create(normal_format, RD::TextureView());
				if (oidn_normal_tex.is_null()) {
					cleanup_denoise_resources();
					return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
				}
				// Keep one complete source coefficient resident while padding every
				// destination. Staged outputs cannot contaminate later source reads.
				RD::TextureFormat source_format = rd->texture_get_format(light_accum_tex);
				source_format.array_layers = atlas_slices;
				source_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
				RID oidn_light_source_tex = rd->texture_create(source_format, RD::TextureView());
				if (oidn_light_source_tex.is_null()) {
					cleanup_denoise_resources();
					return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
				}
				if (p_bake_shadowmask) {
					SWAP(shadowmask_tex, shadowmask_tex2);
				}
				for (int coefficient = 0; coefficient < light_coefficients && error == BAKE_OK; coefficient++) {
					for (int slice = 0; slice < atlas_slices; slice++) {
						if (rd->texture_update(oidn_light_source_tex, slice, FileAccess::get_file_as_bytes(staged_light_files.paths[slice * light_coefficients + coefficient])) != OK) {
							error = BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
							break;
						}
					}
					if (error != BAKE_OK) {
						break;
					}
					auto process_oidn_slice = [&](int p_slice, RID p_margin_tex) -> BakeError {
						BakeError pad_error = _pad_oidn_slice(rd, compute_shader, compute_base_uniform_set, push_constant, oidn_light_source_tex, light_accum_tex, p_margin_tex, mesh_tex, atlas_size, p_slice, coefficient, p_slice);
						if (pad_error != BAKE_OK) {
							return pad_error;
						}
						if (!store_staged_light_layer(p_slice * light_coefficients + coefficient, rd->texture_get_data(light_accum_tex, coefficient))) {
							return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
						}
						// Normal context and shadowmask are independent of SH coefficient.
						if (coefficient == 0) {
							if (p_bake_shadowmask) {
								pad_error = _pad_oidn_slice(rd, compute_shader, compute_base_uniform_set, push_constant, shadowmask_tex2, shadowmask_tex, p_margin_tex, mesh_tex, atlas_size, p_slice, p_slice, p_slice);
								if (pad_error != BAKE_OK) {
									return pad_error;
								}
							}
							// Export before reusing the normal layer for the next destination.
							const String path = cache_prefix + "normal_" + itos(p_slice);
							temporary_files.push_back(path);
							Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
							if (file.is_null() || !file->store_buffer(rd->texture_get_data(oidn_normal_tex, 0))) {
								ERR_FAIL_V_MSG(BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES, "Could not save temporary OIDN normal images.");
							}
						}
						return BAKE_OK;
					};
					error = _prepare_oidn_margins(rd, raster_base_uniform, mesh_tex, normal_tex, unocclude_tex, atlas_size, slice_seam_count, slice_seam_sources, p_denoiser_range + 3, oidn_normal_tex, prepare_geometry_slice, process_oidn_slice, p_step_function, p_bake_userdata);
				}
				rd->free_rid(oidn_light_source_tex);
				free_texture_before_oidn(oidn_normal_tex);
				if (error != BAKE_OK) {
					remove_temporary_files();
					cleanup_denoise_resources();
					return error;
				}
				free_texture_before_oidn(light_source_tex);
				free_texture_before_oidn(light_environment_tex);
				free_texture_before_oidn(area_light_atlas_tex);
				rd->submit();
				rd->sync();

				// The material atlases are only needed while tracing rays. Destroy the
				// uniform sets which reference them, then replace them with tiny dummy
				// textures after OIDN for the post-processing shader layouts.
				free_base_uniform_sets();
				free_texture_before_oidn(albedo_array_tex);
				free_texture_before_oidn(emission_array_tex);
				free_texture_before_oidn(position_tex);
				free_texture_before_oidn(unocclude_tex);
				free_texture_before_oidn(normal_tex);
				if (p_step_function && p_step_function(0.8, RTR("Saving temporary denoiser images"), p_bake_userdata, true)) {
					remove_temporary_files();
					cleanup_denoise_resources();
					return BAKE_ERROR_USER_ABORTED;
				}
				if (!store_texture(light_accum_tex, "light_working_", false) || !store_texture(light_accum_tex2, "scratch_", false) ||
						!store_texture(mesh_tex, "mesh_") || !store_texture(invalid_samples_tex, "invalid_") ||
						!store_texture(shadowmask_tex, "shadow_") || !store_texture(shadowmask_tex2, "shadow_scratch_", false)) {
					remove_temporary_files();
					cleanup_denoise_resources();
					ERR_FAIL_V_MSG(BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES, "Could not save temporary OIDN images.");
				}
				rd->submit();
				rd->sync();
				String oidn_device_for_bake = oidn_device;
				if (oidn_device_for_bake != "cpu") {
					const uint64_t gpu_budget = rd->get_memory_budget();
					RenderingDevice *main_device = RenderingDevice::get_singleton();
					const uint64_t renderer_usage = main_device != nullptr && main_device != rd ? main_device->get_memory_usage(RenderingDevice::MEMORY_TOTAL) : 0;
					const uint64_t bake_usage = rd->get_memory_usage(RenderingDevice::MEMORY_TOTAL);
					const uint64_t available_gpu = gpu_budget > renderer_usage + bake_usage ? gpu_budget - renderer_usage - bake_usage : 0;
					// OIDN's CUDA/SYCL/HIP filters need substantially more than their input
					// images. Use a conservative fixed reserve plus a resolution-dependent
					// working set, based on observed RTLightmap behavior.
					const uint64_t estimated_oidn_gpu = uint64_t(2) * 1024 * 1024 * 1024 + uint64_t(atlas_size.width) * uint64_t(atlas_size.height) * 64;
					if (gpu_budget != 0 && available_gpu < estimated_oidn_gpu) {
						oidn_device_for_bake = "cpu";
						WARN_PRINT(vformat("OIDN switched to CPU: estimated GPU memory needed %s, available budget %s.", String::humanize_size(estimated_oidn_gpu), String::humanize_size(available_gpu)));
					}
				}
				error = _denoise_oidn(staged_light_prefix, cache_prefix + "normal_", atlas_size, atlas_slices, p_bake_sh, false, oidn_path, oidn_device_for_bake, p_step_function, p_bake_userdata);
				if (error == BAKE_OK && p_bake_shadowmask) {
					error = _denoise_oidn(cache_prefix + "shadow_", cache_prefix + "normal_", atlas_size, atlas_slices, false, true, oidn_path, oidn_device_for_bake, p_step_function, p_bake_userdata);
				}
				if (error == BAKE_OK) {
					// Restore arrays only after the external process has released its GPU memory.
					for (const StoredTexture &stored : stored_textures) {
						RD::TextureFormat restored_format = stored.format;
						restored_format.usage_bits |= RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
						*stored.rid = rd->texture_create(restored_format, RD::TextureView());
						if (stored.rid->is_null()) {
							error = BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
							break;
						}
						for (uint32_t layer = 0; stored.preserve && layer < stored.format.array_layers; layer++) {
							if (rd->texture_update(*stored.rid, layer, FileAccess::get_file_as_bytes(stored.path + itos(layer))) != OK) {
								error = BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
								break;
							}
						}
						if (error != BAKE_OK) {
							break;
						}
					}
				}
				if (error == BAKE_OK) {
					albedo_array_tex = create_dummy_material_texture(RD::DATA_FORMAT_R8G8B8A8_UNORM);
					emission_array_tex = create_dummy_material_texture(RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32);
					if (albedo_array_tex.is_null() || emission_array_tex.is_null()) {
						error = BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
					} else {
						set_base_texture(8, albedo_array_tex);
						set_base_texture(9, emission_array_tex);
						if (!recreate_base_uniform_sets()) {
							error = BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
						}
					}
				}
				remove_temporary_files();
			} else {
				// JNLM (built-in).
				for (int slice = 0; slice < atlas_slices && error == BAKE_OK; slice++) {
					if (!load_staged_light_slice(slice, light_accum_tex)) {
						error = BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
						break;
					}
					prepare_geometry_slice(slice);
					error = _denoise_slice(rd, compute_shader, compute_base_uniform_set, push_constant, light_accum_tex, normal_tex, light_accum_tex2, unocclude_tex, p_denoiser_strength, p_denoiser_range, atlas_size, 0, slice, atlas_slices, p_bake_sh, p_step_function, p_bake_userdata);
					if (error == BAKE_OK && !store_staged_light_slice(slice, light_accum_tex)) {
						error = BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
					}
				}
			}
			if (unlikely(error != BAKE_OK)) {
				cleanup_denoise_resources();
				return error;
			}
		}

		if (p_bake_shadowmask && denoiser != 1) {
			BakeError error = BAKE_OK;
			for (int slice = 0; slice < atlas_slices && error == BAKE_OK; slice++) {
				prepare_geometry_slice(slice);
				error = _denoise_slice(rd, compute_shader, compute_base_uniform_set, push_constant, shadowmask_tex, normal_tex, shadowmask_tex2, unocclude_tex, p_denoiser_strength, p_denoiser_range, atlas_size, slice, slice, atlas_slices, false, p_step_function, p_bake_userdata);
			}
			if (unlikely(error != BAKE_OK)) {
				cleanup_denoise_resources();
				return error;
			}
		}
		if (oidn_normal_tex.is_valid()) {
			rd->free_rid(oidn_normal_tex);
			oidn_normal_tex = RID();
		}
	}

	// Albedo is needed by the built-in denoiser, but not by dilation or seams.
	free_base_uniform_sets();
	if (albedo_array_tex.is_valid()) {
		rd->free_rid(albedo_array_tex);
	}
	albedo_array_tex = create_dummy_material_texture(RD::DATA_FORMAT_R8G8B8A8_UNORM);
	set_base_texture(8, albedo_array_tex);
	if (albedo_array_tex.is_null() || !recreate_base_uniform_sets()) {
		cleanup_denoise_resources();
		return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
	}

	/* DILATE */

	{
		for (int slice = 0; slice < atlas_slices; slice++) {
			if (!load_staged_light_slice(slice, light_accum_tex)) {
				cleanup_denoise_resources();
				return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
			}
			BakeError error = _dilate(rd, compute_shader, compute_base_uniform_set, push_constant, light_accum_tex, light_accum_tex2, atlas_size, light_coefficients);
			if (unlikely(error != BAKE_OK) || !store_staged_light_slice(slice, light_accum_tex)) {
				cleanup_denoise_resources();
				return error != BAKE_OK ? error : BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
			}
		}

		if (p_bake_shadowmask) {
			BakeError error = _dilate(rd, compute_shader, compute_base_uniform_set, push_constant, shadowmask_tex, shadowmask_tex2, atlas_size, atlas_slices);
			if (unlikely(error != BAKE_OK)) {
				cleanup_denoise_resources();
				return error;
			}
		}
	}

#ifdef DEBUG_TEXTURES

	for (int i = 0; i < atlas_slices * (p_bake_sh ? 4 : 1); i++) {
		Vector<uint8_t> s = FileAccess::get_file_as_bytes(staged_light_files.paths[i]);
		Ref<Image> img = Image::create_from_data(atlas_size.width, atlas_size.height, false, Image::FORMAT_RGBAH, s);
		img->save_exr("res://4_light_secondary_" + itos(i) + ".exr", false);
	}
#endif

	/* BLEND SEAMS */
	//shaders
	Ref<RDShaderFile> blendseams_shader;
	blendseams_shader.instantiate();
	err = blendseams_shader->parse_versions_from_text(lm_blendseams_shader_glsl);
	if (err != OK) {
		FREE_TEXTURES
		FREE_BUFFERS
		FREE_RASTER_RESOURCES
		FREE_COMPUTE_RESOURCES
		memdelete(rd);

		memdelete(rcd);

		blendseams_shader->print_errors("blendseams_shader");
	}
	ERR_FAIL_COND_V(err != OK, BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);

	RID blendseams_line_raster_shader = rd->shader_create_from_spirv(blendseams_shader->get_spirv_stages("lines"));

	ERR_FAIL_COND_V(blendseams_line_raster_shader.is_null(), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);

	RID blendseams_triangle_raster_shader = rd->shader_create_from_spirv(blendseams_shader->get_spirv_stages("triangles"));

	ERR_FAIL_COND_V(blendseams_triangle_raster_shader.is_null(), BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES);

#define FREE_BLENDSEAMS_RESOURCES \
	rd->free_rid(blendseams_line_raster_shader); \
	rd->free_rid(blendseams_triangle_raster_shader);

	{
		const int subslices = p_bake_sh ? 4 : 1;
		RD::TextureFormat seam_source_format = rd->texture_get_format(light_accum_tex);
		seam_source_format.array_layers = atlas_slices;
		seam_source_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		RID seam_source_tex = rd->texture_create(seam_source_format, RD::TextureView());
		if (seam_source_tex.is_null()) {
			cleanup_denoise_resources();
			return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
		}
		Vector<RID> framebuffers;
		for (int i = 0; i < subslices; i++) {
			RID slice_tex = rd->texture_create_shared_from_slice(RD::TextureView(), light_accum_tex2, i, 0);
			Vector<RID> fb;
			fb.push_back(slice_tex);
			fb.push_back(raster_depth_buffer);
			framebuffers.push_back(rd->framebuffer_create(fb));
		}

		Vector<RD::Uniform> line_uniforms;
		{
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 0;
				u.append_id(seam_source_tex);
				line_uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 1;
				u.append_id(invalid_samples_tex);
				line_uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 2;
				u.append_id(mesh_tex);
				line_uniforms.push_back(u);
			}
		}

		RID blendseams_line_raster_uniform = rd->uniform_set_create(line_uniforms, blendseams_line_raster_shader, 1);
		Vector<RD::Uniform> triangle_uniforms;
		triangle_uniforms.push_back(line_uniforms[0]);
		RID blendseams_triangle_raster_uniform = rd->uniform_set_create(triangle_uniforms, blendseams_triangle_raster_shader, 1);

		RD::PipelineColorBlendState bs = RD::PipelineColorBlendState::create_blend(1);
		bs.attachments.write[0].src_alpha_blend_factor = RD::BLEND_FACTOR_ZERO;
		bs.attachments.write[0].dst_alpha_blend_factor = RD::BLEND_FACTOR_ONE;

		RD::PipelineDepthStencilState ds;
		ds.enable_depth_test = true;
		ds.enable_depth_write = true;
		ds.depth_compare_operator = RD::COMPARE_OP_LESS; //so it does not render same pixel twice, this avoids wrong blending

		RID blendseams_line_raster_pipeline = rd->render_pipeline_create(blendseams_line_raster_shader, rd->framebuffer_get_format(framebuffers[0]), RD::INVALID_FORMAT_ID, RD::RENDER_PRIMITIVE_LINES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), ds, bs, 0);
		RID blendseams_triangle_raster_pipeline = rd->render_pipeline_create(blendseams_triangle_raster_shader, rd->framebuffer_get_format(framebuffers[0]), RD::INVALID_FORMAT_ID, RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), ds, bs, 0);

		Vector<uint32_t> seam_offsets;
		Vector<uint32_t> triangle_offsets;
		seam_offsets.resize(atlas_slices);
		triangle_offsets.resize(atlas_slices);
		uint32_t seam_offset = 0;
		uint32_t triangle_offset = 0;
		for (int slice = 0; slice < atlas_slices; slice++) {
			seam_offsets.write[slice] = seam_offset;
			triangle_offsets.write[slice] = triangle_offset;
			seam_offset += slice_seam_count[slice];
			triangle_offset += slice_triangle_count[slice];
		}

		Vector<Ref<Image>> seamed_images;
		seamed_images.resize(atlas_slices * subslices);
		for (int k = 0; k < subslices; k++) {
			for (int source_slice = 0; source_slice < atlas_slices; source_slice++) {
				if (rd->texture_update(seam_source_tex, source_slice, FileAccess::get_file_as_bytes(staged_light_files.paths[source_slice * subslices + k])) != OK) {
					rd->free_rid(seam_source_tex);
					cleanup_denoise_resources();
					return BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES;
				}
			}
			for (int i = 0; i < atlas_slices; i++) {
				rd->texture_copy(seam_source_tex, light_accum_tex2, Vector3(), Vector3(), Vector3(atlas_size.width, atlas_size.height, 1), 0, 0, i, k);

				if (slice_seam_count[i] == 0) {
					Vector<uint8_t> data = rd->texture_get_data(light_accum_tex2, k);
					Ref<Image> image = Image::create_from_data(atlas_size.width, atlas_size.height, false, Image::FORMAT_RGBAH, data);
					image->convert(Image::FORMAT_RGBH);
					seamed_images.write[i * subslices + k] = image;
					continue;
				}

				RasterSeamsPushConstant seams_push_constant;
				seams_push_constant.slice = uint32_t(i);
				seams_push_constant.subslices = 1;
				seams_push_constant.pad = lighting_ray_count;
				// Store the current subslice in the breadcrumb.
				RD::DrawListID draw_list = rd->draw_list_begin(framebuffers[k], RD::DRAW_CLEAR_DEPTH, Vector<Color>(), 1.0f, 0, Rect2(), RDD::BreadcrumbMarker::LIGHTMAPPER_PASS | seams_push_constant.slice);

				rd->draw_list_bind_uniform_set(draw_list, raster_base_uniform, 0);
				rd->draw_list_bind_uniform_set(draw_list, blendseams_line_raster_uniform, 1);

				const int uv_offset_count = 9;
				static const Vector3 uv_offsets[uv_offset_count] = {
					Vector3(0, 0, 0.5),
					Vector3(0, 1, 0.2),
					Vector3(0, -1, 0.2),
					Vector3(1, 0, 0.2),
					Vector3(-1, 0, 0.2),
					Vector3(-1, -1, 0.1),
					Vector3(1, -1, 0.1),
					Vector3(1, 1, 0.1),
					Vector3(-1, 1, 0.1),
				};

				/* step 1 use lines to blend the edges */
				{
					seams_push_constant.base_index = seam_offsets[i];
					rd->draw_list_bind_render_pipeline(draw_list, blendseams_line_raster_pipeline);
					seams_push_constant.uv_offset[0] = uv_offsets[0].x / float(atlas_size.width);
					seams_push_constant.uv_offset[1] = uv_offsets[0].y / float(atlas_size.height);
					seams_push_constant.blend = uv_offsets[0].z;

					rd->draw_list_set_push_constant(draw_list, &seams_push_constant, sizeof(RasterSeamsPushConstant));
					rd->draw_list_draw(draw_list, false, 1, slice_seam_count[i] * 2);
				}

				/* step 2 use triangles to mask the interior */

				{
					seams_push_constant.base_index = triangle_offsets[i];
					rd->draw_list_bind_render_pipeline(draw_list, blendseams_triangle_raster_pipeline);
					rd->draw_list_bind_uniform_set(draw_list, blendseams_triangle_raster_uniform, 1);
					seams_push_constant.blend = 0; //do not draw them, just fill the z-buffer so its used as a mask

					rd->draw_list_set_push_constant(draw_list, &seams_push_constant, sizeof(RasterSeamsPushConstant));
					rd->draw_list_draw(draw_list, false, 1, slice_triangle_count[i] * 3);
				}
				/* step 3 blend around the triangle */

				rd->draw_list_bind_render_pipeline(draw_list, blendseams_line_raster_pipeline);
				rd->draw_list_bind_uniform_set(draw_list, blendseams_line_raster_uniform, 1);

				for (int j = 1; j < uv_offset_count; j++) {
					seams_push_constant.base_index = seam_offsets[i];
					seams_push_constant.uv_offset[0] = uv_offsets[j].x / float(atlas_size.width);
					seams_push_constant.uv_offset[1] = uv_offsets[j].y / float(atlas_size.height);
					seams_push_constant.blend = uv_offsets[j].z;

					rd->draw_list_set_push_constant(draw_list, &seams_push_constant, sizeof(RasterSeamsPushConstant));
					rd->draw_list_draw(draw_list, false, 1, slice_seam_count[i] * 2);
				}
				rd->draw_list_end();
				Vector<uint8_t> data = rd->texture_get_data(light_accum_tex2, k);
				Ref<Image> image = Image::create_from_data(atlas_size.width, atlas_size.height, false, Image::FORMAT_RGBAH, data);
				image->convert(Image::FORMAT_RGBH);
				seamed_images.write[i * subslices + k] = image;
			}
		}
		for (const Ref<Image> &image : seamed_images) {
			lightmap_textures.push_back(image);
		}
		rd->free_rid(seam_source_tex);
	}

#ifdef DEBUG_TEXTURES

	for (int i = 0; i < lightmap_textures.size(); i++) {
		lightmap_textures[i]->save_exr("res://5_blendseams" + itos(i) + ".exr", false);
	}
#endif

	if (p_step_function) {
		p_step_function(0.9, RTR("Retrieving textures"), p_bake_userdata, true);
	}

	if (p_bake_shadowmask) {
		for (int i = 0; i < atlas_slices; i++) {
			Vector<uint8_t> s = rd->texture_get_data(shadowmask_tex, i);
			Ref<Image> img = Image::create_from_data(atlas_size.width, atlas_size.height, false, Image::FORMAT_RGBA8, s);
			img->convert(Image::FORMAT_R8);
			shadowmask_textures.push_back(img);
		}
	}

	if (probe_positions.size() > 0) {
		probe_values.resize(probe_positions.size() * 9);
		Vector<uint8_t> probe_data = rd->buffer_get_data(light_probe_buffer);
		memcpy(probe_values.ptrw(), probe_data.ptr(), probe_data.size());
		rd->free_rid(light_probe_buffer);

#ifdef DEBUG_TEXTURES
		{
			Ref<Image> img2 = Image::create_from_data(probe_values.size(), 1, false, Image::FORMAT_RGBAF, probe_data);
			img2->save_exr("res://6_lightprobes.exr", false);
		}
#endif
	}

	FREE_TEXTURES
	FREE_BUFFERS
	FREE_RASTER_RESOURCES
	FREE_COMPUTE_RESOURCES
	FREE_BLENDSEAMS_RESOURCES

	memdelete(rd);

	memdelete(rcd);

	return BAKE_OK;
}

int LightmapperRD::get_bake_texture_count() const {
	return lightmap_textures.size();
}

Ref<Image> LightmapperRD::get_bake_texture(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, lightmap_textures.size(), Ref<Image>());
	return lightmap_textures[p_index];
}

int LightmapperRD::get_shadowmask_texture_count() const {
	return shadowmask_textures.size();
}

Ref<Image> LightmapperRD::get_shadowmask_texture(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, shadowmask_textures.size(), Ref<Image>());
	return shadowmask_textures[p_index];
}

int LightmapperRD::get_bake_mesh_count() const {
	return mesh_instances.size();
}

Variant LightmapperRD::get_bake_mesh_userdata(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, mesh_instances.size(), Variant());
	return mesh_instances[p_index].data.userdata;
}

Rect2 LightmapperRD::get_bake_mesh_uv_scale(int p_index) const {
	ERR_FAIL_COND_V(lightmap_textures.is_empty(), Rect2());
	Rect2 uv_ofs;
	Vector2 atlas_size = Vector2(lightmap_textures[0]->get_width(), lightmap_textures[0]->get_height());
	uv_ofs.position = Vector2(mesh_instances[p_index].offset) / atlas_size;
	uv_ofs.size = Vector2(mesh_instances[p_index].data.lightmap_size) / atlas_size;
	return uv_ofs;
}

int LightmapperRD::get_bake_mesh_texture_slice(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, mesh_instances.size(), Variant());
	return mesh_instances[p_index].slice;
}

int LightmapperRD::get_bake_probe_count() const {
	return probe_positions.size();
}

Vector3 LightmapperRD::get_bake_probe_point(int p_probe) const {
	ERR_FAIL_INDEX_V(p_probe, probe_positions.size(), Variant());
	return Vector3(probe_positions[p_probe].position[0], probe_positions[p_probe].position[1], probe_positions[p_probe].position[2]);
}

Vector<Color> LightmapperRD::get_bake_probe_sh(int p_probe) const {
	ERR_FAIL_INDEX_V(p_probe, probe_positions.size(), Vector<Color>());
	Vector<Color> ret;
	ret.resize(9);
	memcpy(ret.ptrw(), &probe_values[p_probe * 9], sizeof(Color) * 9);
	return ret;
}

LightmapperRD::LightmapperRD() {
}

/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#include "core/crypto/crypto_core.h"
#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"
#include "core/math/vector3.h"
#include "core/os/memory.h"

#include <xatlas.h>

#include <cstring>

extern bool (*array_mesh_lightmap_unwrap_callback)(float p_texel_size, const float *p_vertices, const float *p_normals, int p_vertex_count, const int *p_indices, int p_index_count, const uint8_t *p_cache_data, bool *r_use_cache, uint8_t **r_mesh_cache, int *r_mesh_cache_size, float **r_uv, int **r_vertex, int *r_vertex_count, int **r_index, int *r_index_count, int *r_size_hint_x, int *r_size_hint_y);

// Unwrap on a grid finer than the lightmap texel, so small and thin charts (pipes,
// trims) are not rounded up to whole texels.
static constexpr int UNWRAP_MAX_SUBDIVISIONS = 4;
// Packing time grows with the grid size, so large meshes use fewer subdivisions
// (they don't need them) to keep the finer grid within this many texels.
static constexpr double UNWRAP_MAX_GRID_TEXELS = 4.0e6;
// Bump when the unwrap settings change, so cached UV2 is regenerated.
static constexpr int UNWRAP_VERSION = 7;

bool xatlas_mesh_lightmap_unwrap_callback(float p_texel_size, const float *p_vertices, const float *p_normals, int p_vertex_count, const int *p_indices, int p_index_count, const uint8_t *p_cache_data, bool *r_use_cache, uint8_t **r_mesh_cache, int *r_mesh_cache_size, float **r_uv, int **r_vertex, int *r_vertex_count, int **r_index, int *r_index_count, int *r_size_hint_x, int *r_size_hint_y) {
	CryptoCore::MD5Context ctx;
	ctx.start();

	ctx.update((unsigned char *)&UNWRAP_VERSION, sizeof(int));
	ctx.update((unsigned char *)&p_texel_size, sizeof(float));
	ctx.update((unsigned char *)p_indices, sizeof(int) * p_index_count);
	ctx.update((unsigned char *)p_vertices, sizeof(float) * p_vertex_count * 3);
	ctx.update((unsigned char *)p_normals, sizeof(float) * p_vertex_count * 3);

	unsigned char hash[16];
	ctx.finish(hash);

	bool cached = false;
	unsigned int cache_idx = 0;

	*r_mesh_cache = nullptr;
	*r_mesh_cache_size = 0;

	if (p_cache_data) {
		//Check if hash is in cache data
		int *cache_data = (int *)p_cache_data;
		int n_entries = cache_data[0];
		unsigned int read_idx = 1;
		for (int i = 0; i < n_entries; ++i) {
			if (memcmp(&cache_data[read_idx], hash, 16) == 0) {
				cached = true;
				cache_idx = read_idx;
				break;
			}

			read_idx += 4; // hash
			read_idx += 2; // size hint

			int vertex_count = cache_data[read_idx];
			read_idx += 1; // vertex count
			read_idx += vertex_count; // vertex
			read_idx += vertex_count * 2; // uvs

			int index_count = cache_data[read_idx];
			read_idx += 1; // index count
			read_idx += index_count; // indices
		}
	}

	if (cached) {
		int *cache_data = (int *)p_cache_data;

		cache_idx += 4;

		// Load size
		*r_size_hint_x = cache_data[cache_idx];
		*r_size_hint_y = cache_data[cache_idx + 1];
		cache_idx += 2;

		// Load vertices
		*r_vertex_count = cache_data[cache_idx];
		cache_idx++;
		*r_vertex = &cache_data[cache_idx];
		cache_idx += *r_vertex_count;

		// Load UVs
		*r_uv = (float *)&cache_data[cache_idx];
		cache_idx += *r_vertex_count * 2;

		// Load indices
		*r_index_count = cache_data[cache_idx];
		cache_idx++;
		*r_index = &cache_data[cache_idx];
	} else {
		// set up input mesh
		xatlas::MeshDecl input_mesh;
		input_mesh.indexData = p_indices;
		input_mesh.indexCount = p_index_count;
		input_mesh.indexFormat = xatlas::IndexFormat::UInt32;

		input_mesh.vertexCount = p_vertex_count;
		input_mesh.vertexPositionData = p_vertices;
		input_mesh.vertexPositionStride = sizeof(float) * 3;
		input_mesh.vertexNormalData = p_normals;
		input_mesh.vertexNormalStride = sizeof(uint32_t) * 3;
		input_mesh.vertexUvData = nullptr;
		input_mesh.vertexUvStride = 0;

		xatlas::ChartOptions chart_options;
		chart_options.fixWinding = true;
		// Like Unity's hard angle: grow charts across hard edges, limited only by normal deviation (~73 degrees max).
		// Otherwise every face of a flat-shaded mesh becomes its own chart, wasting space on padding and adding seams.
		chart_options.normalSeamWeight = 0.0f;

		ERR_FAIL_COND_V_MSG(p_texel_size <= 0.0f, false, "Texel size must be greater than 0.");

		double surface_area = 0.0;
		for (int i = 0; i + 2 < p_index_count; i += 3) {
			const float *a = &p_vertices[p_indices[i] * 3];
			const float *b = &p_vertices[p_indices[i + 1] * 3];
			const float *c = &p_vertices[p_indices[i + 2] * 3];
			const Vector3 ab(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
			const Vector3 ac(c[0] - a[0], c[1] - a[1], c[2] - a[2]);
			surface_area += ab.cross(ac).length() * 0.5;
		}
		const double lightmap_texels = surface_area / (double(p_texel_size) * p_texel_size);
		const int subdivisions = CLAMP(int(Math::sqrt(UNWRAP_MAX_GRID_TEXELS / MAX(lightmap_texels, 1.0))), 1, UNWRAP_MAX_SUBDIVISIONS);

		xatlas::PackOptions pack_options;
		// xatlas' bilinear margin is one texel of the finer grid, so pad with two lightmap
		// texels to keep bilinear/bicubic filtering from bleeding between charts.
		pack_options.padding = 2 * subdivisions;
		pack_options.maxChartSize = 4094 * subdivisions; // 4094 lightmap texels: atlassing needs 2 for padding between meshes, so 4096-2
		// Block alignment is meant for compressed textures, but LightmapGI rescales this layout anyway.
		pack_options.blockAlign = false;
		pack_options.texelsPerUnit = subdivisions / p_texel_size;

		xatlas::Atlas *atlas = xatlas::Create();

		xatlas::AddMeshError err = xatlas::AddMesh(atlas, input_mesh, 1);
		ERR_FAIL_COND_V_MSG(err != xatlas::AddMeshError::Success, false, xatlas::StringForEnum(err));

		xatlas::ComputeCharts(atlas, chart_options);
		xatlas::PackCharts(atlas, pack_options);

		if (atlas->width == 0 || atlas->height == 0) {
			xatlas::Destroy(atlas);
			return false; //could not bake because there is no area
		}

		*r_size_hint_x = Math::division_round_up(atlas->width, (uint32_t)subdivisions);
		*r_size_hint_y = Math::division_round_up(atlas->height, (uint32_t)subdivisions);

		// Normalize against the rounded-up hint, so UV2 maps exactly onto lightmap texels.
		float w = *r_size_hint_x * subdivisions;
		float h = *r_size_hint_y * subdivisions;

		const xatlas::Mesh &output = atlas->meshes[0];

		*r_vertex = (int *)memalloc(sizeof(int) * output.vertexCount);
		ERR_FAIL_NULL_V_MSG(*r_vertex, false, "Out of memory.");
		*r_uv = (float *)memalloc(sizeof(float) * output.vertexCount * 2);
		ERR_FAIL_NULL_V_MSG(*r_uv, false, "Out of memory.");
		*r_index = (int *)memalloc(sizeof(int) * output.indexCount);
		ERR_FAIL_NULL_V_MSG(*r_index, false, "Out of memory.");

		float max_x = 0;
		float max_y = 0;
		for (uint32_t i = 0; i < output.vertexCount; i++) {
			(*r_vertex)[i] = output.vertexArray[i].xref;
			(*r_uv)[i * 2 + 0] = output.vertexArray[i].uv[0] / w;
			(*r_uv)[i * 2 + 1] = output.vertexArray[i].uv[1] / h;
			max_x = MAX(max_x, output.vertexArray[i].uv[0]);
			max_y = MAX(max_y, output.vertexArray[i].uv[1]);
		}

		*r_vertex_count = output.vertexCount;

		for (uint32_t i = 0; i < output.indexCount; i++) {
			(*r_index)[i] = output.indexArray[i];
		}

		*r_index_count = output.indexCount;

		xatlas::Destroy(atlas);
	}

	if (*r_use_cache) {
		// Build cache data for current mesh

		unsigned int new_cache_size = 4 + 2 + 1 + *r_vertex_count + (*r_vertex_count * 2) + 1 + *r_index_count; // hash + size hint + vertex_count + vertices + uvs + index_count + indices
		new_cache_size *= sizeof(int);
		int *new_cache_data = (int *)memalloc(new_cache_size);
		unsigned int new_cache_idx = 0;

		// hash
		memcpy(&new_cache_data[new_cache_idx], hash, 16);
		new_cache_idx += 4;

		// size hint
		new_cache_data[new_cache_idx] = *r_size_hint_x;
		new_cache_data[new_cache_idx + 1] = *r_size_hint_y;
		new_cache_idx += 2;

		// vertex count
		new_cache_data[new_cache_idx] = *r_vertex_count;
		new_cache_idx++;

		// vertices
		memcpy(&new_cache_data[new_cache_idx], *r_vertex, sizeof(int) * (*r_vertex_count));
		new_cache_idx += *r_vertex_count;

		// uvs
		memcpy(&new_cache_data[new_cache_idx], *r_uv, sizeof(float) * (*r_vertex_count) * 2);
		new_cache_idx += *r_vertex_count * 2;

		// index count
		new_cache_data[new_cache_idx] = *r_index_count;
		new_cache_idx++;

		// indices
		memcpy(&new_cache_data[new_cache_idx], *r_index, sizeof(int) * (*r_index_count));

		// Return cache data to the caller
		*r_mesh_cache = (uint8_t *)new_cache_data;
		*r_mesh_cache_size = new_cache_size;
	}

	*r_use_cache = cached; // Return whether cache was used.

	return true;
}

void initialize_xatlas_unwrap_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	array_mesh_lightmap_unwrap_callback = xatlas_mesh_lightmap_unwrap_callback;
}

void uninitialize_xatlas_unwrap_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}

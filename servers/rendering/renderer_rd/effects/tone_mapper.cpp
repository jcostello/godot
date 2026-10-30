/**************************************************************************/
/*  tone_mapper.cpp                                                       */
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

#include "tone_mapper.h"

#include "servers/rendering/renderer_rd/renderer_compositor_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/storage/environment_storage.h"

using namespace RendererRD;

RID ToneMapper::_upload_mobile_grading_buffer(const TonemapSettings &p_settings) {
	ToneMapper::MobileGradingData data = {};
	for (int row = 0; row < 3; row++) {
		for (int column = 0; column < 3; column++) {
			data.white_balance[row][column] = p_settings.white_balance_matrix[row][column];
		}
	}
	data.white_balance[0][3] = p_settings.use_chromatic_adaptation;
	for (int range = 0; range < 12; range++) {
		for (int parameter = 0; parameter < 4; parameter++) {
			data.tonal_correction[range][parameter] = p_settings.tonal_correction[range][parameter];
		}
	}
	mobile_grading_buffers.prepare_for_upload();
	RID buffer = mobile_grading_buffers._get(0);
	mobile_grading_buffers.upload(0, &data, sizeof(data));
	return buffer;
}

RID ToneMapper::_get_white_balance_buffer(const Basis &p_matrix, const Vector3 &p_offset) {
	if (white_balance_buffer.is_valid() && uploaded_white_balance_matrix == p_matrix && uploaded_white_balance_offset == p_offset) {
		return white_balance_buffer;
	}
	float rows[3][4] = {};
	for (int row = 0; row < 3; row++) {
		for (int column = 0; column < 3; column++) {
			rows[row][column] = p_matrix[row][column];
		}
		rows[row][3] = p_offset[row];
	}
	if (white_balance_buffer.is_null()) {
		white_balance_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(rows));
		ERR_FAIL_COND_V(white_balance_buffer.is_null(), RID());
	}
	RD::get_singleton()->buffer_update(white_balance_buffer, 0, sizeof(rows), rows);
	uploaded_white_balance_matrix = p_matrix;
	uploaded_white_balance_offset = p_offset;
	return white_balance_buffer;
}

RID ToneMapper::_get_forward_grading_buffer(const TonemapSettings &p_settings) {
	ForwardGradingData data;
	data.rows[0][0] = p_settings.shadows_start;
	data.rows[0][1] = p_settings.shadows_end;
	data.rows[0][2] = p_settings.highlights_start;
	data.rows[0][3] = p_settings.highlights_end;
	for (int range = 0; range < 12; range++) {
		for (int channel = 0; channel < 4; channel++) {
			data.rows[1 + range][channel] = p_settings.tonal_correction[range][channel];
		}
	}
	data.rows[13][0] = p_settings.vibrance;
	data.rows[13][1] = p_settings.vignette;
	data.rows[13][2] = p_settings.vignette_start;
	data.rows[13][3] = p_settings.vignette_end;
	data.rows[14][0] = p_settings.forward_curve_flags;
	data.rows[14][1] = p_settings.forward_adjustment_flags;
	data.rows[14][2] = p_settings.use_1d_color_correction;
	if ((p_settings.forward_adjustment_flags & RendererEnvironmentStorage::FORWARD_ADJUSTMENT_CORRECTION_CONTRAST) != 0) {
		const float pivot = p_settings.tonal_correction[5].w;
		if (cached_correction_pivot != pivot) {
			cached_correction_pivot = pivot;
			cached_correction_logit = Math::log(pivot / (1.0f - pivot));
		}
		data.rows[14][3] = cached_correction_logit;
	}

	if (forward_grading_buffer.is_valid() && memcmp(&data, &uploaded_forward_grading, sizeof(data)) == 0) {
		return forward_grading_buffer;
	}
	if (forward_grading_buffer.is_null()) {
		forward_grading_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(data));
		ERR_FAIL_COND_V(forward_grading_buffer.is_null(), RID());
	}
	RD::get_singleton()->buffer_update(forward_grading_buffer, 0, sizeof(data), &data);
	uploaded_forward_grading = data;
	return forward_grading_buffer;
}

ToneMapper::ToneMapper(bool p_use_mobile_version) {
	using_mobile_version = p_use_mobile_version;
	if (using_mobile_version) {
		mobile_grading_buffers.set_uniform_size(0, sizeof(MobileGradingData));
		// Initialize tonemapper
		Vector<String> tonemap_modes;
		tonemap_modes.push_back("\n");
		tonemap_modes.push_back("\n#define USE_1D_LUT\n");
		tonemap_modes.push_back("\n#define SUBPASS\n");
		tonemap_modes.push_back("\n#define SUBPASS\n#define USE_1D_LUT\n");

		// multiview versions of our shaders
		tonemap_modes.push_back("\n#define USE_MULTIVIEW\n");
		tonemap_modes.push_back("\n#define USE_MULTIVIEW\n#define USE_1D_LUT\n");
		tonemap_modes.push_back("\n#define USE_MULTIVIEW\n#define SUBPASS\n");
		tonemap_modes.push_back("\n#define USE_MULTIVIEW\n#define SUBPASS\n#define USE_1D_LUT\n");

		Vector<uint64_t> dynamic_buffers;
		dynamic_buffers.push_back(ShaderRD::DynamicBuffer::encode(0, 8));
		tonemap_mobile.shader.initialize(tonemap_modes, "", Vector<RD::PipelineImmutableSampler>(), dynamic_buffers);

		if (!RendererCompositorRD::get_singleton()->is_xr_enabled()) {
			tonemap_mobile.shader.set_variant_enabled(TONEMAP_MOBILE_MODE_NORMAL_MULTIVIEW, false);
			tonemap_mobile.shader.set_variant_enabled(TONEMAP_MOBILE_MODE_1D_LUT_MULTIVIEW, false);
			tonemap_mobile.shader.set_variant_enabled(TONEMAP_MOBILE_MODE_SUBPASS_MULTIVIEW, false);
			tonemap_mobile.shader.set_variant_enabled(TONEMAP_MOBILE_MODE_SUBPASS_1D_LUT_MULTIVIEW, false);
		}

		tonemap_mobile.shader_version = tonemap_mobile.shader.version_create();

		for (int i = 0; i < TONEMAP_MOBILE_MODE_MAX; i++) {
			if (tonemap_mobile.shader.is_variant_enabled(i)) {
				tonemap_mobile.pipelines[i].setup(tonemap_mobile.shader.version_get_shader(tonemap_mobile.shader_version, i), RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_disabled(), 0);
			} else {
				tonemap_mobile.pipelines[i].clear();
			}
		}

	} else {
		// Initialize tonemapper
		Vector<String> tonemap_modes;
		tonemap_modes.push_back("\n");
		tonemap_modes.push_back("\n#define USE_GLOW_FILTER_BICUBIC\n");

		// multiview versions of our shaders
		tonemap_modes.push_back("\n#define USE_MULTIVIEW\n");
		tonemap_modes.push_back("\n#define USE_MULTIVIEW\n#define USE_GLOW_FILTER_BICUBIC\n");

		const int base_variant_count = tonemap_modes.size();
		for (int i = 0; i < base_variant_count; i++) {
			String defines = tonemap_modes[i] + "\n#define USE_WHITE_BALANCE\n";
			tonemap_modes.push_back(defines);
		}
		for (int i = 0; i < TONEMAP_MODE_FORWARD_GRADING; i++) {
			tonemap_modes.push_back(tonemap_modes[i] + "\n#define USE_FORWARD_GRADING\n");
		}
		for (int i = 0; i < TONEMAP_MODE_FORWARD_CURVES; i++) {
			tonemap_modes.push_back(tonemap_modes[i] + "\n#define USE_FORWARD_CURVES\n");
		}
		tonemap.shader.initialize(tonemap_modes);
		// Curves always use the grading data, so curves-only variants are never used.
		for (int i = TONEMAP_MODE_FORWARD_CURVES; i < TONEMAP_MODE_FORWARD_CURVES + TONEMAP_MODE_FORWARD_GRADING; i++) {
			tonemap.shader.set_variant_enabled(i, false);
		}

		if (!RendererCompositorRD::get_singleton()->is_xr_enabled()) {
			for (int i = 0; i < TONEMAP_MODE_MAX; i++) {
				if ((i & 2) != 0) {
					tonemap.shader.set_variant_enabled(i, false);
				}
			}
		}

		tonemap.shader_version = tonemap.shader.version_create();

		for (int i = 0; i < TONEMAP_MODE_MAX; i++) {
			if (tonemap.shader.is_variant_enabled(i)) {
				tonemap.pipelines[i].setup(tonemap.shader.version_get_shader(tonemap.shader_version, i), RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_disabled(), 0);
			} else {
				tonemap.pipelines[i].clear();
			}
		}
	}
}

ToneMapper::~ToneMapper() {
	if (forward_grading_buffer.is_valid()) {
		RD::get_singleton()->free_rid(forward_grading_buffer);
	}
	if (white_balance_buffer.is_valid()) {
		RD::get_singleton()->free_rid(white_balance_buffer);
	}
	if (using_mobile_version) {
		mobile_grading_buffers.uninit();
		tonemap_mobile.shader.version_free(tonemap_mobile.shader_version);
	} else {
		tonemap.shader.version_free(tonemap.shader_version);
	}
}

void ToneMapper::tonemapper(RID p_source_color, RID p_dst_framebuffer, const TonemapSettings &p_settings) {
	ERR_FAIL_COND_MSG(using_mobile_version, "Can't use the non mobile version of the tonemapper with the Mobile renderer.");
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	const uint32_t forward_flags = p_settings.forward_adjustment_flags;
	const bool use_curves = (forward_flags & (RendererEnvironmentStorage::FORWARD_ADJUSTMENT_CURVES | RendererEnvironmentStorage::FORWARD_ADJUSTMENT_LUT)) != 0;
	const bool use_forward_grading = forward_flags != 0;
	const bool use_transform = p_settings.use_forward_white_balance;
	const float saturation = p_settings.saturation;
	const int color_modes = int(p_settings.color_modes.x);
	const bool soft_contrast = (color_modes & 2) != 0 && p_settings.contrast != 1.0f;
	const bool preserve_luminance = (color_modes & 1) != 0;
	uint32_t adjustment_specializations = 0;
	adjustment_specializations |= soft_contrast ? 1 << 0 : 0;
	adjustment_specializations |= saturation != 1.0f ? 1 << 1 : 0;
	adjustment_specializations |= saturation != 1.0f && preserve_luminance ? 1 << 2 : 0;
	// Forward flag bit k enables specialization constant 4 + k.
	adjustment_specializations |= forward_flags << 4;

	memset(&tonemap.push_constant, 0, sizeof(TonemapPushConstant));
	tonemap.push_constant.saturation = saturation;
	tonemap.push_constant.adjustment_controls[0] = p_settings.contrast;
	tonemap.push_constant.adjustment_controls[1] = p_settings.color_modes.y;
	if (soft_contrast && p_settings.contrast > 0.0f) {
		if (cached_soft_contrast != p_settings.contrast || cached_soft_contrast_pivot != p_settings.color_modes.y) {
			cached_soft_contrast = p_settings.contrast;
			cached_soft_contrast_pivot = p_settings.color_modes.y;
			cached_soft_contrast_bias = (1.0f - cached_soft_contrast) * Math::log(cached_soft_contrast_pivot / (1.0f - cached_soft_contrast_pivot));
		}
		tonemap.push_constant.adjustment_controls[3] = cached_soft_contrast_bias;
	}

	tonemap.push_constant.flags |= p_settings.use_glow ? TONEMAP_FLAG_USE_GLOW : 0;
	tonemap.push_constant.glow_intensity = p_settings.glow_intensity;
	tonemap.push_constant.glow_map_strength = p_settings.glow_map_strength;
	tonemap.push_constant.glow_levels[0] = p_settings.glow_levels[0]; // clean this up to just pass by pointer or something
	tonemap.push_constant.glow_levels[1] = p_settings.glow_levels[1];
	tonemap.push_constant.glow_levels[2] = p_settings.glow_levels[2];
	tonemap.push_constant.glow_levels[3] = p_settings.glow_levels[3];
	tonemap.push_constant.glow_levels[4] = p_settings.glow_levels[4];
	tonemap.push_constant.glow_levels[5] = p_settings.glow_levels[5];
	tonemap.push_constant.glow_levels[6] = p_settings.glow_levels[6];
	tonemap.push_constant.glow_texture_size[0] = p_settings.glow_texture_size.x;
	tonemap.push_constant.glow_texture_size[1] = p_settings.glow_texture_size.y;
	tonemap.push_constant.glow_mode = p_settings.glow_mode;

	int mode = p_settings.glow_use_bicubic_upscale ? TONEMAP_MODE_BICUBIC_GLOW_FILTER : TONEMAP_MODE_NORMAL;

	tonemap.push_constant.tonemapper = p_settings.tonemap_mode;
	tonemap.push_constant.tonemapper_params[0] = p_settings.tonemapper_params[0];
	tonemap.push_constant.tonemapper_params[1] = p_settings.tonemapper_params[1];
	tonemap.push_constant.tonemapper_params[2] = p_settings.tonemapper_params[2];
	tonemap.push_constant.tonemapper_params[3] = p_settings.tonemapper_params[3];
	tonemap.push_constant.flags |= p_settings.use_auto_exposure ? TONEMAP_FLAG_USE_AUTO_EXPOSURE : 0;
	tonemap.push_constant.exposure = p_settings.exposure;
	tonemap.push_constant.white = p_settings.white;
	tonemap.push_constant.auto_exposure_scale = p_settings.auto_exposure_scale;
	tonemap.push_constant.luminance_multiplier = p_settings.luminance_multiplier;
	tonemap.push_constant.output_max_value = MAX(p_settings.max_value, 1.0f);

	tonemap.push_constant.flags |= p_settings.use_fxaa ? TONEMAP_FLAG_USE_FXAA : 0;
	if (p_settings.debanding_mode == TonemapSettings::DEBANDING_MODE_8_BIT) {
		tonemap.push_constant.flags |= TONEMAP_FLAG_USE_8_BIT_DEBANDING;
	}
	tonemap.push_constant.pixel_size[0] = 1.0 / p_settings.texture_size.x;
	tonemap.push_constant.pixel_size[1] = 1.0 / p_settings.texture_size.y;

	tonemap.push_constant.flags |= p_settings.convert_to_srgb ? TONEMAP_FLAG_CONVERT_TO_SRGB : 0;

	if (p_settings.view_count > 1) {
		// Use USE_MULTIVIEW versions
		mode += TONEMAP_MODE_NORMAL_MULTIVIEW;
	}

	RID default_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID default_mipmap_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	RD::Uniform u_source_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ p_settings.bilinear_filtering ? default_sampler : nearest_sampler, p_source_color }));

	RD::Uniform u_exposure_texture;
	u_exposure_texture.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	u_exposure_texture.binding = 0;
	u_exposure_texture.append_id(default_sampler);
	u_exposure_texture.append_id(p_settings.exposure_texture);

	RD::Uniform u_glow_texture;
	u_glow_texture.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	u_glow_texture.binding = 0;
	u_glow_texture.append_id(default_mipmap_sampler);
	u_glow_texture.append_id(p_settings.glow_texture);

	RD::Uniform u_glow_map;
	u_glow_map.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	u_glow_map.binding = 1;
	u_glow_map.append_id(default_mipmap_sampler);
	u_glow_map.append_id(p_settings.glow_map);

	RD::Uniform u_white_balance;
	if (use_transform) {
		mode += TONEMAP_MODE_WHITE_BALANCE_NORMAL;
		RID buffer = _get_white_balance_buffer(p_settings.white_balance_matrix, p_settings.forward_white_balance_offset);
		ERR_FAIL_COND(buffer.is_null());
		u_white_balance = RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, Vector<RID>({ buffer }));
	}

	RD::Uniform u_forward_grading;
	if (use_forward_grading) {
		mode += TONEMAP_MODE_FORWARD_GRADING;
		RID buffer = _get_forward_grading_buffer(p_settings);
		ERR_FAIL_COND(buffer.is_null());
		u_forward_grading = RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, Vector<RID>({ buffer }));
	}
	LocalVector<RD::Uniform> curve_uniforms;
	if (use_curves) {
		mode += TONEMAP_MODE_FORWARD_CURVES;
		const RID textures[] = { p_settings.hue_vs_hue_texture, p_settings.hue_vs_saturation_texture, p_settings.saturation_vs_saturation_texture, p_settings.luminance_vs_saturation_texture };
		for (int i = 0; i < 4; i++) {
			curve_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, i, Vector<RID>({ default_sampler, textures[i] })));
		}
		// Both LUT dimensions have stable bindings; only the selected one is sampled.
		RID lut_3d = TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
		if ((forward_flags & RendererEnvironmentStorage::FORWARD_ADJUSTMENT_LUT) != 0 && !p_settings.use_1d_color_correction) {
			lut_3d = p_settings.color_correction_texture;
		}
		curve_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ default_sampler, (forward_flags & RendererEnvironmentStorage::FORWARD_ADJUSTMENT_LUT) != 0 && p_settings.use_1d_color_correction ? p_settings.color_correction_texture : TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_WHITE) })));
		curve_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ default_sampler, lut_3d })));
	}
	RID shader = tonemap.shader.version_get_shader(tonemap.shader_version, mode);
	ERR_FAIL_COND(shader.is_null());

	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_dst_framebuffer);
	RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, tonemap.pipelines[mode].get_render_pipeline(RD::INVALID_ID, RD::get_singleton()->framebuffer_get_format(p_dst_framebuffer), false, RD::get_singleton()->draw_list_get_current_pass(), adjustment_specializations));
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set_cache->get_cache(shader, 0, u_source_color), 0);
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set_cache->get_cache(shader, 1, u_exposure_texture), 1);
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set_cache->get_cache(shader, 2, u_glow_texture, u_glow_map), 2);

	if (use_transform) {
		RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set_cache->get_cache(shader, 3, u_white_balance), 3);
	}

	if (use_forward_grading) {
		RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set_cache->get_cache(shader, 4, u_forward_grading), 4);
	}
	if (use_curves) {
		RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set_cache->get_cache_vec(shader, 5, curve_uniforms), 5);
	}
	RD::get_singleton()->draw_list_set_push_constant(draw_list, &tonemap.push_constant, sizeof(TonemapPushConstant));
	RD::get_singleton()->draw_list_draw(draw_list, false, 1u, 3u);
	RD::get_singleton()->draw_list_end();
}

void ToneMapper::tonemapper_mobile(RID p_source_color, RID p_dst_framebuffer, const TonemapSettings &p_settings) {
	ERR_FAIL_COND_MSG(!using_mobile_version, "Can't use the mobile version of the tonemapper with the clustered renderer.");
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	memset(&tonemap_mobile.push_constant, 0, sizeof(TonemapPushConstantMobile));

	tonemap_mobile.push_constant.bcs[0] = p_settings.brightness;
	tonemap_mobile.push_constant.bcs[1] = p_settings.contrast;
	tonemap_mobile.push_constant.bcs[2] = p_settings.saturation;

	tonemap_mobile.push_constant.src_pixel_size[0] = 1.0 / p_settings.texture_size.x;
	tonemap_mobile.push_constant.src_pixel_size[1] = 1.0 / p_settings.texture_size.y;
	tonemap_mobile.push_constant.dest_pixel_size[0] = 1.0 / p_settings.dest_texture_size.x;
	tonemap_mobile.push_constant.dest_pixel_size[1] = 1.0 / p_settings.dest_texture_size.y;
	tonemap_mobile.push_constant.glow_intensity = p_settings.glow_intensity;
	tonemap_mobile.push_constant.glow_map_strength = p_settings.glow_map_strength;

	tonemap_mobile.push_constant.exposure = p_settings.exposure;
	tonemap_mobile.push_constant.white = p_settings.white;
	tonemap_mobile.push_constant.luminance_multiplier = p_settings.luminance_multiplier;
	tonemap_mobile.push_constant.output_max_value = MAX(p_settings.max_value, 1.0f);
	tonemap_mobile.push_constant.tonemap_temperature[0] = p_settings.tonemap_temperature[0];
	tonemap_mobile.push_constant.tonemap_temperature[1] = p_settings.tonemap_temperature[1];
	tonemap_mobile.push_constant.tonemap_temperature[2] = p_settings.tonemap_temperature[2];
	tonemap_mobile.push_constant.vibrance = p_settings.vibrance;
	tonemap_mobile.push_constant.color_adjustment_flags = p_settings.color_modes.x;
	tonemap_mobile.push_constant.contrast_pivot = p_settings.color_modes.y;
	tonemap_mobile.push_constant.vignette = p_settings.vignette;
	tonemap_mobile.push_constant.vignette_range[0] = p_settings.vignette_start;
	tonemap_mobile.push_constant.vignette_range[1] = p_settings.vignette_end;

	tonemap_mobile.push_constant.tonemapper_params[0] = p_settings.tonemapper_params[0];
	tonemap_mobile.push_constant.tonemapper_params[1] = p_settings.tonemapper_params[1];
	tonemap_mobile.push_constant.tonemapper_params[2] = p_settings.tonemapper_params[2];
	tonemap_mobile.push_constant.tonemapper_params[3] = p_settings.tonemapper_params[3];
	tonemap_mobile.push_constant.tint_midtones_range[0] = p_settings.tint;
	tonemap_mobile.push_constant.tint_midtones_range[1] = p_settings.midtones_start;
	tonemap_mobile.push_constant.tint_midtones_range[2] = p_settings.midtones_end;
	tonemap_mobile.push_constant.tonal_softness[0] = p_settings.shadows_start;
	tonemap_mobile.push_constant.tonal_softness[1] = p_settings.shadows_end;
	tonemap_mobile.push_constant.tonal_softness[2] = p_settings.highlights_start;
	tonemap_mobile.push_constant.tonal_softness[3] = p_settings.highlights_end;

	uint32_t spec_constant = 0;
	spec_constant |= p_settings.use_bcs ? TONEMAP_MOBILE_FLAG_USE_BCS : 0;
	spec_constant |= p_settings.use_glow ? TONEMAP_MOBILE_FLAG_USE_GLOW : 0;
	spec_constant |= p_settings.glow_map_strength > 0.01 ? TONEMAP_MOBILE_FLAG_USE_GLOW_MAP : 0;
	spec_constant |= p_settings.use_color_correction ? TONEMAP_MOBILE_FLAG_USE_COLOR_CORRECTION : 0;
	spec_constant |= p_settings.use_fxaa ? TONEMAP_MOBILE_FLAG_USE_FXAA : 0;
	spec_constant |= p_settings.debanding_mode == TonemapSettings::DEBANDING_MODE_8_BIT ? TONEMAP_MOBILE_FLAG_USE_8_BIT_DEBANDING : 0;
	spec_constant |= p_settings.debanding_mode == TonemapSettings::DEBANDING_MODE_10_BIT ? TONEMAP_MOBILE_FLAG_USE_10_BIT_DEBANDING : 0;
	spec_constant |= p_settings.convert_to_srgb ? TONEMAP_MOBILE_FLAG_CONVERT_TO_SRGB : 0;
	spec_constant |= p_settings.tonemap_mode == RSE::ENV_TONE_MAPPER_LINEAR ? TONEMAP_MOBILE_FLAG_TONEMAPPER_LINEAR : 0;
	spec_constant |= p_settings.tonemap_mode == RSE::ENV_TONE_MAPPER_REINHARD ? TONEMAP_MOBILE_FLAG_TONEMAPPER_REINHARD : 0;
	spec_constant |= p_settings.tonemap_mode == RSE::ENV_TONE_MAPPER_FILMIC ? TONEMAP_MOBILE_FLAG_TONEMAPPER_FILMIC : 0;
	spec_constant |= p_settings.tonemap_mode == RSE::ENV_TONE_MAPPER_ACES ? TONEMAP_MOBILE_FLAG_TONEMAPPER_ACES : 0;
	spec_constant |= p_settings.tonemap_mode == RSE::ENV_TONE_MAPPER_AGX ? TONEMAP_MOBILE_FLAG_TONEMAPPER_AGX : 0;
	spec_constant |= p_settings.use_color_grading ? TONEMAP_MOBILE_FLAG_USE_COLOR_GRADING : 0;
	spec_constant |= p_settings.use_color_grading_curves ? TONEMAP_MOBILE_FLAG_USE_COLOR_GRADING_CURVES : 0;
	spec_constant |= p_settings.glow_mode == RSE::ENV_GLOW_BLEND_MODE_ADDITIVE ? TONEMAP_MOBILE_FLAG_GLOW_MODE_ADD : 0;
	spec_constant |= p_settings.glow_mode == RSE::ENV_GLOW_BLEND_MODE_SCREEN ? TONEMAP_MOBILE_FLAG_GLOW_MODE_SCREEN : 0;
	spec_constant |= p_settings.glow_mode == RSE::ENV_GLOW_BLEND_MODE_SOFTLIGHT ? TONEMAP_MOBILE_FLAG_GLOW_MODE_SOFTLIGHT : 0;
	spec_constant |= p_settings.glow_mode == RSE::ENV_GLOW_BLEND_MODE_REPLACE ? TONEMAP_MOBILE_FLAG_GLOW_MODE_REPLACE : 0;
	spec_constant |= p_settings.glow_mode == RSE::ENV_GLOW_BLEND_MODE_MIX ? TONEMAP_MOBILE_FLAG_GLOW_MODE_MIX : 0;

	int mode = p_settings.use_1d_color_correction ? TONEMAP_MOBILE_MODE_1D_LUT : TONEMAP_MOBILE_MODE_NORMAL;

	if (p_settings.view_count > 1) {
		// Use USE_MULTIVIEW versions
		mode += 4;
	}

	RID default_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID default_mipmap_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	RD::Uniform u_source_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ p_settings.bilinear_filtering ? default_sampler : nearest_sampler, p_source_color }));

	RD::Uniform u_glow_texture;
	u_glow_texture.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	u_glow_texture.binding = 1;
	u_glow_texture.append_id(default_mipmap_sampler);
	u_glow_texture.append_id(p_settings.glow_texture);

	RD::Uniform u_glow_map;
	u_glow_map.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	u_glow_map.binding = 2;
	u_glow_map.append_id(default_mipmap_sampler);
	u_glow_map.append_id(p_settings.glow_map);

	RD::Uniform u_color_correction_texture;
	u_color_correction_texture.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	u_color_correction_texture.binding = 3;
	u_color_correction_texture.append_id(default_sampler);
	u_color_correction_texture.append_id(p_settings.color_correction_texture);
	RD::Uniform u_hue_vs_hue(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ default_sampler, p_settings.hue_vs_hue_texture }));
	RD::Uniform u_hue_vs_saturation(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ default_sampler, p_settings.hue_vs_saturation_texture }));
	RD::Uniform u_saturation_vs_saturation(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ default_sampler, p_settings.saturation_vs_saturation_texture }));
	RD::Uniform u_luminance_vs_saturation(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ default_sampler, p_settings.luminance_vs_saturation_texture }));
	RID grading_buffer = _upload_mobile_grading_buffer(p_settings);
	RD::Uniform u_grading(RD::UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC, 8, Vector<RID>({ grading_buffer }));

	RID shader = tonemap_mobile.shader.version_get_shader(tonemap_mobile.shader_version, mode);
	ERR_FAIL_COND(shader.is_null());

	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_dst_framebuffer);
	RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, tonemap_mobile.pipelines[mode].get_render_pipeline(RD::INVALID_ID, RD::get_singleton()->framebuffer_get_format(p_dst_framebuffer), false, RD::get_singleton()->draw_list_get_current_pass(), spec_constant));
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set_cache->get_cache(shader, 0, u_source_color, u_glow_texture, u_glow_map, u_color_correction_texture, u_hue_vs_hue, u_hue_vs_saturation, u_saturation_vs_saturation, u_luminance_vs_saturation, u_grading), 0);
	RD::get_singleton()->draw_list_set_push_constant(draw_list, &tonemap_mobile.push_constant, sizeof(TonemapPushConstantMobile));
	RD::get_singleton()->draw_list_draw(draw_list, false, 1u, 3u);
	RD::get_singleton()->draw_list_end();
}

void ToneMapper::tonemapper_subpass(RD::DrawListID p_subpass_draw_list, RID p_source_color, RD::FramebufferFormatID p_dst_format_id, const TonemapSettings &p_settings) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	ERR_FAIL_COND_MSG(p_settings.use_glow, "Glow is not supported when using subpasses.");

	memset(&tonemap_mobile.push_constant, 0, sizeof(TonemapPushConstantMobile));

	tonemap_mobile.push_constant.bcs[0] = p_settings.brightness;
	tonemap_mobile.push_constant.bcs[1] = p_settings.contrast;
	tonemap_mobile.push_constant.bcs[2] = p_settings.saturation;

	tonemap_mobile.push_constant.src_pixel_size[0] = 1.0 / p_settings.texture_size.x;
	tonemap_mobile.push_constant.src_pixel_size[1] = 1.0 / p_settings.texture_size.y;
	tonemap_mobile.push_constant.glow_intensity = p_settings.glow_intensity;
	tonemap_mobile.push_constant.glow_map_strength = p_settings.glow_map_strength;

	tonemap_mobile.push_constant.exposure = p_settings.exposure;
	tonemap_mobile.push_constant.white = p_settings.white;
	tonemap_mobile.push_constant.luminance_multiplier = p_settings.luminance_multiplier;
	tonemap_mobile.push_constant.output_max_value = MAX(p_settings.max_value, 1.0f);
	tonemap_mobile.push_constant.tonemap_temperature[0] = p_settings.tonemap_temperature[0];
	tonemap_mobile.push_constant.tonemap_temperature[1] = p_settings.tonemap_temperature[1];
	tonemap_mobile.push_constant.tonemap_temperature[2] = p_settings.tonemap_temperature[2];
	tonemap_mobile.push_constant.vibrance = p_settings.vibrance;
	tonemap_mobile.push_constant.color_adjustment_flags = p_settings.color_modes.x;
	tonemap_mobile.push_constant.contrast_pivot = p_settings.color_modes.y;
	tonemap_mobile.push_constant.vignette = p_settings.vignette;
	tonemap_mobile.push_constant.vignette_range[0] = p_settings.vignette_start;
	tonemap_mobile.push_constant.vignette_range[1] = p_settings.vignette_end;

	tonemap_mobile.push_constant.tonemapper_params[0] = p_settings.tonemapper_params[0];
	tonemap_mobile.push_constant.tonemapper_params[1] = p_settings.tonemapper_params[1];
	tonemap_mobile.push_constant.tonemapper_params[2] = p_settings.tonemapper_params[2];
	tonemap_mobile.push_constant.tonemapper_params[3] = p_settings.tonemapper_params[3];
	tonemap_mobile.push_constant.tint_midtones_range[0] = p_settings.tint;
	tonemap_mobile.push_constant.tint_midtones_range[1] = p_settings.midtones_start;
	tonemap_mobile.push_constant.tint_midtones_range[2] = p_settings.midtones_end;
	tonemap_mobile.push_constant.tonal_softness[0] = p_settings.shadows_start;
	tonemap_mobile.push_constant.tonal_softness[1] = p_settings.shadows_end;
	tonemap_mobile.push_constant.tonal_softness[2] = p_settings.highlights_start;
	tonemap_mobile.push_constant.tonal_softness[3] = p_settings.highlights_end;

	uint32_t spec_constant = TONEMAP_MOBILE_ADRENO_BUG;
	spec_constant |= p_settings.use_bcs ? TONEMAP_MOBILE_FLAG_USE_BCS : 0;
	//spec_constant |= p_settings.use_glow ? TONEMAP_MOBILE_FLAG_USE_GLOW : 0;
	//spec_constant |= p_settings.glow_map_strength > 0.01 ? TONEMAP_MOBILE_FLAG_USE_GLOW_MAP : 0;
	spec_constant |= p_settings.use_color_correction ? TONEMAP_MOBILE_FLAG_USE_COLOR_CORRECTION : 0;
	//spec_constant |= p_settings.use_fxaa ? TONEMAP_MOBILE_FLAG_USE_FXAA : 0;
	spec_constant |= p_settings.debanding_mode == TonemapSettings::DEBANDING_MODE_8_BIT ? TONEMAP_MOBILE_FLAG_USE_8_BIT_DEBANDING : 0;
	spec_constant |= p_settings.convert_to_srgb ? TONEMAP_MOBILE_FLAG_CONVERT_TO_SRGB : 0;
	spec_constant |= p_settings.tonemap_mode == RSE::ENV_TONE_MAPPER_LINEAR ? TONEMAP_MOBILE_FLAG_TONEMAPPER_LINEAR : 0;
	spec_constant |= p_settings.tonemap_mode == RSE::ENV_TONE_MAPPER_REINHARD ? TONEMAP_MOBILE_FLAG_TONEMAPPER_REINHARD : 0;
	spec_constant |= p_settings.tonemap_mode == RSE::ENV_TONE_MAPPER_FILMIC ? TONEMAP_MOBILE_FLAG_TONEMAPPER_FILMIC : 0;
	spec_constant |= p_settings.tonemap_mode == RSE::ENV_TONE_MAPPER_ACES ? TONEMAP_MOBILE_FLAG_TONEMAPPER_ACES : 0;
	spec_constant |= p_settings.tonemap_mode == RSE::ENV_TONE_MAPPER_AGX ? TONEMAP_MOBILE_FLAG_TONEMAPPER_AGX : 0;
	spec_constant |= p_settings.use_color_grading ? TONEMAP_MOBILE_FLAG_USE_COLOR_GRADING : 0;
	spec_constant |= p_settings.use_color_grading_curves ? TONEMAP_MOBILE_FLAG_USE_COLOR_GRADING_CURVES : 0;
	//spec_constant |= p_settings.glow_mode == RSE::ENV_GLOW_BLEND_MODE_ADDITIVE ? TONEMAP_MOBILE_FLAG_GLOW_MODE_ADD : 0;
	//spec_constant |= p_settings.glow_mode == RSE::ENV_GLOW_BLEND_MODE_SCREEN ? TONEMAP_MOBILE_FLAG_GLOW_MODE_SCREEN : 0;
	//spec_constant |= p_settings.glow_mode == RSE::ENV_GLOW_BLEND_MODE_SOFTLIGHT ? TONEMAP_MOBILE_FLAG_GLOW_MODE_SOFTLIGHT : 0;
	//spec_constant |= p_settings.glow_mode == RSE::ENV_GLOW_BLEND_MODE_REPLACE ? TONEMAP_MOBILE_FLAG_GLOW_MODE_REPLACE : 0;
	//spec_constant |= p_settings.glow_mode == RSE::ENV_GLOW_BLEND_MODE_MIX ? TONEMAP_MOBILE_FLAG_GLOW_MODE_MIX : 0;

	int mode = p_settings.use_1d_color_correction ? TONEMAP_MOBILE_MODE_SUBPASS_1D_LUT : TONEMAP_MOBILE_MODE_SUBPASS;
	if (p_settings.view_count > 1) {
		// Use USE_MULTIVIEW versions
		mode += 4;
	}

	RID default_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID default_mipmap_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	RD::Uniform u_source_color;
	u_source_color.uniform_type = RD::UNIFORM_TYPE_INPUT_ATTACHMENT;
	u_source_color.binding = 0;
	u_source_color.append_id(p_source_color);

	RD::Uniform u_glow_texture;
	u_glow_texture.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	u_glow_texture.binding = 1;
	u_glow_texture.append_id(default_mipmap_sampler);
	u_glow_texture.append_id(p_settings.glow_texture);

	RD::Uniform u_glow_map;
	u_glow_map.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	u_glow_map.binding = 2;
	u_glow_map.append_id(default_mipmap_sampler);
	u_glow_map.append_id(p_settings.glow_map);

	RD::Uniform u_color_correction_texture;
	u_color_correction_texture.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	u_color_correction_texture.binding = 3;
	u_color_correction_texture.append_id(default_sampler);
	u_color_correction_texture.append_id(p_settings.color_correction_texture);
	RD::Uniform u_hue_vs_hue(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ default_sampler, p_settings.hue_vs_hue_texture }));
	RD::Uniform u_hue_vs_saturation(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ default_sampler, p_settings.hue_vs_saturation_texture }));
	RD::Uniform u_saturation_vs_saturation(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ default_sampler, p_settings.saturation_vs_saturation_texture }));
	RD::Uniform u_luminance_vs_saturation(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ default_sampler, p_settings.luminance_vs_saturation_texture }));
	RID grading_buffer = _upload_mobile_grading_buffer(p_settings);
	RD::Uniform u_grading(RD::UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC, 8, Vector<RID>({ grading_buffer }));

	RID shader = tonemap_mobile.shader.version_get_shader(tonemap_mobile.shader_version, mode);
	ERR_FAIL_COND(shader.is_null());

	RD::get_singleton()->draw_list_bind_render_pipeline(p_subpass_draw_list, tonemap_mobile.pipelines[mode].get_render_pipeline(RD::INVALID_ID, p_dst_format_id, false, RD::get_singleton()->draw_list_get_current_pass(), spec_constant));
	RD::get_singleton()->draw_list_bind_uniform_set(p_subpass_draw_list, uniform_set_cache->get_cache(shader, 0, u_source_color, u_glow_texture, u_glow_map, u_color_correction_texture, u_hue_vs_hue, u_hue_vs_saturation, u_saturation_vs_saturation, u_luminance_vs_saturation, u_grading), 0);
	RD::get_singleton()->draw_list_set_push_constant(p_subpass_draw_list, &tonemap_mobile.push_constant, sizeof(TonemapPushConstantMobile));
	RD::get_singleton()->draw_list_draw(p_subpass_draw_list, false, 1u, 3u);
}

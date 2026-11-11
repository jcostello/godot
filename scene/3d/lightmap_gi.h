/**************************************************************************/
/*  lightmap_gi.h                                                         */
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

#pragma once

#include "core/io/resource.h"
#include "core/templates/safe_refcount.h"
#include "scene/3d/light_probe_data.h"
#include "scene/3d/lightmapper.h"
#include "scene/3d/visual_instance_3d.h"
#include "servers/rendering/lightmap_adjustments.h"

class CameraAttributes;
class Light3D;
class LightProbeSystem;
class Mesh;
class Sky;

class LightmapGIData : public Resource {
	GDCLASS(LightmapGIData, Resource);
	RES_BASE_EXTENSION("lmbake")

public:
	enum ShadowmaskMode {
		SHADOWMASK_MODE_NONE,
		SHADOWMASK_MODE_REPLACE,
		SHADOWMASK_MODE_OVERLAY,
		SHADOWMASK_MODE_ONLY,
	};

private:
	// The 'merged' texture atlases actually used by the renderer.
	Ref<TextureLayered> combined_light_texture;
	Ref<TextureLayered> combined_shadowmask_texture;
	Ref<TextureLayered> combined_direction_texture;

	// The temporary texture atlas arrays which are used for storage.
	// If a single atlas is too large, it's split and recombined during loading.
	TypedArray<TextureLayered> storage_light_textures;
	TypedArray<TextureLayered> storage_shadowmask_textures;
	TypedArray<TextureLayered> storage_direction_textures;

	bool uses_spherical_harmonics = false;
	bool interior = false;

	bool _uses_packed_directional = false;

	RID lightmap;
	AABB bounds;
	float baked_exposure = 1.0;
	Vector2 tonal_luminance_range = Vector2(0, -1); // Max below min: not baked.
	uint32_t lightprobe_hash = 0;
	SafeFlag has_probe_capture_data;
	Ref<LightProbeData> migrated_probe_data; // Session-only legacy migration, never serialized.

	struct User {
		NodePath path;
		int32_t sub_instance = 0;
		Rect2 uv_scale;
		int slice_index = 0;
	};

	Vector<User> users;

	void _set_user_data(const Array &p_data);
	Array _get_user_data() const;
	void _set_probe_data(const Dictionary &p_data);
	Dictionary _get_probe_data() const;

	void _reset_lightmap_textures();
	void _reset_shadowmask_textures();
	void _reset_direction_textures();

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	Ref<LightProbeData> get_migrated_probe_data() const { return migrated_probe_data; }
	void set_migrated_probe_data(const Ref<LightProbeData> &p_data) { migrated_probe_data = p_data; }
	void add_user(const NodePath &p_path, const Rect2 &p_uv_scale, int p_slice_index, int32_t p_sub_instance = -1);
	int get_user_count() const;
	NodePath get_user_path(int p_user) const;
	int32_t get_user_sub_instance(int p_user) const;
	Rect2 get_user_lightmap_uv_scale(int p_user) const;
	int get_user_lightmap_slice_index(int p_user) const;
	void clear_users();

#ifndef DISABLE_DEPRECATED
	void set_light_texture(const Ref<TextureLayered> &p_light_texture);
	Ref<TextureLayered> get_light_texture() const;

	void _set_light_textures_data(const Array &p_data);
	Array _get_light_textures_data() const;
#endif

	void set_uses_spherical_harmonics(bool p_enable);
	bool is_using_spherical_harmonics() const;

	void _set_uses_packed_directional(bool p_enable);
	bool _is_using_packed_directional() const;

	void update_shadowmask_mode(ShadowmaskMode p_mode);
	ShadowmaskMode get_shadowmask_mode() const;

	bool is_interior() const;
	float get_baked_exposure() const;
	void set_baked_exposure(float p_exposure);
	void set_lightmap_bounds(const AABB &p_bounds);
	void set_tonal_luminance_range(const Vector2 &p_range);
	Vector2 get_tonal_luminance_range() const;

	void set_capture_data(const AABB &p_bounds, bool p_interior, const PackedVector3Array &p_points, const PackedColorArray &p_point_sh, const PackedInt32Array &p_tetrahedra, const PackedInt32Array &p_bsp_tree, float p_baked_exposure, uint32_t p_lightprobe_hash);
	PackedVector3Array get_capture_points() const;
	PackedColorArray get_capture_sh() const;
	PackedInt32Array get_capture_tetrahedra() const;
	PackedInt32Array get_capture_bsp_tree() const;
	uint32_t get_lightprobe_hash() const;

	AABB get_capture_bounds() const;

	void clear();

	void set_lightmap_textures(const TypedArray<TextureLayered> &p_data);
	TypedArray<TextureLayered> get_lightmap_textures() const;

	void set_shadowmask_textures(const TypedArray<TextureLayered> &p_data);
	TypedArray<TextureLayered> get_shadowmask_textures() const;
	void clear_shadowmask_textures();
	bool has_shadowmask_textures();

	// Dominant-direction textures replace the L1 SH layers of directional lightmaps.
	void set_direction_textures(const TypedArray<TextureLayered> &p_data);
	TypedArray<TextureLayered> get_direction_textures() const;

	void update_specular_intensity(float p_intensity);

	virtual RID get_rid() const override;
	LightmapGIData();
	~LightmapGIData();
};

class LightmapGI : public VisualInstance3D {
	GDCLASS(LightmapGI, VisualInstance3D);

public:
	enum BakeQuality {
		BAKE_QUALITY_LOW,
		BAKE_QUALITY_MEDIUM,
		BAKE_QUALITY_HIGH,
		BAKE_QUALITY_ULTRA,
	};

	enum BakeMode {
		BAKE_MODE_LIGHTMAP_AND_PROBES,
		BAKE_MODE_LIGHTMAP_ONLY,
		BAKE_MODE_PROBES_ONLY,
	};

	enum BakeError {
		BAKE_ERROR_OK,
		BAKE_ERROR_NO_SCENE_ROOT,
		BAKE_ERROR_FOREIGN_DATA,
		BAKE_ERROR_NO_LIGHTMAPPER,
		BAKE_ERROR_NO_SAVE_PATH,
		BAKE_ERROR_NO_MESHES,
		BAKE_ERROR_MESHES_INVALID,
		BAKE_ERROR_CANT_CREATE_IMAGE,
		BAKE_ERROR_USER_ABORTED,
		BAKE_ERROR_TEXTURE_SIZE_TOO_SMALL,
		BAKE_ERROR_LIGHTMAP_TOO_SMALL,
		BAKE_ERROR_ATLAS_TOO_SMALL,
		BAKE_ERROR_NO_PROBE_SYSTEM,
	};

	enum EnvironmentMode {
		ENVIRONMENT_MODE_DISABLED,
		ENVIRONMENT_MODE_SCENE,
		ENVIRONMENT_MODE_CUSTOM_SKY,
		ENVIRONMENT_MODE_CUSTOM_COLOR,
	};

	enum DebugMode {
		DEBUG_DISABLED,
		DEBUG_SHADOWS,
		DEBUG_MIDDLETONES,
		DEBUG_HIGHLIGHTS,
	};

	enum DirectionalFormat {
		DIRECTIONAL_FORMAT_SPHERICAL_HARMONICS,
		DIRECTIONAL_FORMAT_DOMINANT_DIRECTION,
	};

private:
	BakeQuality bake_quality = BAKE_QUALITY_MEDIUM;
	bool use_denoiser = true;
	float denoiser_strength = 0.1f;
	int denoiser_range = 10;
	int bounces = 3;
	float bounce_indirect_energy = 1.0;
	float indirect_sample_clamp = 32.0;
	float bias = 0.0005;
	bool bake_ao = false;
	float ao_distance = 1.0;
	float ao_strength = 1.0;
	float ao_power = 1.0;
	float ao_contact_distance = 0.25;
	float ao_contact_strength = 0.0;
	int ao_samples = 64;
	float texel_scale = 1.0;
	int max_texture_size = 16384;
	bool supersampling_enabled = false;
	float supersampling_factor = 2.0;
	bool interior = false;
	EnvironmentMode environment_mode = ENVIRONMENT_MODE_SCENE;
	float environment_energy = 1.0;
	Ref<Sky> environment_custom_sky;
	Color environment_custom_color = Color(1, 1, 1);
	float environment_custom_energy = 1.0;
	bool directional = false;
	DirectionalFormat directional_format = DIRECTIONAL_FORMAT_DOMINANT_DIRECTION;
	float directional_resolution_scale = 1.0f;
	bool use_texture_for_bounces = true;
	LightmapGIData::ShadowmaskMode shadowmask_mode = LightmapGIData::SHADOWMASK_MODE_NONE;
	Ref<CameraAttributes> camera_attributes;
	float specular_intensity = 0.0f;
	LightmapAdjustments adjustments;
	void _set_tonal_range(int p_index, float p_value);

	Ref<LightmapGIData> light_data;
	Node *last_owner = nullptr;

	struct LightsFound {
		Transform3D xform;
		Light3D *light = nullptr;
	};

	struct AreaLightAtlasTexture {
		Rect2 texture_rect;
		float max_mipmap;
	};

	struct MeshesFound {
		Transform3D xform;
		NodePath node_path;
		int32_t subindex = 0;
		Ref<Mesh> mesh;
		float lightmap_scale = 0.0;
		Vector<Ref<Material>> overrides;
	};

	struct BakeMaterialsUD {
		Vector<MeshesFound> *meshes = nullptr;
		float exposure_normalization = 1.0f;
	};

	static Lightmapper::BakeError _bake_material(int p_mesh_index, const Size2i &p_size, Ref<Image> &r_albedo, Ref<Image> &r_emission, void *p_userdata);

	void _find_meshes_and_lights(Node *p_at_node, Vector<MeshesFound> &meshes, Vector<LightsFound> &lights);

	void _migrate_legacy_probe_data();
	void _assign_lightmaps();
	void _clear_lightmaps();

	struct BakeStepUD {
		Lightmapper::BakeStepFunc func;
		void *ud = nullptr;
		float from_percent = 0.0;
		float to_percent = 0.0;
	};

	static bool _lightmap_bake_step_function(float p_completion, const String &p_text, void *ud, bool p_refresh);

	BakeError _save_and_reimport_atlas_textures(const Vector<Ref<Image>> &p_images, const String &p_base_name, TypedArray<TextureLayered> &r_textures, bool p_save_png, float p_resolution_scale, bool p_high_quality = false, Lightmapper *p_seam_stitcher = nullptr) const;
	void _build_area_light_texture_atlas(const Vector<LightmapGI::LightsFound> &lights_found, HashMap<Ref<Texture2D>, AreaLightAtlasTexture> &r_texture_rects, Size2i &r_atlas_size, int &r_mipmaps) const;

protected:
	bool _set(const StringName &p_name, const Variant &p_value);
	void _validate_property(PropertyInfo &p_property) const;
	static void _bind_methods();
	void _update_color_adjustments();
	void _notification(int p_what);

public:
	void set_light_data(const Ref<LightmapGIData> &p_data);
	Ref<LightmapGIData> get_light_data() const;

	void set_bake_quality(BakeQuality p_quality);
	BakeQuality get_bake_quality() const;

	void set_use_denoiser(bool p_enable);
	bool is_using_denoiser() const;

	void set_denoiser_strength(float p_denoiser_strength);
	float get_denoiser_strength() const;

	void set_denoiser_range(int p_denoiser_range);
	int get_denoiser_range() const;

	void set_directional(bool p_enable);
	bool is_directional() const;

	void set_directional_format(DirectionalFormat p_format);
	DirectionalFormat get_directional_format() const;

	void set_directional_resolution_scale(float p_scale);
	float get_directional_resolution_scale() const;

	void set_shadowmask_mode(LightmapGIData::ShadowmaskMode p_mode);
	LightmapGIData::ShadowmaskMode get_shadowmask_mode() const;

	void set_use_texture_for_bounces(bool p_enable);
	bool is_using_texture_for_bounces() const;

	void set_interior(bool p_interior);
	bool is_interior() const;

	void set_environment_mode(EnvironmentMode p_mode);
	EnvironmentMode get_environment_mode() const;

	void set_environment_energy(float p_energy);
	float get_environment_energy() const;

	void set_environment_custom_sky(const Ref<Sky> &p_sky);
	Ref<Sky> get_environment_custom_sky() const;

	void set_environment_custom_color(const Color &p_color);
	Color get_environment_custom_color() const;

	void set_environment_custom_energy(float p_energy);
	float get_environment_custom_energy() const;

	void set_bounces(int p_bounces);
	int get_bounces() const;

	void set_bounce_indirect_energy(float p_indirect_energy);
	float get_bounce_indirect_energy() const;

	void set_indirect_sample_clamp(float p_clamp);
	float get_indirect_sample_clamp() const;

	void set_bias(float p_bias);
	float get_bias() const;

	void set_bake_ao(bool p_enable);
	bool is_baking_ao() const;
	void set_ao_distance(float p_distance);
	float get_ao_distance() const;
	void set_ao_strength(float p_strength);
	float get_ao_strength() const;
	void set_ao_power(float p_power);
	float get_ao_power() const;
	void set_ao_contact_distance(float p_distance);
	float get_ao_contact_distance() const;
	void set_ao_contact_strength(float p_strength);
	float get_ao_contact_strength() const;
	void set_ao_samples(int p_samples);
	int get_ao_samples() const;

	void set_texel_scale(float p_multiplier);
	float get_texel_scale() const;

	void set_max_texture_size(int p_size);
	int get_max_texture_size() const;

	void set_supersampling_enabled(bool p_enable);
	bool is_supersampling_enabled() const;

	void set_supersampling_factor(float p_factor);
	float get_supersampling_factor() const;

	void set_camera_attributes(const Ref<CameraAttributes> &p_camera_attributes);
	Ref<CameraAttributes> get_camera_attributes() const;

	void set_shadows_black_point(float p_value);
	float get_shadows_black_point() const;
	void set_shadows_white_point(float p_value);
	float get_shadows_white_point() const;
	void set_shadows_gamma(float p_value);
	float get_shadows_gamma() const;
	void set_shadows_saturation(float p_value);
	float get_shadows_saturation() const;
	void set_shadows_contrast(float p_value);
	float get_shadows_contrast() const;
	void set_midtones_black_point(float p_value);
	float get_midtones_black_point() const;
	void set_midtones_white_point(float p_value);
	float get_midtones_white_point() const;
	void set_midtones_gamma(float p_value);
	float get_midtones_gamma() const;
	void set_midtones_saturation(float p_value);
	float get_midtones_saturation() const;
	void set_midtones_contrast(float p_value);
	float get_midtones_contrast() const;
	void set_highlights_black_point(float p_value);
	float get_highlights_black_point() const;
	void set_highlights_white_point(float p_value);
	float get_highlights_white_point() const;
	void set_highlights_gamma(float p_value);
	float get_highlights_gamma() const;
	void set_highlights_saturation(float p_value);
	float get_highlights_saturation() const;
	void set_highlights_contrast(float p_value);
	float get_highlights_contrast() const;

	void set_tonal_midtones_start(float p_value);
	float get_tonal_midtones_start() const;
	void set_tonal_shadows_end(float p_value);
	float get_tonal_shadows_end() const;
	void set_tonal_highlights_start(float p_value);
	float get_tonal_highlights_start() const;
	void set_tonal_midtones_end(float p_value);
	float get_tonal_midtones_end() const;
	void set_tonal_range_bottom(float p_value);
	float get_tonal_range_bottom() const;
	void set_tonal_range_top(float p_value);
	float get_tonal_range_top() const;
	void set_tonal_softness(float p_value);
	float get_tonal_softness() const;
	void set_shadows_tint(const Color &p_value);
	Color get_shadows_tint() const;
	void set_midtones_tint(const Color &p_value);
	Color get_midtones_tint() const;
	void set_highlights_tint(const Color &p_value);
	Color get_highlights_tint() const;
	void set_debug_mode(DebugMode p_value);
	DebugMode get_debug_mode() const;

	void set_adjustments_enabled(bool p_enabled);
	bool is_adjustments_enabled() const;

	void set_energy(float p_value);
	float get_energy() const;
	void set_tint(const Color &p_value);
	Color get_tint() const;
	void set_saturation(float p_value);
	float get_saturation() const;
	void set_contrast(float p_value);
	float get_contrast() const;
	void set_shadows(float p_value);
	float get_shadows() const;
	void set_midtones(float p_value);
	float get_midtones() const;
	void set_highlights(float p_value);
	float get_highlights() const;
	void set_tonal_lower(int p_range, float p_value);
	float get_tonal_lower(int p_range) const;
	void set_tonal_upper(int p_range, float p_value);
	float get_tonal_upper(int p_range) const;

	float get_specular_intensity() const;
	void set_specular_intensity(float p_strength);

	AABB get_aabb() const override;

	BakeError bake(Node *p_from_node, String p_image_data_path = "", Lightmapper::BakeStepFunc p_bake_step = nullptr, void *p_bake_userdata = nullptr, BakeMode p_mode = BAKE_MODE_LIGHTMAP_AND_PROBES);

	virtual PackedStringArray get_configuration_warnings() const override;

	LightmapGI();
};

VARIANT_ENUM_CAST(LightmapGIData::ShadowmaskMode);
VARIANT_ENUM_CAST(LightmapGI::BakeQuality);
VARIANT_ENUM_CAST(LightmapGI::BakeMode);
VARIANT_ENUM_CAST(LightmapGI::BakeError);
VARIANT_ENUM_CAST(LightmapGI::EnvironmentMode);
VARIANT_ENUM_CAST(LightmapGI::DebugMode);
VARIANT_ENUM_CAST(LightmapGI::DirectionalFormat);

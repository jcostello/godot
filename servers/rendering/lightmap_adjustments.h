/**************************************************************************/
/*  lightmap_adjustments.h                                                */
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

#include "core/math/color.h"
#include "core/math/vector3.h"

#include <array>

// Runtime grading in linear HDR space. Keep the curve in sync with lightmap_adjustments_inc.glsl.
struct LightmapAdjustments {
	bool enabled = true;
	float energy = 1.0f;
	Color tint = Color(1, 1, 1);
	float saturation = 1.0f;
	float contrast = 1.0f;
	float shadows = 0.0f;
	float midtones = 0.0f;
	float highlights = 0.0f;
	float directionality_strength = 1.0f;
	Color tonal_ranges = Color(0.1f, 0.4f, 0.4f, 0.8f);
	Color shadows_tint = Color(1, 1, 1);
	Color midtones_tint = Color(1, 1, 1);
	Color highlights_tint = Color(1, 1, 1);
	Vector3 tonal_saturation = Vector3(1, 1, 1);
	Vector3 tonal_contrast = Vector3(1, 1, 1);
	int debug_mode = 0;

	void store(float *r_tint_energy, float *r_grading, float *r_tonal, float *r_shadows_tint, float *r_midtones_tint, float *r_highlights_tint, float *r_tonal_contrast, float *r_tonal_ranges) const {
		r_tint_energy[0] = tint.r;
		r_tint_energy[1] = tint.g;
		r_tint_energy[2] = tint.b;
		r_tint_energy[3] = energy;
		r_grading[0] = saturation;
		r_grading[1] = contrast;
		r_grading[2] = enabled ? directionality_strength : 1.0f;
		r_grading[3] = enabled ? 1.0f : 0.0f;
		r_tonal[0] = shadows;
		r_tonal[1] = midtones;
		r_tonal[2] = highlights;
		r_tonal[3] = enabled ? debug_mode : 0;
		const Color colors[3] = { shadows_tint, midtones_tint, highlights_tint };
		float *destinations[3] = { r_shadows_tint, r_midtones_tint, r_highlights_tint };
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				destinations[i][j] = colors[i][j];
			}
			destinations[i][3] = tonal_saturation[i];
			r_tonal_contrast[i] = tonal_contrast[i];
		}
		r_tonal_contrast[3] = has_tonal_adjustments() ? 1.0f : 0.0f;
		for (int i = 0; i < 4; i++) {
			r_tonal_ranges[i] = tonal_ranges[i];
		}
	}

	bool has_tonal_adjustments() const {
		return tonal_saturation != Vector3(1, 1, 1) || tonal_contrast != Vector3(1, 1, 1) || contrast != 1.0f || shadows != 0.0f || midtones != 0.0f || highlights != 0.0f ||
				shadows_tint.r != 1.0f || shadows_tint.g != 1.0f || shadows_tint.b != 1.0f ||
				midtones_tint.r != 1.0f || midtones_tint.g != 1.0f || midtones_tint.b != 1.0f ||
				highlights_tint.r != 1.0f || highlights_tint.g != 1.0f || highlights_tint.b != 1.0f;
	}

	bool probe_lighting_equal(const LightmapAdjustments &p_other) const {
		if (enabled != p_other.enabled) {
			return false;
		}
		if (!enabled || (energy == 0.0f && p_other.energy == 0.0f)) {
			return true;
		}
		return energy == p_other.energy && tint == p_other.tint && saturation == p_other.saturation &&
				contrast == p_other.contrast && shadows == p_other.shadows && midtones == p_other.midtones &&
				highlights == p_other.highlights && directionality_strength == p_other.directionality_strength &&
				shadows_tint == p_other.shadows_tint && midtones_tint == p_other.midtones_tint && highlights_tint == p_other.highlights_tint &&
				tonal_saturation == p_other.tonal_saturation && tonal_contrast == p_other.tonal_contrast &&
				(!has_tonal_adjustments() || tonal_ranges == p_other.tonal_ranges);
	}

private:
	Vector3 _apply(const Vector3 &p_light, bool p_tonal) const {
		Vector3 light = p_light;
		if (saturation != 1.0f) {
			const float luminance = p_light.dot(Vector3(0.2126f, 0.7152f, 0.0722f));
			light = Vector3(luminance, luminance, luminance).lerp(light, saturation);
		}
		if (p_tonal) {
			light = light.max(Vector3());
			const float luma = light.dot(Vector3(0.2126f, 0.7152f, 0.0722f));
			if (luma > 0.00001f) {
				const float range = luma / (1.0f + luma);
				const float shadow_weight = 1.0f - Math::smoothstep(tonal_ranges.r, tonal_ranges.g, range);
				const float highlight_weight = Math::smoothstep(tonal_ranges.b, tonal_ranges.a, range);
				const float stops = shadows * shadow_weight + midtones * (1.0f - shadow_weight - highlight_weight) + highlights * highlight_weight;
				const Vector3 weights(shadow_weight, 1.0f - shadow_weight - highlight_weight, highlight_weight);
				const float range_saturation = tonal_saturation.dot(weights);
				const float range_contrast = tonal_contrast.dot(weights);
				if (range_saturation != 1.0f) {
					light = Vector3(luma, luma, luma).lerp(light, range_saturation).max(Vector3());
				}
				const float adjusted_luma = light.dot(Vector3(0.2126f, 0.7152f, 0.0722f));
				const Color range_tint = shadows_tint * shadow_weight + midtones_tint * (1.0f - shadow_weight - highlight_weight) + highlights_tint * highlight_weight;
				light *= Vector3(range_tint.r, range_tint.g, range_tint.b);
				const float exponent = MIN(contrast * range_contrast, 4.0f);
				if (exponent != 1.0f) {
					light *= 0.18f * Math::pow(adjusted_luma / 0.18f, exponent) / MAX(adjusted_luma, 0.00001f);
				}
				if (stops != 0.0f) {
					light *= Math::pow(2.0f, stops);
				}
			}
		}
		return light * Vector3(tint.r, tint.g, tint.b) * energy;
	}

public:
	void apply_sh(Color *r_sh) const {
		if (!enabled) {
			return;
		}
		const bool tonal = has_tonal_adjustments();
		if (!tonal && energy == 1.0f && tint == Color(1, 1, 1) && saturation == 1.0f && directionality_strength == 1.0f) {
			return;
		}
		if (energy == 0.0f) {
			for (int i = 0; i < 9; i++) {
				r_sh[i].r = r_sh[i].g = r_sh[i].b = 0.0f;
			}
			return;
		}
		// Linear controls can operate directly on signed SH coefficients, without approximation.
		if (!tonal) {
			for (int i = 0; i < 9; i++) {
				Vector3 light = _apply(Vector3(r_sh[i].r, r_sh[i].g, r_sh[i].b), false);
				if (i > 0) {
					light *= directionality_strength;
				}
				r_sh[i] = Color(light.x, light.y, light.z, r_sh[i].a);
			}
			return;
		}

		// Project the graded irradiance back into SH using spherical quadrature.
		// Nonlinear grading is approximate for probes, whose representation has only nine coefficients.
		struct QuadratureSample {
			float evaluation[9];
			float projection[9];
		};
		static const std::array<QuadratureSample, 32> samples = []() {
			std::array<QuadratureSample, 32> result = {};
			static const float z_nodes[4] = { -0.86113631f, -0.33998104f, 0.33998104f, 0.86113631f };
			static const float z_weights[4] = { 0.34785485f, 0.65214515f, 0.65214515f, 0.34785485f };
			static const float convolution[9] = { (float)Math::PI, 2.0f * (float)Math::PI / 3.0f, 2.0f * (float)Math::PI / 3.0f, 2.0f * (float)Math::PI / 3.0f,
				(float)Math::PI / 4.0f, (float)Math::PI / 4.0f, (float)Math::PI / 4.0f, (float)Math::PI / 4.0f, (float)Math::PI / 4.0f };
			for (int j = 0; j < 4; j++) {
				const float z = z_nodes[j];
				const float radius = Math::sqrt(1.0f - z * z);
				for (int k = 0; k < 8; k++) {
					const float phi = (k + 0.5f) * (float)Math::TAU / 8.0f;
					const float x = radius * Math::cos(phi);
					const float y = radius * Math::sin(phi);
					const float basis[9] = { 0.282095f, 0.488603f * y, 0.488603f * z, 0.488603f * x,
						1.092548f * x * y, 1.092548f * y * z, 0.315392f * (3.0f * z * z - 1.0f), 1.092548f * x * z, 0.546274f * (x * x - y * y) };
					for (int i = 0; i < 9; i++) {
						result[j * 8 + k].evaluation[i] = basis[i] * convolution[i];
						result[j * 8 + k].projection[i] = basis[i] * z_weights[j] * (float)Math::TAU / 8.0f / convolution[i];
					}
				}
			}
			return result;
		}();
		Vector3 coefficients[9];
		Vector3 result[9];
		for (int i = 0; i < 9; i++) {
			coefficients[i] = Vector3(r_sh[i].r, r_sh[i].g, r_sh[i].b) * (i == 0 ? 1.0f : directionality_strength);
		}
		for (const QuadratureSample &sample : samples) {
			Vector3 light;
			for (int i = 0; i < 9; i++) {
				light += coefficients[i] * sample.evaluation[i];
			}
			light = _apply(light, true);
			for (int i = 0; i < 9; i++) {
				result[i] += light * sample.projection[i];
			}
		}
		for (int i = 0; i < 9; i++) {
			r_sh[i] = Color(result[i].x, result[i].y, result[i].z, r_sh[i].a);
		}
	}
};

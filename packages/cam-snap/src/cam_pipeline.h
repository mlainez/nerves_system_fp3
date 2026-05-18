/*
 * cam_pipeline.h — shared color processing knobs + tone curve formula.
 *
 * Both cam-snap (stills, full-MHC + JPEG) and cam-stream (live, NEON
 * downscaled + Venus H.264) include this header so they apply the same
 * white-balance, gamma, S-curve, and saturation parameters and produce
 * visually-equivalent output.
 *
 * Header-only — all functions are `static inline` and the LUTs are built
 * locally in each binary (sized 1024 entries for cam-snap's 10-bit path,
 * 256 for cam-stream's 8-bit path) using the same tone curve formula.
 *
 * Why values diverge in different commits:
 *   1. The WB ratios are sensor-specific (calibrated against android-ref2
 *      indoor light; see Project's `assets/camera-samples/`).
 *   2. The tone-curve params target a "phone-look" — punchier mids,
 *      gentle S-curve, gamma 2.2.
 *
 * IF YOU CHANGE A DEFAULT HERE, both binaries pick up the change after
 * a rebuild — that is the whole point of this header.
 */

#pragma once

#include <math.h>
#include <stdint.h>

/* ---------- Tone curve defaults (gamma + S-curve + saturation) -------- */
#define CAM_GAMMA_DEFAULT       2.2f
#define CAM_CONTRAST_DEFAULT    0.35f
#define CAM_SATURATION_DEFAULT  1.50f
#define CAM_BRIGHTNESS_DEFAULT  1.00f  /* pre-gamma linear multiplier */

/* ---------- Per-camera WB gains, Q8.8 (×256) --------------------------
 *
 * The values are calibrated against `assets/camera-samples/android-ref2`,
 * assuming cam-snap's MHC 5×5 demosaic. MHC's cross-channel interpolation
 * implicitly mixes some G into R/B which softens the raw WB excess; in
 * the per-binary _STREAM variants below the simpler bilinear demosaic
 * doesn't do that mixing, so the R-gain is dialed back to compensate
 * (otherwise the live stream comes out visibly redder than the still). */

/* Rear  s5kgm1sp — cam-snap (MHC):  G/R≈1.54, G/B≈1.24 */
#define CAM_WB_REAR_R_Q8       (uint16_t)(2.10f * 256)
#define CAM_WB_REAR_G_Q8       (uint16_t)(1.00f * 256)
#define CAM_WB_REAR_B_Q8       (uint16_t)(1.50f * 256)
/* Rear cam-stream (bilinear): reduce R-gain to match cam-snap's look */
#define CAM_WB_REAR_R_Q8_STREAM    (uint16_t)(1.50f * 256)
#define CAM_WB_REAR_B_Q8_STREAM    (uint16_t)(1.30f * 256)

/* Front s5k3p9sp — cam-snap (MHC):  G/R≈1.35, G/B≈1.43 */
#define CAM_WB_FRONT_R_Q8      (uint16_t)(1.75f * 256)
#define CAM_WB_FRONT_G_Q8      (uint16_t)(1.00f * 256)
#define CAM_WB_FRONT_B_Q8      (uint16_t)(2.15f * 256)
/* Front cam-stream (bilinear): similar compensation */
#define CAM_WB_FRONT_R_Q8_STREAM   (uint16_t)(1.30f * 256)
#define CAM_WB_FRONT_B_Q8_STREAM   (uint16_t)(1.80f * 256)

/* ---------- Sensor geometry (post-2× downscale crop is computed in
 * the consumer; the raw bayer dimensions are constants of the part) -- */
#define CAM_BAYER_W_REAR        4000
#define CAM_BAYER_H_REAR        3000
#define CAM_BAYER_W_FRONT       4608
#define CAM_BAYER_H_FRONT       3456

/* ---------- Shared tone curve formula ---------------------------------
 * Maps a normalised value v∈[0..1] through:
 *   1. Piecewise-power S-curve (contrast=0 → identity; contrast>0 deepens
 *      shadows and lifts highlights).
 *   2. Gamma encode (v ← v^(1/gamma_val)).
 * Returns the encoded value, still in [0..1] (caller scales to 8-bit). */
static inline float cam_pipeline_tone(float v, float gamma_val, float contrast)
{
	if (contrast > 0.001f) {
		float p = 1.0f + contrast;
		v = (v < 0.5f)
			? 0.5f * powf(2.0f * v, p)
			: 1.0f - 0.5f * powf(2.0f - 2.0f * v, p);
	}
	return powf(v, 1.0f / gamma_val);
}

/* Build an N-entry LUT (any N — 256 for 8-bit input, 1024 for 10-bit)
 * where lut[i] = 8-bit gamma-encoded output for normalised input i/(N-1).
 * `brightness` is a linear pre-gamma multiplier — 1.0 is neutral, 1.5
 * brightens the image, 0.7 darkens. Baked into the LUT for zero per-
 * pixel cost. */
static inline void cam_pipeline_build_lut(uint8_t *lut, int n,
					  float gamma_val, float contrast,
					  float brightness)
{
	float denom = (float)(n - 1);
	for (int i = 0; i < n; i++) {
		float v = (float)i / denom;
		v *= brightness;
		if (v > 1.0f) v = 1.0f;
		if (v < 0.0f) v = 0.0f;
		v = cam_pipeline_tone(v, gamma_val, contrast);
		int q = (int)(v * 255.0f + 0.5f);
		lut[i] = (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
	}
}

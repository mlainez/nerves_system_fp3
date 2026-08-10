/*
 * cam-snap — software ISP + JPEG encode for raw-Bayer CAMSS cameras.
 *
 * Goes from raw 10-bit packed Bayer (SGRBG10P) to a viewable JPEG:
 *
 *   1. set sensor exposure / analogue gain via V4L2 controls
 *   2. capture an mplane SGRBG10P frame after a few warm-up frames
 *   3. unpack 10-bit packed → linear 16-bit Bayer
 *   4. dump per-subpixel-position means (diagnostic)
 *   5. subtract black level
 *   6. optional gray-world auto white balance (off by default since
 *      gray-world fights any scene that isn't actually gray on
 *      average — `--awb` enables it, `--wb R G B` overrides)
 *   7. optional 3x3 colour matrix (off by default, `--ccm` enables)
 *   8. apply per-channel digital gain in Bayer space
 *   9. demosaic with libbayer2rgb-neon
 *  10. gamma 2.2 + clip to 8-bit
 *  11. JPEG encode via libjpeg-turbo
 *
 * The Bayer pattern is selectable from the CLI so we can run a quick
 * sanity check by capturing the same scene four times — the right
 * pattern is the one whose output looks normal.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <bayer2rgb/bayer2rgb.h>
#include <jpeglib.h>

#include "phone-curve.h"
#include "cam_pipeline.h"

#define NUM_BUFFERS 4
#define BLACK_LEVEL 64      /* 10-bit space, typical Samsung sensor */
#define MAX_VALUE  1023
#define WARMUP_FRAMES 3
#define MAX_AVG_FRAMES 9    /* multi-frame noise reduction cap */

static const float GAMMA = CAM_GAMMA_DEFAULT;

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
	return r;
}

static void die(const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
	fputc('\n', stderr); exit(1);
}

static void set_ctrl(int fd, uint32_t id, int32_t val, const char *name)
{
	struct v4l2_control c = { .id = id, .value = val };
	if (xioctl(fd, VIDIOC_S_CTRL, &c) < 0)
		fprintf(stderr, "warn: set %s=%d failed: %s\n",
			name, val, strerror(errno));
}

/* Unpack one row of SGRBG10P → uint16 little-endian 10-bit. */
static void unpack_row(const uint8_t *src, uint16_t *dst, unsigned int width)
{
	for (unsigned int x = 0; x < width; x += 4) {
		uint8_t lsbs = src[4];
		dst[0] = ((uint16_t)src[0] << 2) | (lsbs & 0x03);
		dst[1] = ((uint16_t)src[1] << 2) | ((lsbs >> 2) & 0x03);
		dst[2] = ((uint16_t)src[2] << 2) | ((lsbs >> 4) & 0x03);
		dst[3] = ((uint16_t)src[3] << 2) | ((lsbs >> 6) & 0x03);
		src += 5; dst += 4;
	}
}

static inline uint16_t clip_u10(int v)
{
	return v < 0 ? 0 : v > MAX_VALUE ? MAX_VALUE : (uint16_t)v;
}

/* Compute mean of each (y%2, x%2) sub-grid — pure diagnostic, no
 * interpretation. The caller maps these to R/G1/G2/B based on the
 * chosen Bayer pattern. We sub-sample by 8 in both axes; for a
 * 4000x3000 frame that's ~187k samples per quadrant, plenty. */
struct quad_means {
	double mean[2][2];  /* indexed by [y&1][x&1] */
	uint64_t count[2][2];
};

/*
 * Per-quadrant mean, excluding pixels saturated near the top (>= 950) or
 * stuck near black level (<= 80). Saturated pixels poison gray-world
 * (a blown-out white window makes G look way higher than R/B because
 * G saturates first in most sensors), and very-dark pixels are mostly
 * sensor noise.
 */
static void compute_quad_means(const uint16_t *bayer,
			       unsigned int w, unsigned int h,
			       struct quad_means *qm)
{
	uint64_t sum[2][2] = {{0}};
	uint64_t cnt[2][2] = {{0}};
	const unsigned int step = 16;
	for (int yp = 0; yp < 2; yp++)
		for (int xp = 0; xp < 2; xp++)
			for (unsigned int y = yp; y < h; y += step)
				for (unsigned int x = xp; x < w; x += step) {
					uint16_t v = bayer[y * w + x];
					if (v < 80 || v > 950) continue;
					sum[yp][xp] += v;
					cnt[yp][xp]++;
				}
	for (int i = 0; i < 2; i++)
		for (int j = 0; j < 2; j++) {
			qm->mean[i][j] = cnt[i][j] ? (double)sum[i][j]/cnt[i][j] : 0;
			qm->count[i][j] = cnt[i][j];
		}
}

/*
 * Bayer pattern enum. Names match the libbayer2rgb-neon enum and the
 * V4L2 fourcc convention: "GRBG" = first 2x2 block is (G,R) top row,
 * (B,G) bottom row.
 */
enum bayer_pattern { BP_GRBG, BP_RGGB, BP_BGGR, BP_GBRG };

static int parse_bayer(const char *s, enum bayer_pattern *bp)
{
	if (!s) return -1;
	char up[5] = {0}; size_t n = strlen(s);
	if (n != 4) return -1;
	for (int i = 0; i < 4; i++) up[i] = toupper((unsigned char)s[i]);
	if (!strcmp(up, "GRBG")) { *bp = BP_GRBG; return 0; }
	if (!strcmp(up, "RGGB")) { *bp = BP_RGGB; return 0; }
	if (!strcmp(up, "BGGR")) { *bp = BP_BGGR; return 0; }
	if (!strcmp(up, "GBRG")) { *bp = BP_GBRG; return 0; }
	return -1;
}

/* Given the Bayer pattern and a quad_means struct, return per-channel
 * means by mapping (y&1, x&1) → R/G/B. There are two G positions; we
 * average them. */
static void quad_to_rgb(enum bayer_pattern bp, const struct quad_means *qm,
			double *r, double *g, double *b)
{
	int rxy[2], bxy[2], g1[2], g2[2];
	switch (bp) {
	/* (y, x) of each channel within the 2x2 block. */
	case BP_GRBG: g1[0]=0;g1[1]=0; rxy[0]=0;rxy[1]=1; bxy[0]=1;bxy[1]=0; g2[0]=1;g2[1]=1; break;
	case BP_RGGB: rxy[0]=0;rxy[1]=0; g1[0]=0;g1[1]=1; g2[0]=1;g2[1]=0; bxy[0]=1;bxy[1]=1; break;
	case BP_BGGR: bxy[0]=0;bxy[1]=0; g1[0]=0;g1[1]=1; g2[0]=1;g2[1]=0; rxy[0]=1;rxy[1]=1; break;
	case BP_GBRG: g1[0]=0;g1[1]=0; bxy[0]=0;bxy[1]=1; rxy[0]=1;rxy[1]=0; g2[0]=1;g2[1]=1; break;
	}
	*r = qm->mean[rxy[0]][rxy[1]];
	*b = qm->mean[bxy[0]][bxy[1]];
	*g = (qm->mean[g1[0]][g1[1]] + qm->mean[g2[0]][g2[1]]) / 2.0;
}

static int bayer_to_lib(enum bayer_pattern bp)
{
	switch (bp) {
	case BP_GRBG: return BAYER_GRBG;
	case BP_RGGB: return BAYER_RGGB;
	case BP_BGGR: return BAYER_BGGR;
	case BP_GBRG: return BAYER_GBRG;
	}
	return BAYER_GRBG;
}

/*
 * The capture format has to carry the same Bayer order as the pipeline.
 * CAMSS compares the video node's format against the source pad it is
 * fed from and fails VIDIOC_STREAMON with -EPIPE on a mismatch, so
 * asking for GRBG from an RGGB sensor does not produce wrong colours,
 * it produces no frame at all.
 */
static uint32_t bayer_to_v4l2(enum bayer_pattern bp)
{
	switch (bp) {
	case BP_GRBG: return V4L2_PIX_FMT_SGRBG10P;
	case BP_RGGB: return V4L2_PIX_FMT_SRGGB10P;
	case BP_BGGR: return V4L2_PIX_FMT_SBGGR10P;
	case BP_GBRG: return V4L2_PIX_FMT_SGBRG10P;
	}
	return V4L2_PIX_FMT_SGRBG10P;
}

/*
 * In-place black-level subtract + per-channel digital gain + radial
 * lens shading correction. LSC is approximated as a 1 + lsc_amount *
 * r²/r_max² brightness boost from center to corner — a stand-in for a
 * proper flat-field calibration. lsc_amount of 0.4-0.6 visibly de-
 * vignettes corners without obvious overshoot.
 */
static void apply_bls_and_gain(uint16_t *bayer, unsigned int w, unsigned int h,
			       enum bayer_pattern bp,
			       float gain_r, float gain_g, float gain_b,
			       float lsc_amount)
{
	/* For each (y&1, x&1) cell, what channel sits there? */
	float cell[2][2];
	switch (bp) {
	case BP_GRBG: cell[0][0]=gain_g; cell[0][1]=gain_r; cell[1][0]=gain_b; cell[1][1]=gain_g; break;
	case BP_RGGB: cell[0][0]=gain_r; cell[0][1]=gain_g; cell[1][0]=gain_g; cell[1][1]=gain_b; break;
	case BP_BGGR: cell[0][0]=gain_b; cell[0][1]=gain_g; cell[1][0]=gain_g; cell[1][1]=gain_r; break;
	case BP_GBRG: cell[0][0]=gain_g; cell[0][1]=gain_b; cell[1][0]=gain_r; cell[1][1]=gain_g; break;
	}
	const float cx = (float)w / 2.0f;
	const float cy = (float)h / 2.0f;
	const float r_max_sq = cx * cx + cy * cy;
	const bool do_lsc = lsc_amount > 0.001f;
	for (unsigned int y = 0; y < h; y++) {
		uint16_t *row = bayer + y * w;
		float gl = cell[y & 1][0];
		float gr = cell[y & 1][1];
		float dy = (float)y - cy;
		for (unsigned int x = 0; x < w; x++) {
			int v = (int)row[x] - BLACK_LEVEL;
			if (v < 0) v = 0;
			float g = (x & 1) ? gr : gl;
			if (do_lsc) {
				float dx = (float)x - cx;
				float r2 = (dx*dx + dy*dy) / r_max_sq;
				g *= (1.0f + lsc_amount * r2);
			}
			row[x] = clip_u10((int)(v * g + 0.5f));
		}
	}
}

/*
 * Sum of |Laplacian|² over a central crop of the raw Bayer. This is
 * the focus metric used by the contrast-detect AF sweep. Higher means
 * sharper. We work directly on the green-ish channel (the dominant
 * subgrid: positions (0,0) and (1,1) under GRBG) — proxy for luma,
 * avoids demosaicing, and is fast enough to do per AF probe frame.
 */
static double focus_metric(const uint16_t *bayer, unsigned int w, unsigned int h)
{
	/* Sample a 1024-wide × 768-tall center crop, walking on the
	 * "G at (0,0)" lattice so spacing is consistent (we step by 2
	 * in both axes to stay on G pixels of one sub-grid). */
	unsigned int cw = w > 1024 ? 1024 : w;
	unsigned int ch = h > 768 ? 768 : h;
	unsigned int x0 = (w - cw) / 2; if (x0 & 1) x0--;
	unsigned int y0 = (h - ch) / 2; if (y0 & 1) y0--;
	double acc = 0.0;
	for (unsigned int y = y0 + 2; y < y0 + ch - 2; y += 2) {
		const uint16_t *r0 = bayer + y * w;
		const uint16_t *rm = bayer + (y - 2) * w;
		const uint16_t *rp = bayer + (y + 2) * w;
		for (unsigned int x = x0 + 2; x < x0 + cw - 2; x += 2) {
			int c = r0[x];
			int lap = 4 * c - rm[x] - rp[x] - r0[x - 2] - r0[x + 2];
			acc += (double)lap * lap;
		}
	}
	return acc;
}

/*
 * Malvar-He-Cutler (MHC) 5x5 linear demosaic. Strictly sharper than the
 * bayer2rgb-neon ROUND_2 averaging path; preserves luma frequency content
 * by using cross-channel correlations. Works on 10-bit raw bayer in/out
 * 8-bit RGB. GRBG-only for now (the FP3+ S5KGM1SP layout).
 *
 * Reference: Malvar, He, Cutler. "High-Quality Linear Interpolation for
 * Demosaicing of Bayer-Patterned Color Images", IEEE ICASSP 2004.
 */
static inline uint8_t mhc_clamp_u8(int v)
{
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (uint8_t)v;
}

/* G at R or B center (4-connected G neighbors at ±1, same-color at ±2). */
static inline int mhc_g_at_rb(const uint16_t *p, int w)
{
	int s = 2 * (p[-w] + p[w] + p[-1] + p[1])
	      + 4 * p[0]
	      - (p[-2*w] + p[2*w] + p[-2] + p[2]);
	return s >> 3;
}

/* R at G_red_row (G center on a row that also has R), or B at G_blue_row. */
static inline int mhc_xc_at_g_samerow(const uint16_t *p, int w)
{
	int s = (p[-2*w] + p[2*w])
	      - 2 * (p[-w-1] + p[-w+1] + p[w-1] + p[w+1])
	      - 2 * (p[-2] + p[2])
	      + 8 * (p[-1] + p[1])
	      + 10 * p[0];
	return s >> 4;
}

/* R at G_blue_row (vertical neighbors are R), or B at G_red_row. */
static inline int mhc_xc_at_g_othrow(const uint16_t *p, int w)
{
	int s = (p[-2] + p[2])
	      - 2 * (p[-w-1] + p[-w+1] + p[w-1] + p[w+1])
	      - 2 * (p[-2*w] + p[2*w])
	      + 8 * (p[-w] + p[w])
	      + 10 * p[0];
	return s >> 4;
}

/* R at B center (or B at R) — diagonals are the same colour. */
static inline int mhc_xc_at_yc(const uint16_t *p, int w)
{
	int s = 4 * (p[-w-1] + p[-w+1] + p[w-1] + p[w+1])
	      + 12 * p[0]
	      - 3 * (p[-2*w] + p[2*w] + p[-2] + p[2]);
	return s >> 4;
}

/* GRBG bayer layout:
 *   (even y, even x) → G_r (green on red row)
 *   (even y, odd  x) → R
 *   (odd  y, even x) → B
 *   (odd  y, odd  x) → G_b (green on blue row)
 * 10-bit raw in, 8-bit RGB (pad to RGBx) out. Borders within 2 px of any
 * edge fall through to zero — same convention as the previous demosaic.
 */
static void demosaic_mhc_grbg(const uint16_t *bayer, uint8_t *rgbx,
			      unsigned int w, unsigned int h)
{
	memset(rgbx, 0, (size_t)w * h * 4);

	for (unsigned int y = 2; y + 2 < h; y++) {
		const uint16_t *row = bayer + (size_t)y * w;
		uint8_t *out_row = rgbx + (size_t)y * w * 4;
		int yodd = y & 1;
		for (unsigned int x = 2; x + 2 < w; x++) {
			const uint16_t *p = row + x;
			int xodd = x & 1;
			int r, g, b;

			if (!yodd && !xodd) {
				/* G_r: G centre; R horizontal at ±1; B vertical at ±1 */
				g = p[0];
				r = mhc_xc_at_g_samerow(p, (int)w);
				b = mhc_xc_at_g_othrow(p, (int)w);
			} else if (!yodd && xodd) {
				/* R centre; G 4-conn; B diagonals */
				r = p[0];
				g = mhc_g_at_rb(p, (int)w);
				b = mhc_xc_at_yc(p, (int)w);
			} else if (yodd && !xodd) {
				/* B centre; G 4-conn; R diagonals */
				b = p[0];
				g = mhc_g_at_rb(p, (int)w);
				r = mhc_xc_at_yc(p, (int)w);
			} else {
				/* G_b: G centre; B horizontal; R vertical */
				g = p[0];
				b = mhc_xc_at_g_samerow(p, (int)w);
				r = mhc_xc_at_g_othrow(p, (int)w);
			}

			uint8_t *o = out_row + x * 4;
			o[0] = mhc_clamp_u8(r >> 2);
			o[1] = mhc_clamp_u8(g >> 2);
			o[2] = mhc_clamp_u8(b >> 2);
			o[3] = 0;
		}
	}
}

/*
 * 5x5 bilateral filter, applied per RGB channel. Spatial Gaussian σ=1.5
 * (precomputed), intensity Gaussian σ=`strength`. `strength`≈10 → mild,
 * 25→strong. Skipped if strength<=0. Edges fall through unchanged.
 *
 * Per-pixel: 25 neighbour reads × 3 channels × (1 exp via LUT + MAC).
 * At 4000×3000 this is ≈900M ops; ~3–5 s on the FP3+ A53s.
 */
static uint8_t bilat_intensity_lut[256][256];
static int    bilat_intensity_lut_built_for = -1;
static const int BILAT_W_SPATIAL[5][5] = {
	/* exp(-(dx²+dy²)/(2*σ²)) × 64, σ=1.5  */
	{  3,  9, 14,  9,  3},
	{  9, 27, 41, 27,  9},
	{ 14, 41, 64, 41, 14},
	{  9, 27, 41, 27,  9},
	{  3,  9, 14,  9,  3},
};

static void bilat_build_intensity_lut(int sigma_r)
{
	if (bilat_intensity_lut_built_for == sigma_r) return;
	double s2 = 2.0 * (double)sigma_r * (double)sigma_r;
	for (int diff = 0; diff < 256; diff++) {
		double w = exp(-(double)(diff * diff) / s2);
		uint8_t v = (uint8_t)(w * 255.0 + 0.5);
		bilat_intensity_lut[diff][0] = v;
	}
	bilat_intensity_lut_built_for = sigma_r;
}

static inline int bilat_iw(int a, int b)
{
	int d = a - b;
	if (d < 0) d = -d;
	return bilat_intensity_lut[d][0];
}

static void bilateral_5x5(uint8_t *rgbx, unsigned int w, unsigned int h,
			  int strength)
{
	if (strength <= 0) return;
	bilat_build_intensity_lut(strength);

	size_t bytes = (size_t)w * h * 4;
	uint8_t *tmp = malloc(bytes);
	if (!tmp) return;
	memcpy(tmp, rgbx, bytes);

	for (unsigned int y = 2; y + 2 < h; y++) {
		for (unsigned int x = 2; x + 2 < w; x++) {
			uint8_t *out = rgbx + (size_t)((y * w + x) * 4);
			for (int c = 0; c < 3; c++) {
				int cv = tmp[(y * w + x) * 4 + c];
				int wsum = 0, vsum = 0;
				for (int dy = -2; dy <= 2; dy++) {
					const uint8_t *prow = tmp +
						(size_t)(((y + dy) * w + (x - 2)) * 4) + c;
					for (int dx = -2; dx <= 2; dx++) {
						int v = prow[dx * 4];
						int wsp = BILAT_W_SPATIAL[dy + 2][dx + 2];
						int wi = bilat_iw(cv, v);
						int wt = wsp * wi;
						wsum += wt;
						vsum += v * wt;
					}
				}
				out[c] = wsum > 0 ? (uint8_t)(vsum / wsum) : (uint8_t)cv;
			}
		}
	}
	free(tmp);
}

/*
 * Unsharp mask using a 5x5 Gaussian (σ=1.0) reference blur, then
 * `sharp = src + amount * (src - blur)` per channel. A 5x5 reference
 * catches a wider range of edge widths than a 3x3 box blur — important
 * when the bilateral denoise just smoothed mid-frequency content.
 * amount=0.5..2.5 is the useful range. Edges within 2 px untouched.
 */
static void unsharp_mask(uint8_t *rgbx, unsigned int w, unsigned int h,
			 float amount)
{
	if (w < 5 || h < 5 || amount <= 0.0f) return;
	uint8_t *blur = malloc((size_t)w * h * 4);
	if (!blur) return;

	/* 5x5 Gaussian σ=1.0, integer weights /256:
	 *   1  4  6  4 1
	 *   4 16 24 16 4
	 *   6 24 36 24 6
	 *   4 16 24 16 4
	 *   1  4  6  4 1
	 */
	static const int gk[5][5] = {
		{ 1,  4,  6,  4, 1},
		{ 4, 16, 24, 16, 4},
		{ 6, 24, 36, 24, 6},
		{ 4, 16, 24, 16, 4},
		{ 1,  4,  6,  4, 1},
	};

	for (unsigned int y = 2; y + 2 < h; y++) {
		for (unsigned int x = 2; x + 2 < w; x++) {
			for (int c = 0; c < 3; c++) {
				int s = 0;
				for (int dy = -2; dy <= 2; dy++)
					for (int dx = -2; dx <= 2; dx++)
						s += rgbx[((y+dy)*w + (x+dx)) * 4 + c]
						     * gk[dy+2][dx+2];
				blur[(y*w + x) * 4 + c] = (uint8_t)(s >> 8);
			}
		}
	}
	for (unsigned int y = 2; y + 2 < h; y++) {
		for (unsigned int x = 2; x + 2 < w; x++) {
			for (int c = 0; c < 3; c++) {
				int src = rgbx[(y*w + x) * 4 + c];
				int b   = blur[(y*w + x) * 4 + c];
				int v = src + (int)((src - b) * amount);
				if (v < 0) v = 0; else if (v > 255) v = 255;
				rgbx[(y*w + x) * 4 + c] = (uint8_t)v;
			}
		}
	}
	free(blur);
}

/*
 * 10-bit→8-bit tone LUT — gamma + S-curve. Built via the shared formula
 * in cam_pipeline.h so cam-stream's 256-entry version produces visually-
 * matching output for the same gamma/contrast parameters.
 */
static uint8_t lut[1024];
static void build_gamma_lut(float gamma_val, float contrast)
{
	/* cam-snap historically applies brightness as a per-pixel `exp_boost`
	 * multiplier inside rgbx_postprocess; pass 1.0 here so the LUT stays
	 * neutral and the per-pixel multiply still works the way it always
	 * has. cam-stream bakes brightness into the LUT instead — both yield
	 * the same visual result. */
	cam_pipeline_build_lut(lut, 1024, gamma_val, contrast, 1.0f);
}

/*
 * Optionally apply 3x3 CCM + exposure boost + saturation, always apply
 * the gamma/S-curve LUT, pack RGBx → RGB888 in-place. exp_boost is a
 * linear pre-gamma multiplier; sat boosts chroma by pulling each
 * pixel's R/G/B away from its luminance (Y = 0.299R + 0.587G + 0.114B)
 * by `sat`. sat=1 is identity, 1.3 is a typical phone-look.
 */
static void rgbx_postprocess(uint8_t *buf, unsigned int w, unsigned int h,
			     const float ccm[3][3], bool use_ccm,
			     float exp_boost, float sat, bool use_phone_curve)
{
	uint8_t *src = buf, *dst = buf;

	/*
	 * Phone-curve path: bypass our CCM/sat/exposure/gamma LUTs and
	 * use the per-channel histogram-match LUTs reverse-engineered
	 * from a side-by-side Android-vs-Nerves pair. The LUT input is
	 * the 8-bit RGB straight out of the demosaic + AWB + sharpen
	 * stages, output is what the Android pipeline would have
	 * produced from the same input.
	 */
	if (use_phone_curve) {
		for (unsigned int p = 0; p < w * h; p++) {
			dst[0] = phone_curve[0][src[0]];
			dst[1] = phone_curve[1][src[1]];
			dst[2] = phone_curve[2][src[2]];
			src += 4; dst += 3;
		}
		return;
	}

	float s = exp_boost * 4.0f;
	for (unsigned int p = 0; p < w * h; p++) {
		float r = src[0], g = src[1], b = src[2];
		src += 4;
		float R, G, B;
		if (use_ccm) {
			R = ccm[0][0]*r + ccm[0][1]*g + ccm[0][2]*b;
			G = ccm[1][0]*r + ccm[1][1]*g + ccm[1][2]*b;
			B = ccm[2][0]*r + ccm[2][1]*g + ccm[2][2]*b;
		} else { R = r; G = g; B = b; }

		if (sat != 1.0f) {
			float Y = 0.299f * R + 0.587f * G + 0.114f * B;
			R = Y + (R - Y) * sat;
			G = Y + (G - Y) * sat;
			B = Y + (B - Y) * sat;
		}

		int Ri = (int)(R * s + 0.5f);
		int Gi = (int)(G * s + 0.5f);
		int Bi = (int)(B * s + 0.5f);
		if (Ri < 0) Ri = 0; else if (Ri > 1023) Ri = 1023;
		if (Gi < 0) Gi = 0; else if (Gi > 1023) Gi = 1023;
		if (Bi < 0) Bi = 0; else if (Bi > 1023) Bi = 1023;
		dst[0] = lut[Ri]; dst[1] = lut[Gi]; dst[2] = lut[Bi];
		dst += 3;
	}
}

/*
 * Auto-levels: linear histogram stretch so the 2nd..98th percentile of
 * luminance lands on the target range [lo..hi]. Calibrated to match the
 * android-ref2 reference distribution ([4..250]). Operates in-place on
 * packed RGB888. The 2% bleed at each end lets a handful of true black
 * and specular highlights clip — same behaviour as a phone-class JPEG.
 */
static void apply_auto_levels(uint8_t *rgb, unsigned int w, unsigned int h,
			      int target_lo, int target_hi)
{
	unsigned int hist[256] = {0};
	unsigned int total = w * h;
	for (unsigned int i = 0; i < total; i++) {
		const uint8_t *p = rgb + i * 3;
		/* Rec.601 luma, integer: 0.299/0.587/0.114 × 256 = 77/150/29 */
		int Y = (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8;
		if (Y > 255) Y = 255;
		hist[Y]++;
	}
	unsigned int lo_count = (total * 2) / 100;
	unsigned int hi_count = (total * 98) / 100;
	int Y_lo = 0, Y_hi = 255;
	unsigned int cumul = 0;
	for (int i = 0; i < 256; i++) {
		cumul += hist[i];
		if (cumul <= lo_count) Y_lo = i;
		if (cumul <= hi_count) Y_hi = i;
	}
	if (Y_hi <= Y_lo + 5) return;  /* degenerate; flat image */

	int span = Y_hi - Y_lo;
	int tspan = target_hi - target_lo;
	fprintf(stderr, "auto-levels: Y[2..98] = [%d..%d] → [%d..%d]\n",
		Y_lo, Y_hi, target_lo, target_hi);

	/* Precompute LUT mapping in→out */
	uint8_t map[256];
	for (int v = 0; v < 256; v++) {
		int o = ((v - Y_lo) * tspan) / span + target_lo;
		if (o < 0) o = 0; else if (o > 255) o = 255;
		map[v] = (uint8_t)o;
	}
	for (unsigned int i = 0; i < total; i++) {
		uint8_t *p = rgb + i * 3;
		p[0] = map[p[0]];
		p[1] = map[p[1]];
		p[2] = map[p[2]];
	}
}

static void write_jpeg(const char *path, const uint8_t *rgb,
		       unsigned int w, unsigned int h, int quality)
{
	FILE *f = fopen(path, "wb");
	if (!f) die("open %s: %s", path, strerror(errno));
	struct jpeg_compress_struct ci; struct jpeg_error_mgr je;
	ci.err = jpeg_std_error(&je);
	jpeg_create_compress(&ci);
	jpeg_stdio_dest(&ci, f);
	ci.image_width = w; ci.image_height = h;
	ci.input_components = 3; ci.in_color_space = JCS_RGB;
	jpeg_set_defaults(&ci);
	jpeg_set_quality(&ci, quality, TRUE);
	jpeg_start_compress(&ci, TRUE);
	JSAMPROW row;
	while (ci.next_scanline < ci.image_height) {
		row = (JSAMPROW)(rgb + ci.next_scanline * w * 3);
		jpeg_write_scanlines(&ci, &row, 1);
	}
	jpeg_finish_compress(&ci); jpeg_destroy_compress(&ci); fclose(f);
}

static const char *USAGE =
	"usage: cam-snap --device DEV --width W --height H --out FILE\n"
	"               [--quality 1..100] [--gamma 2.2]\n"
	"               [--subdev /dev/v4l-subdevN]   sensor controls\n"
	"               [--lens /dev/v4l-subdevN] [--focus 0..1023]\n"
	"                                            (lens VCM, 0=∞, 1023=macro)\n"
	"               [--frames 1..9]              multi-frame averaging\n"
	"               [--exposure N] [--gain N]\n"
	"               [--bayer grbg|rggb|bggr|gbrg]   (default grbg)\n"
	"               [--awb]                         (gray-world AWB)\n"
	"               [--wb R G B]                    (manual gains; overrides --awb)\n"
	"               [--warm-bias 1.05]              (R-gain multiplier; LED+fluo are green-biased)\n"
	"               [--ccm]                         (apply built-in sRGB matrix)\n"
	"               [--lsc 0..1]                    (radial lens shading boost, def 0.4 with --lens)\n"
	"               [--autofocus]                   (contrast-detect AF sweep; overrides --focus)\n"
	"               [--contrast 0..1]               (S-curve strength, def 0.25)\n"
	"               [--sharpen [--sharpen-amount A]] (unsharp mask, A≈1.0)\n";

/*
 * Mildly sharp sRGB-ish CCM. The lower G→R/B coupling is intentional —
 * fluorescent / cheap LED indoor lighting has a green spike that AWB
 * alone doesn't fully neutralize, so we let the CCM cut a bit more
 * green out of R and B.
 */
static const float CCM_SRGB[3][3] = {
	{ 1.55f, -0.45f, -0.10f},
	{-0.15f,  1.30f, -0.15f},
	{-0.05f, -0.35f,  1.40f},
};

/*
 * Read back what fp3-cam-setup resolved for this slot. It has already
 * walked the media graph and knows which module is fitted, so this is
 * the one place the sensor's identity is established; everything here
 * only fills in values the caller did not pass explicitly.
 *
 * A missing file is not an error: the caller keeps its own defaults.
 */
static void load_cam_conf(const char *preset,
			  const char **dev, const char **subdev,
			  const char **lens,
			  unsigned int *width, unsigned int *height,
			  enum bayer_pattern *bp, bool *bayer_explicit)
{
	char path[64];
	snprintf(path, sizeof(path), "/run/fp3-cam-%s.conf", preset);

	FILE *f = fopen(path, "r");
	if (!f)
		return;

	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char *key = line, *val = eq + 1;

		val[strcspn(val, "\r\n")] = '\0';
		size_t vlen = strlen(val);
		if (vlen >= 2 && val[0] == '\'' && val[vlen - 1] == '\'') {
			val[vlen - 1] = '\0';
			val++;
		}
		if (!*val)
			continue;

		if (!strcmp(key, "VIDEO")) {
			if (!*dev) *dev = strdup(val);
		} else if (!strcmp(key, "SUBDEV")) {
			if (!*subdev) *subdev = strdup(val);
		} else if (!strcmp(key, "LENS")) {
			if (!*lens) *lens = strdup(val);
		} else if (!strcmp(key, "WIDTH")) {
			if (!*width) *width = (unsigned int)strtoul(val, NULL, 10);
		} else if (!strcmp(key, "HEIGHT")) {
			if (!*height) *height = (unsigned int)strtoul(val, NULL, 10);
		} else if (!strcmp(key, "BAYER")) {
			if (!*bayer_explicit && parse_bayer(val, bp) == 0)
				*bayer_explicit = true;
		}
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	const char *dev = NULL, *subdev = NULL, *out = NULL, *lens = NULL;
	unsigned int width = 0, height = 0;
	bool bayer_explicit = false;
	int quality = 90, exposure = -1, gain = -1, focus = -1;
	int avg_frames = 1;
	float gamma_val = GAMMA;
	/* Pop / phone-look defaults — calibrated against android-ref2:
	 * ref2 has Y-stddev ≈ 68 and per-pixel max-min ≈ 20 vs our previous
	 * 50 / 11. Bumping contrast and saturation closes most of the gap. */
	/* Phone-look defaults — picked side-by-side against android-ref2.
	 * Contrast=0.35 + saturation=1.5 reproduces the chroma punch ref2
	 * has without darkening mids the way contrast=0.5 did. */
	float contrast = 0.35f;
	bool auto_levels = false;
	int  auto_levels_lo = 4, auto_levels_hi = 250;
	enum bayer_pattern bp = BP_GRBG;
	/* Default sharpen on: the MHC demosaic + (optional) bilateral pair
	 * still trails Android's HW ISP — a fixed unsharp recovers most of
	 * that mid-frequency detail. amount auto-scales below if --denoise. */
	bool do_awb = false, do_ccm = false, do_sharpen = true, do_autofocus = false;
	bool sharpen_explicit = false;
	/* Default WB calibrated against android-ref2 on FP3+ S5KGM1SP under
	 * mixed indoor light: ΔRGB ≈ 6 to ref. Pass --awb or --wb to override. */
	float wb[3] = {2.1f, 1.0f, 1.5f};
	bool manual_wb = true;
	float sharpen_amount = 1.0f;
	float warm_bias = 1.05f;   /* boost R slightly to kill green-cast indoor LEDs */
	bool use_mhc = true;       /* Malvar-He-Cutler demosaic (sharper than bayer2rgb-neon) */
	int  denoise_strength = 4; /* mild bilateral; 0=off; pass --denoise 0 to disable */
	float lsc_amount = -1.0f;  /* -1 = auto: 0.4 if lens given, else 0.0 */
	float exp_boost = 1.0f;    /* pre-gamma RGB multiplier, lifts dim scenes */
	float saturation = 1.50f;  /* chroma boost; 1.0=neutral, 1.5≈ref2 pop */
	bool use_phone_curve = false;  /* histogram-matched Android-look LUTs */
	const char *camera_preset = NULL;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--camera") && i+1 < argc) camera_preset = argv[++i];
		else if (!strcmp(a, "--device") && i+1 < argc) dev = argv[++i];
		else if (!strcmp(a, "--subdev") && i+1 < argc) subdev = argv[++i];
		else if (!strcmp(a, "--lens") && i+1 < argc) lens = argv[++i];
		else if (!strcmp(a, "--focus") && i+1 < argc) focus = atoi(argv[++i]);
		else if (!strcmp(a, "--frames") && i+1 < argc) avg_frames = atoi(argv[++i]);
		else if (!strcmp(a, "--out") && i+1 < argc) out = argv[++i];
		else if (!strcmp(a, "--width") && i+1 < argc) width = atoi(argv[++i]);
		else if (!strcmp(a, "--height") && i+1 < argc) height = atoi(argv[++i]);
		else if (!strcmp(a, "--quality") && i+1 < argc) quality = atoi(argv[++i]);
		else if (!strcmp(a, "--gamma") && i+1 < argc) gamma_val = atof(argv[++i]);
		else if (!strcmp(a, "--exposure") && i+1 < argc) exposure = atoi(argv[++i]);
		else if (!strcmp(a, "--gain") && i+1 < argc) gain = atoi(argv[++i]);
		else if (!strcmp(a, "--bayer") && i+1 < argc) {
			if (parse_bayer(argv[++i], &bp))
				die("--bayer expects grbg/rggb/bggr/gbrg");
			bayer_explicit = true;
		}
		else if (!strcmp(a, "--awb")) do_awb = true;
		else if (!strcmp(a, "--ccm")) do_ccm = true;
		else if (!strcmp(a, "--autofocus")) do_autofocus = true;
		else if (!strcmp(a, "--warm-bias") && i+1 < argc) warm_bias = atof(argv[++i]);
		else if (!strcmp(a, "--lsc") && i+1 < argc) lsc_amount = atof(argv[++i]);
		else if (!strcmp(a, "--contrast") && i+1 < argc) contrast = atof(argv[++i]);
		else if (!strcmp(a, "--exposure-boost") && i+1 < argc) exp_boost = atof(argv[++i]);
		else if (!strcmp(a, "--saturation") && i+1 < argc) saturation = atof(argv[++i]);
		else if (!strcmp(a, "--phone-curve")) use_phone_curve = true;
		else if (!strcmp(a, "--no-mhc")) use_mhc = false;
		else if (!strcmp(a, "--no-auto-levels")) auto_levels = false;
		else if (!strcmp(a, "--auto-levels") && i+2 < argc) {
			auto_levels = true;
			auto_levels_lo = atoi(argv[++i]);
			auto_levels_hi = atoi(argv[++i]);
		}
		else if (!strcmp(a, "--denoise") && i+1 < argc) denoise_strength = atoi(argv[++i]);
		else if (!strcmp(a, "--sharpen")) { do_sharpen = true; sharpen_explicit = true; }
		else if (!strcmp(a, "--no-sharpen")) { do_sharpen = false; sharpen_explicit = true; }
		else if (!strcmp(a, "--sharpen-amount") && i+1 < argc) {
			do_sharpen = true; sharpen_explicit = true; sharpen_amount = atof(argv[++i]);
		}
		else if (!strcmp(a, "--wb") && i+3 < argc) {
			wb[0] = atof(argv[++i]);
			wb[1] = atof(argv[++i]);
			wb[2] = atof(argv[++i]);
			manual_wb = true;
		}
		else { fputs(USAGE, stderr); return 1; }
	}
	/* --camera preset: fill in device, subdev, lens, dimensions, and
	 * sensor-specific WB only where the user did not pass an explicit
	 * value. Verified subdev numbers for the FP3+ stock kernel — if
	 * the kernel build changes their order, override with --device etc. */
	if (camera_preset) {
		/* Configure the pipeline first, then take the geometry, Bayer
		 * order and subdev nodes from what fp3-cam-setup actually
		 * found. The camera modules are user-replaceable and the two
		 * phone generations fit different sensors in the same slot, so
		 * none of this can be a constant: the rear slot is a 4032x3024
		 * RGGB IMX363 on one phone and a 4000x3000 GRBG S5KGM1SP on the
		 * other, and even /dev/v4l-subdevN shifts between them. */
		char setup_cmd[128];
		snprintf(setup_cmd, sizeof(setup_cmd),
			 "/usr/bin/fp3-cam-setup %s 1>&2", camera_preset);
		int setup_rc = system(setup_cmd);
		if (setup_rc != 0)
			fprintf(stderr, "cam-snap: fp3-cam-setup returned %d (continuing)\n",
				setup_rc);
		load_cam_conf(camera_preset, &dev, &subdev, &lens,
			      &width, &height, &bp, &bayer_explicit);

		if (!strcmp(camera_preset, "rear")) {
			if (!dev)    dev    = "/dev/video0";
			if (!subdev) subdev = "/dev/v4l-subdev16";
			if (!lens)   lens   = "/dev/v4l-subdev17";
			if (!width)  width  = 4000;
			if (!height) height = 3000;
			/* rear WB defaults already match (2.1/1.0/1.5) */
		} else if (!strcmp(camera_preset, "front")) {
			if (!dev)    dev    = "/dev/video1";
			if (!subdev) subdev = "/dev/v4l-subdev18";
			if (!width)  width  = 4608;
			if (!height) height = 3456;
			/* Front s5k3p9sp wants less R-gain, more B-gain than the
			 * rear s5kgm1sp. Only override if user didn't pass --wb. */
			if (wb[0] == 2.1f && wb[1] == 1.0f && wb[2] == 1.5f) {
				wb[0] = 1.75f; wb[1] = 1.0f; wb[2] = 2.15f;
			}
		} else {
			fprintf(stderr, "--camera: expected 'rear' or 'front', got '%s'\n",
				camera_preset);
			return 1;
		}
	}

	if (!dev || !out || !width || !height) { fputs(USAGE, stderr); return 1; }
	if (width % 4 != 0) die("width must be a multiple of 4");

	signal(SIGPIPE, SIG_IGN);
	build_gamma_lut(gamma_val, contrast);
	if (lsc_amount < 0)
		lsc_amount = (lens != NULL) ? 0.4f : 0.0f;

	if (subdev && (exposure >= 0 || gain >= 0)) {
		int sfd = open(subdev, O_RDWR);
		if (sfd < 0)
			fprintf(stderr, "warn: open %s: %s\n", subdev, strerror(errno));
		else {
			if (exposure >= 0) set_ctrl(sfd, V4L2_CID_EXPOSURE, exposure, "exposure");
			if (gain >= 0) set_ctrl(sfd, V4L2_CID_ANALOGUE_GAIN, gain, "gain");
			close(sfd);
		}
	}

	/* Drive the lens VCM (ak7375 on rear, /dev/v4l-subdev17). Without
	 * this the lens sits at its rest position — typically focused
	 * around 50cm — and everything beyond is soft. Range 0..1023:
	 * 0 = infinity, 1023 = macro. Tune per-distance for now; a
	 * proper contrast-detect AF loop would beat this. */
	if (lens && focus >= 0) {
		int lfd = open(lens, O_RDWR);
		if (lfd < 0)
			fprintf(stderr, "warn: open lens %s: %s\n", lens, strerror(errno));
		else {
			set_ctrl(lfd, V4L2_CID_FOCUS_ABSOLUTE, focus, "focus");
			close(lfd);
			/* Give the VCM ~50ms to settle before we start
			 * capturing — VCMs are mechanical. */
			usleep(50 * 1000);
		}
	}

	if (avg_frames < 1) avg_frames = 1;
	if (avg_frames > MAX_AVG_FRAMES) avg_frames = MAX_AVG_FRAMES;

	int fd = open(dev, O_RDWR | O_NONBLOCK);
	if (fd < 0) die("open %s: %s", dev, strerror(errno));

	struct v4l2_format fmt = {0};
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = width;
	fmt.fmt.pix_mp.height = height;
	fmt.fmt.pix_mp.pixelformat = bayer_to_v4l2(bp);
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) die("VIDIOC_S_FMT: %s", strerror(errno));
	unsigned int bpl = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;

	struct v4l2_requestbuffers req = {
		.count = NUM_BUFFERS,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
	};
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) die("VIDIOC_REQBUFS: %s", strerror(errno));

	struct { void *p; size_t len; } bufs[NUM_BUFFERS];
	for (unsigned int i = 0; i < req.count; i++) {
		struct v4l2_plane pl[1] = {0};
		struct v4l2_buffer b = {.type = req.type, .memory = req.memory,
					.index = i, .length = 1, .m.planes = pl};
		if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) die("QUERYBUF[%u]", i);
		bufs[i].len = pl[0].length;
		bufs[i].p = mmap(NULL, pl[0].length, PROT_READ|PROT_WRITE,
				 MAP_SHARED, fd, pl[0].m.mem_offset);
		if (bufs[i].p == MAP_FAILED) die("mmap[%u]", i);
		if (xioctl(fd, VIDIOC_QBUF, &b) < 0) die("QBUF[%u]", i);
	}
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) die("STREAMON: %s", strerror(errno));

	uint16_t *bayer = malloc((size_t)width * height * sizeof(*bayer));
	if (!bayer) die("malloc bayer");
	/* Accumulator for multi-frame averaging: 32-bit so 9 × 10-bit
	 * values can't overflow (max sum < 9 * 1023 < 2^14, well under). */
	uint32_t *accum = NULL;
	uint16_t *tmp = NULL;
	if (avg_frames > 1) {
		accum = calloc((size_t)width * height, sizeof(*accum));
		tmp = malloc((size_t)width * sizeof(*tmp));
		if (!accum || !tmp) die("malloc accum");
	}

	/*
	 * Contrast-detect AF — sweep the lens, capture a probe frame at
	 * each step, score sharpness with the Laplacian-squared metric,
	 * then leave the lens at the best position. Coarse 7-step sweep
	 * (~50ms each → ~350ms total) covers infinity → ~20cm range. We
	 * dequeue and immediately requeue probe buffers, so the streaming
	 * state stays continuous for the main capture that follows.
	 */
	if (do_autofocus && lens) {
		int lfd = open(lens, O_RDWR);
		if (lfd < 0) {
			fprintf(stderr, "warn: open lens %s: %s\n", lens, strerror(errno));
		} else {
			/* Dense linear sweep with step ≈ 64. The DW9800W focus DoF
			 * window is narrow (~50 units), so the original 7-probe sweep
			 * straddled but missed the peak entirely. 16 probes ≈ 1 s at
			 * ~60 ms/probe. */
			static const int probe_positions[] = {
				0, 64, 128, 192, 256, 320, 384, 448,
				512, 576, 640, 704, 768, 832, 896, 1023,
			};
			const int n_probes = sizeof(probe_positions) / sizeof(*probe_positions);
			double best_score = -1.0;
			int best_pos = probe_positions[0];
			uint16_t *probe = malloc((size_t)width * height * sizeof(*probe));
			if (!probe) die("malloc probe");

			for (int p = 0; p < n_probes; p++) {
				int fpos = probe_positions[p];
				set_ctrl(lfd, V4L2_CID_FOCUS_ABSOLUTE, fpos, "focus");
				/* Drop one frame after the lens moves (it
				 * may still be settling) then evaluate the
				 * next. */
				for (int drop = 0; drop < 2; drop++) {
					struct pollfd pfd = { .fd = fd, .events = POLLIN };
					int pr;
					do { pr = poll(&pfd, 1, 5000); }
					while (pr == -1 && errno == EINTR);
					if (pr <= 0) die("AF poll: %s",
							  pr ? strerror(errno) : "timeout");
					struct v4l2_plane pl[1] = {0};
					struct v4l2_buffer b = {.type = type,
								.memory = V4L2_MEMORY_MMAP,
								.length = 1, .m.planes = pl};
					if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
						if (errno == EAGAIN) { drop--; continue; }
						die("AF DQBUF: %s", strerror(errno));
					}
					if (drop == 1) {
						const uint8_t *base = bufs[b.index].p;
						for (unsigned int y = 0; y < height; y++)
							unpack_row(base + y * bpl,
								   probe + y * width, width);
						double s = focus_metric(probe, width, height);
						fprintf(stderr, "AF probe focus=%4d score=%.3e\n",
							fpos, s);
						if (s > best_score) {
							best_score = s;
							best_pos = fpos;
						}
					}
					if (xioctl(fd, VIDIOC_QBUF, &b) < 0)
						die("AF QBUF (recycle)");
				}
			}
			free(probe);
			fprintf(stderr, "AF picked focus=%d\n", best_pos);
			set_ctrl(lfd, V4L2_CID_FOCUS_ABSOLUTE, best_pos, "focus");
			close(lfd);
			usleep(50 * 1000);
		}
	}

	for (unsigned int f = 0; f < WARMUP_FRAMES + (unsigned)avg_frames; f++) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr;
		do { pr = poll(&pfd, 1, 5000); } while (pr == -1 && errno == EINTR);
		if (pr <= 0) die("poll: %s", pr ? strerror(errno) : "timeout");
		struct v4l2_plane pl[1] = {0};
		struct v4l2_buffer b = {.type = type, .memory = V4L2_MEMORY_MMAP,
					.length = 1, .m.planes = pl};
		if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
			if (errno == EAGAIN) { f--; continue; }
			die("DQBUF: %s", strerror(errno));
		}
		if (f >= WARMUP_FRAMES) {
			const uint8_t *base = bufs[b.index].p;
			if (avg_frames == 1) {
				for (unsigned int y = 0; y < height; y++)
					unpack_row(base + y * bpl,
						   bayer + y * width, width);
			} else {
				for (unsigned int y = 0; y < height; y++) {
					unpack_row(base + y * bpl, tmp, width);
					uint32_t *arow = accum + y * width;
					for (unsigned int x = 0; x < width; x++)
						arow[x] += tmp[x];
				}
			}
		}
		if (xioctl(fd, VIDIOC_QBUF, &b) < 0) die("QBUF (recycle)");
	}
	if (avg_frames > 1) {
		for (size_t i = 0; i < (size_t)width * height; i++)
			bayer[i] = (uint16_t)(accum[i] / (uint32_t)avg_frames);
		free(accum);
		free(tmp);
		fprintf(stderr, "averaged %d frames\n", avg_frames);
	}
	xioctl(fd, VIDIOC_STREAMOFF, &type);
	for (unsigned int i = 0; i < req.count; i++) munmap(bufs[i].p, bufs[i].len);
	close(fd);

	/* --- Diagnostic dump --- */
	struct quad_means qm;
	compute_quad_means(bayer, width, height, &qm);
	fprintf(stderr, "subpixel means (raw 10-bit):\n");
	fprintf(stderr, "  (0,0)=%7.1f  (0,1)=%7.1f\n",
		qm.mean[0][0], qm.mean[0][1]);
	fprintf(stderr, "  (1,0)=%7.1f  (1,1)=%7.1f\n",
		qm.mean[1][0], qm.mean[1][1]);

	double mr, mg, mb;
	quad_to_rgb(bp, &qm, &mr, &mg, &mb);
	fprintf(stderr, "with bayer=%s: R=%.1f G=%.1f B=%.1f (G/R=%.2f G/B=%.2f)\n",
		bp == BP_GRBG ? "grbg" : bp == BP_RGGB ? "rggb" :
		bp == BP_BGGR ? "bggr" : "gbrg",
		mr, mg, mb,
		mr > 0 ? mg / mr : 0, mb > 0 ? mg / mb : 0);

	/* --- White balance gains --- */
	float gr, gg = 1.0f, gb;
	if (manual_wb) {
		gr = wb[0]; gg = wb[1]; gb = wb[2];
	} else if (do_awb) {
		/* Gray-world from frame statistics, with a softer 3.0 cap. */
		gr = (mr > 4) ? (float)(mg / mr) : 1.0f;
		gb = (mb > 4) ? (float)(mg / mb) : 1.0f;
		/* Warm bias on R: indoor LEDs/fluorescents push the scene
		 * slightly green; pure gray-world balances to that lighting
		 * (so it makes the image look as green as the room actually
		 * is). A 5–10% red boost shifts perceived white closer to
		 * daylight. */
		gr *= warm_bias;
		if (gr > 3.0f) gr = 3.0f;
		if (gb > 3.0f) gb = 3.0f;
		if (gr < 0.4f) gr = 0.4f;
		if (gb < 0.4f) gb = 0.4f;
	} else {
		gr = warm_bias; gb = 1.0f;
	}
	fprintf(stderr, "wb gains: R=%.3f G=%.3f B=%.3f (warm_bias=%.2f, lsc=%.2f)\n",
		gr, gg, gb, warm_bias, lsc_amount);

	apply_bls_and_gain(bayer, width, height, bp, gr, gg, gb, lsc_amount);

	/* --- Demosaic --- */
	uint8_t *rgbx = malloc((size_t)width * height * 4);
	if (!rgbx) die("malloc rgbx");
	if (use_mhc && bp == BP_GRBG) {
		demosaic_mhc_grbg(bayer, rgbx, width, height);
		fprintf(stderr, "demosaic: MHC\n");
	} else {
		struct image_in in = {
			.info = { .bpp = 10, .w = width, .h = height,
				  .stride = width * 2, .endian = BAYER_E_LITTLE },
			.data = bayer,
			.type = bayer_to_lib(bp),
		};
		struct image_out o = {
			.info = { .bpp = 8, .w = width, .h = height,
				  .stride = width * 4, .endian = BAYER_E_LITTLE },
			.data = rgbx,
			.type = RGB_FMT_RGBx,
			.quality = QUALITY_ROUND_2,
		};
		struct image_conversion_info ci = {0};
		bayer2rgb_convert(&in, &o, &ci);
		fprintf(stderr, "demosaic: bayer2rgb-neon ROUND_2\n");
	}
	free(bayer);

	if (denoise_strength > 0) {
		fprintf(stderr, "denoise: bilateral 5x5 strength=%d\n",
			denoise_strength);
		bilateral_5x5(rgbx, width, height, denoise_strength);
	}

	if (do_sharpen) {
		/* Auto-strength: if the user didn't pass --sharpen-amount and
		 * we ran the bilateral denoise, bump the amount to recover the
		 * mid-frequency content the denoise smoothed out. */
		float amt = sharpen_amount;
		if (!sharpen_explicit) {
			/* Calibrated against android-ref2 center-crop Laplacian
			 * variance ≈ 288. MHC alone → ~150, ×0.4 unsharp ≈ 280. */
			if (denoise_strength >= 8)      amt = 0.75f;
			else if (denoise_strength >= 4) amt = 0.55f;
			else                            amt = 0.40f;
		}
		fprintf(stderr, "sharpen: 5x5 unsharp amount=%.2f\n", amt);
		unsharp_mask(rgbx, width, height, amt);
	}

	rgbx_postprocess(rgbx, width, height, CCM_SRGB, do_ccm, exp_boost, saturation, use_phone_curve);
	if (auto_levels)
		apply_auto_levels(rgbx, width, height, auto_levels_lo, auto_levels_hi);
	write_jpeg(out, rgbx, width, height, quality);
	free(rgbx);
	fprintf(stderr, "wrote %s (%ux%u, q=%d)\n", out, width, height, quality);
	return 0;
}

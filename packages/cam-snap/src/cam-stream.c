// SPDX-License-Identifier: MIT
/*
 * cam-stream: real-time H.264 video from FP3+ rear or front camera.
 *
 * Pipeline:
 *   V4L2 bayer capture (/dev/video0|1, pgAA 10-bit packed)
 *     → bayer → NV12 (1920x1080, integer 2x downscale + center crop + WB)
 *     → Venus H.264 m2m encoder (/dev/video7)
 *     → raw H.264 bytestream to stdout
 *
 * Consume on dev host:
 *   ssh nerves.local 'cam-stream --camera front' | ffplay -f h264 -framerate 30 -
 *
 * v1 is scalar — first goal is to prove the pipeline. NEON vectorisation
 * for bayer→NV12 is a follow-up.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/videodev2.h>
#include <math.h>
#include <arm_neon.h>
#include "cam_pipeline.h"

/* Output dimensions are fixed; bayer dimensions come from cam_pipeline.h.
 * 2× downscale + center-crop produces a 1920×1080 NV12 frame from either
 * front (4608×3456 → 2304×1728 crop 192/324) or rear (4000×3000 → 2000×
 * 1500 crop 40/210). */
#define OUT_W            1920
#define OUT_H            1080

#define FRONT_BAYER_W    CAM_BAYER_W_FRONT
#define FRONT_BAYER_H    CAM_BAYER_H_FRONT
#define FRONT_CROP_X     ((FRONT_BAYER_W/2 - OUT_W) / 2)
#define FRONT_CROP_Y     ((FRONT_BAYER_H/2 - OUT_H) / 2)

#define REAR_BAYER_W     CAM_BAYER_W_REAR
#define REAR_BAYER_H     CAM_BAYER_H_REAR
#define REAR_CROP_X      ((REAR_BAYER_W/2 - OUT_W) / 2)
#define REAR_CROP_Y      ((REAR_BAYER_H/2 - OUT_H) / 2)

#define CAP_BUFS     4
#define ENC_OUT_BUFS 4
#define ENC_CAP_BUFS 6

#define die(...) do { fprintf(stderr, "cam-stream: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
	return r;
}

struct buf {
	void  *p;
	size_t len;
};

static volatile int running = 1;
static void on_sigint(int s) { (void)s; running = 0; }

/* ----------------------------------------------------------------------
 * V4L2 capture (sensor side) — MPLANE single-plane bayer pgAA
 * --------------------------------------------------------------------*/

static int cap_open_and_setup(const char *dev, const char *subdev,
			      int width, int height,
			      int exposure, int gain,
			      struct buf bufs[CAP_BUFS])
{
	int fd = open(dev, O_RDWR | O_NONBLOCK);
	if (fd < 0) die("open %s: %s", dev, strerror(errno));

	if (subdev) {
		int sfd = open(subdev, O_RDWR);
		if (sfd >= 0) {
			struct v4l2_control c;
			if (exposure >= 0) {
				c = (struct v4l2_control){ .id = V4L2_CID_EXPOSURE, .value = exposure };
				xioctl(sfd, VIDIOC_S_CTRL, &c);
			}
			if (gain >= 0) {
				c = (struct v4l2_control){ .id = V4L2_CID_ANALOGUE_GAIN, .value = gain };
				xioctl(sfd, VIDIOC_S_CTRL, &c);
			}
			close(sfd);
		}
	}

	struct v4l2_format fmt = {0};
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = width;
	fmt.fmt.pix_mp.height = height;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_SGRBG10P;  /* 'pgAA' */
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
		die("cap S_FMT: %s", strerror(errno));

	struct v4l2_requestbuffers req = {0};
	req.count = CAP_BUFS;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0)
		die("cap REQBUFS: %s", strerror(errno));
	if (req.count < CAP_BUFS)
		die("cap REQBUFS only got %u buffers", req.count);

	for (int i = 0; i < CAP_BUFS; i++) {
		struct v4l2_plane pl = {0};
		struct v4l2_buffer b = {0};
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.length = 1;
		b.m.planes = &pl;
		if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0)
			die("cap QUERYBUF[%d]: %s", i, strerror(errno));
		bufs[i].len = pl.length;
		bufs[i].p = mmap(NULL, pl.length, PROT_READ | PROT_WRITE,
				 MAP_SHARED, fd, pl.m.mem_offset);
		if (bufs[i].p == MAP_FAILED)
			die("cap mmap[%d]: %s", i, strerror(errno));

		if (xioctl(fd, VIDIOC_QBUF, &b) < 0)
			die("cap QBUF[%d]: %s", i, strerror(errno));
	}

	int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_STREAMON, &t) < 0)
		die("cap STREAMON: %s", strerror(errno));
	return fd;
}

/* ----------------------------------------------------------------------
 * V4L2 m2m encoder (Venus)
 * --------------------------------------------------------------------*/

static int enc_setup(struct buf outs[ENC_OUT_BUFS],
		     struct buf caps[ENC_CAP_BUFS],
		     int bitrate, int target_fps)
{
	const char *dev = "/dev/video7";
	int fd = open(dev, O_RDWR | O_NONBLOCK);
	if (fd < 0) die("open %s: %s", dev, strerror(errno));

	/* OUTPUT (input frames): NV12 1920x1080 */
	struct v4l2_format fmt = {0};
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	fmt.fmt.pix_mp.width = OUT_W;
	fmt.fmt.pix_mp.height = OUT_H;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
		die("enc OUTPUT S_FMT: %s", strerror(errno));

	/* CAPTURE (encoded bitstream): H.264 */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = OUT_W;
	fmt.fmt.pix_mp.height = OUT_H;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
		die("enc CAPTURE S_FMT: %s", strerror(errno));

	/* Encode rate controls */
	struct v4l2_control c;
	c.id = V4L2_CID_MPEG_VIDEO_BITRATE; c.value = bitrate;
	xioctl(fd, VIDIOC_S_CTRL, &c);
	/* Small GOP so the receiver can lock on quickly (smaller key-frame
	 * period = lower start-up latency at the cost of slightly more bandwidth). */
	c.id = V4L2_CID_MPEG_VIDEO_GOP_SIZE; c.value = 15;
	xioctl(fd, VIDIOC_S_CTRL, &c);
	c.id = V4L2_CID_MPEG_VIDEO_H264_PROFILE;
	c.value = V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
	xioctl(fd, VIDIOC_S_CTRL, &c);
	c.id = V4L2_CID_MPEG_VIDEO_H264_I_PERIOD; c.value = 15;
	xioctl(fd, VIDIOC_S_CTRL, &c);
	c.id = V4L2_CID_MPEG_VIDEO_B_FRAMES; c.value = 0;
	xioctl(fd, VIDIOC_S_CTRL, &c);
	/* Prepend SPS/PPS to each I-frame so consumers can start mid-stream */
	c.id = V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER; c.value = 1;
	xioctl(fd, VIDIOC_S_CTRL, &c);

	/* Tell the encoder the frame rate so its rate control gets it right */
	struct v4l2_streamparm sp = {0};
	sp.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	sp.parm.output.timeperframe.numerator = 1;
	sp.parm.output.timeperframe.denominator = target_fps;
	xioctl(fd, VIDIOC_S_PARM, &sp);

	/* OUTPUT (NV12) buffers — we fill these */
	struct v4l2_requestbuffers req = {0};
	req.count = ENC_OUT_BUFS;
	req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0)
		die("enc OUTPUT REQBUFS: %s", strerror(errno));

	for (int i = 0; i < ENC_OUT_BUFS; i++) {
		struct v4l2_plane pl = {0};
		struct v4l2_buffer b = {0};
		b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.length = 1;
		b.m.planes = &pl;
		if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0)
			die("enc OUTPUT QUERYBUF[%d]: %s", i, strerror(errno));
		outs[i].len = pl.length;
		outs[i].p = mmap(NULL, pl.length, PROT_READ | PROT_WRITE,
				 MAP_SHARED, fd, pl.m.mem_offset);
		if (outs[i].p == MAP_FAILED)
			die("enc OUTPUT mmap[%d]: %s", i, strerror(errno));
	}

	/* CAPTURE (H.264) buffers — encoder fills, we drain */
	memset(&req, 0, sizeof(req));
	req.count = ENC_CAP_BUFS;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0)
		die("enc CAPTURE REQBUFS: %s", strerror(errno));

	for (int i = 0; i < ENC_CAP_BUFS; i++) {
		struct v4l2_plane pl = {0};
		struct v4l2_buffer b = {0};
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.length = 1;
		b.m.planes = &pl;
		if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0)
			die("enc CAPTURE QUERYBUF[%d]: %s", i, strerror(errno));
		caps[i].len = pl.length;
		caps[i].p = mmap(NULL, pl.length, PROT_READ | PROT_WRITE,
				 MAP_SHARED, fd, pl.m.mem_offset);
		if (caps[i].p == MAP_FAILED)
			die("enc CAPTURE mmap[%d]: %s", i, strerror(errno));

		if (xioctl(fd, VIDIOC_QBUF, &b) < 0)
			die("enc CAPTURE QBUF[%d]: %s", i, strerror(errno));
	}

	int t;
	t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (xioctl(fd, VIDIOC_STREAMON, &t) < 0)
		die("enc OUTPUT STREAMON: %s", strerror(errno));
	t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_STREAMON, &t) < 0)
		die("enc CAPTURE STREAMON: %s", strerror(errno));
	return fd;
}

/* ----------------------------------------------------------------------
 * Bayer → NV12 (scalar v1)
 *
 * Input:  4608x3456 GRBG 10-bit packed (pgAA): 5 bytes per 4 pixels.
 *   bytes [hi0][hi1][hi2][hi3][low_bits] where pixel_i = (hi[i]<<2)|((low>>i*2)&3)
 * Output: 1920x1080 NV12 (Y plane then interleaved UV plane).
 *
 * Algorithm:
 *   1. 2× downscale: each output pixel = one 2x2 GRBG cell
 *      → intermediate 2304x1728 RGB (conceptually; computed inline)
 *   2. Center-crop to 1920x1080 (cut 192 left/right, 324 top/bottom)
 *   3. WB gains: R *= wb_r/256, B *= wb_b/256
 *   4. RGB → YUV (BT.601):
 *        Y = ( 77*R + 150*G +  29*B + 128) >> 8
 *        U = (-43*R -  85*G + 128*B + 128*256) >> 8
 *        V = (128*R - 107*G -  21*B + 128*256) >> 8
 *   5. UV is averaged across each 2x2 RGB output block (NV12 chroma is
 *      half-res). We compute one UV pair per output 2x2 block.
 * --------------------------------------------------------------------*/

static inline uint8_t clamp_u8(int v)
{
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (uint8_t)v;
}

/* 8-bit→8-bit tone LUT built from the shared formula in cam_pipeline.h
 * — cam-snap's 1024-entry 10-bit variant maps to the same curve, so the
 * two binaries produce visually-equivalent output for the same gamma/
 * contrast parameters. Applied to R, G, B in linear-RGB before RGB→YUV
 * (not on YUV-Y, which would over-lift mid-shadows). */
static uint8_t gamma_lut[256];

/* Apply gamma_lut to 8 packed bytes (uint8x8_t) — scalar inner loop,
 * fast enough since the LUT fits in L1 (256B). Used per R/G/B vector. */
static inline uint8x8_t apply_lut_x8(uint8x8_t v, const uint8_t *lut)
{
	uint8_t tmp[8] __attribute__((aligned(8)));
	vst1_u8(tmp, v);
	tmp[0] = lut[tmp[0]]; tmp[1] = lut[tmp[1]];
	tmp[2] = lut[tmp[2]]; tmp[3] = lut[tmp[3]];
	tmp[4] = lut[tmp[4]]; tmp[5] = lut[tmp[5]];
	tmp[6] = lut[tmp[6]]; tmp[7] = lut[tmp[7]];
	return vld1_u8(tmp);
}

/* RGB chroma saturation in linear-RGB:
 *   out = clamp_u8(Y + (ch - Y) * sat_q8 / 256)
 * with Y = 0.299R + 0.587G + 0.114B (Rec.601). Matches cam-snap's order
 * — saturation BEFORE gamma — so the two binaries produce visually-
 * equivalent output. Pulls the 16-bit signed result through a 32-bit
 * multiply so `sat_q8` > 256 (saturation > 1.0) doesn't overflow. */
static inline uint8x8_t saturate_channel(uint8x8_t ch, uint8x8_t y, int sat_q8)
{
	int16x8_t ch16 = vreinterpretq_s16_u16(vmovl_u8(ch));
	int16x8_t y16  = vreinterpretq_s16_u16(vmovl_u8(y));
	int16x8_t diff = vsubq_s16(ch16, y16);
	int32x4_t lo = vmull_n_s16(vget_low_s16(diff),  (int16_t)sat_q8);
	int32x4_t hi = vmull_n_s16(vget_high_s16(diff), (int16_t)sat_q8);
	int16x4_t lo16 = vqshrn_n_s32(lo, 8);
	int16x4_t hi16 = vqshrn_n_s32(hi, 8);
	int16x8_t scaled = vcombine_s16(lo16, hi16);
	int16x8_t out = vaddq_s16(y16, scaled);
	return vqmovun_s16(out);
}

/* NEON Q8 multiply-saturate-narrow: out[i] = clamp_u8((v[i] * m) >> 8) */
static inline uint8x8_t neon_mul_q8_sat(uint8x8_t v, uint16_t m)
{
	uint16x8_t v16 = vmovl_u8(v);
	uint32x4_t lo32 = vmull_n_u16(vget_low_u16(v16), m);
	uint32x4_t hi32 = vmull_n_u16(vget_high_u16(v16), m);
	uint16x4_t lo16 = vqshrn_n_u32(lo32, 8);
	uint16x4_t hi16 = vqshrn_n_u32(hi32, 8);
	return vqmovn_u16(vcombine_u16(lo16, hi16));
}

/* NEON bayer→NV12 with 2× downscale + center crop + WB + chroma saturation.
 * `sat_q8` is the YUV chroma multiplier in Q8.8 (256=neutral, 384=1.5× pop). */
static void bayer_to_nv12(const uint8_t *bayer_packed, uint8_t *nv12,
			  int bayer_w, int crop_x, int crop_y,
			  int wb_r_q8, int wb_b_q8, int sat_q8)
{
	uint8_t *yp  = nv12;
	uint8_t *uvp = nv12 + OUT_W * OUT_H;

	/* Map a 20-byte pgAA span (4 groups × 5 bytes each, with byte-4 of
	 * every group being the low-bit byte we drop) onto a contiguous
	 * 16-byte vector of bayer pixels. Combined input vectors are:
	 *   lo16 = bytes 0..15 (loaded with vld1q_u8(p))
	 *   hi16 = bytes 4..19 (loaded with vld1q_u8(p+4))
	 * vqtbl2q index 0..15 picks from lo16; 16..31 picks from hi16
	 * (where hi16[n] = src(4+n)). */
	static const uint8_t PGAA_IDX_BYTES[16] = {
		 0,  1,  2,  3,
		 5,  6,  7,  8,
		10, 11, 12, 13,
		15, 28, 29, 30,
	};
	uint8x16_t PGAA_IDX = vld1q_u8(PGAA_IDX_BYTES);

	const int row_stride = bayer_w * 5 / 4;
	const uint16_t wbr = (uint16_t)wb_r_q8;
	const uint16_t wbb = (uint16_t)wb_b_q8;

	for (int oy = 0; oy < OUT_H; oy += 2) {
		int mid_y0 = oy + crop_y;
		int mid_y1 = mid_y0 + 1;
		const uint8_t *t0 = bayer_packed + (size_t)(mid_y0 * 2)     * row_stride;
		const uint8_t *b0 = bayer_packed + (size_t)(mid_y0 * 2 + 1) * row_stride;
		const uint8_t *t1 = bayer_packed + (size_t)(mid_y1 * 2)     * row_stride;
		const uint8_t *b1 = bayer_packed + (size_t)(mid_y1 * 2 + 1) * row_stride;

		uint8_t *y0_row = yp + (size_t)oy       * OUT_W;
		uint8_t *y1_row = yp + (size_t)(oy + 1) * OUT_W;
		uint8_t *uv_row = uvp + (size_t)(oy / 2) * OUT_W;

		for (int ox = 0; ox < OUT_W; ox += 8) {
			int start_g = (ox + crop_x) / 2;
			const uint8_t *p_t0 = t0 + (size_t)start_g * 5;
			const uint8_t *p_b0 = b0 + (size_t)start_g * 5;
			const uint8_t *p_t1 = t1 + (size_t)start_g * 5;
			const uint8_t *p_b1 = b1 + (size_t)start_g * 5;

			/* Top row of 2×2 cell (GRGR…) for output row oy */
			uint8x16x2_t in_t0 = {{ vld1q_u8(p_t0), vld1q_u8(p_t0 + 4) }};
			uint8x16_t pix_t0 = vqtbl2q_u8(in_t0, PGAA_IDX);
			uint8x8x2_t dt0 = vuzp_u8(vget_low_u8(pix_t0), vget_high_u8(pix_t0));
			uint8x8_t G_t0 = dt0.val[0], R_0 = dt0.val[1];

			/* Bot row of 2×2 cell (BGBG…) for output row oy */
			uint8x16x2_t in_b0 = {{ vld1q_u8(p_b0), vld1q_u8(p_b0 + 4) }};
			uint8x16_t pix_b0 = vqtbl2q_u8(in_b0, PGAA_IDX);
			uint8x8x2_t db0 = vuzp_u8(vget_low_u8(pix_b0), vget_high_u8(pix_b0));
			uint8x8_t B_0 = db0.val[0], G_b0 = db0.val[1];

			/* Same for output row oy+1 */
			uint8x16x2_t in_t1 = {{ vld1q_u8(p_t1), vld1q_u8(p_t1 + 4) }};
			uint8x16_t pix_t1 = vqtbl2q_u8(in_t1, PGAA_IDX);
			uint8x8x2_t dt1 = vuzp_u8(vget_low_u8(pix_t1), vget_high_u8(pix_t1));
			uint8x8_t G_t1 = dt1.val[0], R_1 = dt1.val[1];

			uint8x16x2_t in_b1 = {{ vld1q_u8(p_b1), vld1q_u8(p_b1 + 4) }};
			uint8x16_t pix_b1 = vqtbl2q_u8(in_b1, PGAA_IDX);
			uint8x8x2_t db1 = vuzp_u8(vget_low_u8(pix_b1), vget_high_u8(pix_b1));
			uint8x8_t B_1 = db1.val[0], G_b1 = db1.val[1];

			/* G_avg = (G_top + G_bot + 1) / 2 — rounding halving */
			uint8x8_t G0 = vrhadd_u8(G_t0, G_b0);
			uint8x8_t G1 = vrhadd_u8(G_t1, G_b1);

			/* White-balance R and B (Q8.8 multiply, saturating narrow) */
			uint8x8_t R0_wb = neon_mul_q8_sat(R_0, wbr);
			uint8x8_t B0_wb = neon_mul_q8_sat(B_0, wbb);
			uint8x8_t R1_wb = neon_mul_q8_sat(R_1, wbr);
			uint8x8_t B1_wb = neon_mul_q8_sat(B_1, wbb);

			/* SATURATION in linear-RGB BEFORE gamma — matches
			 * cam-snap. Compute per-row Y (Rec.601) then pull
			 * each channel toward/away from Y by sat_q8/256. */
			uint16x8_t y0_lin_acc = vdupq_n_u16(0);
			y0_lin_acc = vmlaq_n_u16(y0_lin_acc, vmovl_u8(R0_wb), 77);
			y0_lin_acc = vmlaq_n_u16(y0_lin_acc, vmovl_u8(G0),    150);
			y0_lin_acc = vmlaq_n_u16(y0_lin_acc, vmovl_u8(B0_wb), 29);
			uint8x8_t Y0_lin = vshrn_n_u16(y0_lin_acc, 8);

			uint16x8_t y1_lin_acc = vdupq_n_u16(0);
			y1_lin_acc = vmlaq_n_u16(y1_lin_acc, vmovl_u8(R1_wb), 77);
			y1_lin_acc = vmlaq_n_u16(y1_lin_acc, vmovl_u8(G1),    150);
			y1_lin_acc = vmlaq_n_u16(y1_lin_acc, vmovl_u8(B1_wb), 29);
			uint8x8_t Y1_lin = vshrn_n_u16(y1_lin_acc, 8);

			R0_wb = saturate_channel(R0_wb, Y0_lin, sat_q8);
			G0    = saturate_channel(G0,    Y0_lin, sat_q8);
			B0_wb = saturate_channel(B0_wb, Y0_lin, sat_q8);
			R1_wb = saturate_channel(R1_wb, Y1_lin, sat_q8);
			G1    = saturate_channel(G1,    Y1_lin, sat_q8);
			B1_wb = saturate_channel(B1_wb, Y1_lin, sat_q8);

			/* GAMMA + S-curve LUT on the saturated R, G, B. Same
			 * lookup table cam-snap applies after its sat step. */
			R0_wb = apply_lut_x8(R0_wb, gamma_lut);
			G0    = apply_lut_x8(G0,    gamma_lut);
			B0_wb = apply_lut_x8(B0_wb, gamma_lut);
			R1_wb = apply_lut_x8(R1_wb, gamma_lut);
			G1    = apply_lut_x8(G1,    gamma_lut);
			B1_wb = apply_lut_x8(B1_wb, gamma_lut);

			/* Y = (77R + 150G + 29B + 128) >> 8 — now on gamma-
			 * encoded R, G, B so the Y values match cam-snap. */
			uint16x8_t y0a = vdupq_n_u16(128);
			y0a = vmlaq_n_u16(y0a, vmovl_u8(R0_wb), 77);
			y0a = vmlaq_n_u16(y0a, vmovl_u8(G0),    150);
			y0a = vmlaq_n_u16(y0a, vmovl_u8(B0_wb), 29);
			vst1_u8(y0_row + ox, vshrn_n_u16(y0a, 8));

			uint16x8_t y1a = vdupq_n_u16(128);
			y1a = vmlaq_n_u16(y1a, vmovl_u8(R1_wb), 77);
			y1a = vmlaq_n_u16(y1a, vmovl_u8(G1),    150);
			y1a = vmlaq_n_u16(y1a, vmovl_u8(B1_wb), 29);
			vst1_u8(y1_row + ox, vshrn_n_u16(y1a, 8));

			/* UV: average 2×2 RGB blocks (horizontal-pair sums then
			 * vertical add, divide by 4). 8 columns × 2 rows → 4 UV
			 * pairs. Work in signed 16-bit since U/V need negation. */
			uint16x4_t R0p = vpaddl_u8(R0_wb);
			uint16x4_t G0p = vpaddl_u8(G0);
			uint16x4_t B0p = vpaddl_u8(B0_wb);
			uint16x4_t R1p = vpaddl_u8(R1_wb);
			uint16x4_t G1p = vpaddl_u8(G1);
			uint16x4_t B1p = vpaddl_u8(B1_wb);

			int16x4_t Ravg = vreinterpret_s16_u16(vshr_n_u16(vadd_u16(R0p, R1p), 2));
			int16x4_t Gavg = vreinterpret_s16_u16(vshr_n_u16(vadd_u16(G0p, G1p), 2));
			int16x4_t Bavg = vreinterpret_s16_u16(vshr_n_u16(vadd_u16(B0p, B1p), 2));

			/* U = ((-43R - 85G + 128B) >> 8) + 128 */
			int16x4_t Uacc = vmul_n_s16(Ravg, -43);
			Uacc = vmla_n_s16(Uacc, Gavg, -85);
			Uacc = vmla_n_s16(Uacc, Bavg, 128);
			int16x4_t Uval = vadd_s16(vshr_n_s16(Uacc, 8), vdup_n_s16(128));

			/* V = ((128R - 107G - 21B) >> 8) + 128 — chroma sat
			 * was already applied in linear-RGB above, so we
			 * skip the YUV-chroma stretch that earlier versions
			 * did here. */
			int16x4_t Vacc = vmul_n_s16(Ravg, 128);
			Vacc = vmla_n_s16(Vacc, Gavg, -107);
			Vacc = vmla_n_s16(Vacc, Bavg, -21);
			int16x4_t Vval = vadd_s16(vshr_n_s16(Vacc, 8), vdup_n_s16(128));

			/* Saturate to u8 and interleave U,V → 8 bytes */
			uint8x8_t U8 = vqmovun_s16(vcombine_s16(Uval, vdup_n_s16(0)));
			uint8x8_t V8 = vqmovun_s16(vcombine_s16(Vval, vdup_n_s16(0)));
			uint8x8x2_t uvz = vzip_u8(U8, V8);
			vst1_u8(uv_row + ox, uvz.val[0]);
		}
	}
}

/* ----------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------*/

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

/* Laplacian-squared focus metric on the green sub-grid of a raw pgAA frame.
 * Subsamples a 1024×768 center crop (every 2nd green pixel on the G_r
 * sub-grid) so a single probe is ~1 ms — fast enough to sweep 16 lens
 * positions in under a second. */
static double af_score_pgAA(const uint8_t *bayer_packed, int bayer_w, int bayer_h)
{
	const int row_stride = bayer_w * 5 / 4;
	int cw = bayer_w > 1024 ? 1024 : bayer_w;
	int ch = bayer_h > 768  ? 768  : bayer_h;
	int x0 = (bayer_w - cw) / 2; if (x0 & 1) x0--;
	int y0 = (bayer_h - ch) / 2; if (y0 & 1) y0--;
	double acc = 0.0;
	/* Walk on G_r grid (even y, even x). Each pixel comes from pgAA byte
	 * at row_offset = (x/4)*5 + (x%4). For x divisible by 4 this is
	 * simply row_offset = x*5/4; we only sample even x at stride 4 to
	 * stay on G pixels with consistent byte offset. */
	for (int y = y0 + 4; y < y0 + ch - 4; y += 4) {
		const uint8_t *r0  = bayer_packed +  y      * row_stride;
		const uint8_t *r_m = bayer_packed + (y - 4) * row_stride;
		const uint8_t *r_p = bayer_packed + (y + 4) * row_stride;
		for (int x = x0 + 4; x + 4 < x0 + cw; x += 4) {
			/* G at (x, y), (x-4, y), (x+4, y), (x, y-4), (x, y+4) */
			int c  = r0 [(x      / 4) * 5 + (x      % 4)];
			int xL = r0 [((x - 4) / 4) * 5 + ((x - 4) % 4)];
			int xR = r0 [((x + 4) / 4) * 5 + ((x + 4) % 4)];
			int yU = r_m[(x      / 4) * 5 + (x      % 4)];
			int yD = r_p[(x      / 4) * 5 + (x      % 4)];
			int lap = 4 * c - xL - xR - yU - yD;
			acc += (double)lap * lap;
		}
	}
	return acc;
}

/* One-shot contrast-detect autofocus. Sweeps 16 positions, drops a frame
 * after each lens move (settle time), scores the next, picks the best.
 * Drains the cap_fd buffers so the main streaming loop starts clean. */
static int do_autofocus_priv(int cap_fd, struct buf cap_bufs[],
			     int bayer_w, int bayer_h, const char *lens_dev)
{
	int lfd = open(lens_dev, O_RDWR);
	if (lfd < 0) {
		fprintf(stderr, "AF: open %s: %s\n", lens_dev, strerror(errno));
		return -1;
	}
	static const int probes[] = {
		0, 64, 128, 192, 256, 320, 384, 448,
		512, 576, 640, 704, 768, 832, 896, 1023,
	};
	const int n = (int)(sizeof(probes) / sizeof(probes[0]));
	double best_score = -1.0;
	int    best_pos   = probes[0];

	for (int i = 0; i < n; i++) {
		struct v4l2_control c = {
			.id = V4L2_CID_FOCUS_ABSOLUTE, .value = probes[i]
		};
		if (xioctl(lfd, VIDIOC_S_CTRL, &c) < 0)
			continue;

		/* Drop 1 frame for lens settling, score the next */
		for (int drop = 0; drop < 2; drop++) {
			struct pollfd pf = { .fd = cap_fd, .events = POLLIN };
			if (poll(&pf, 1, 1000) <= 0) break;
			struct v4l2_plane pl = {0};
			struct v4l2_buffer cb = {
				.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
				.memory = V4L2_MEMORY_MMAP,
				.length = 1, .m.planes = &pl
			};
			if (xioctl(cap_fd, VIDIOC_DQBUF, &cb) < 0) break;
			if (drop == 1) {
				double s = af_score_pgAA(cap_bufs[cb.index].p,
							 bayer_w, bayer_h);
				fprintf(stderr, "AF probe focus=%4d score=%.3e\n",
					probes[i], s);
				if (s > best_score) { best_score = s; best_pos = probes[i]; }
			}
			memset(&pl, 0, sizeof(pl));
			cb.m.planes = &pl;
			xioctl(cap_fd, VIDIOC_QBUF, &cb);
		}
	}

	struct v4l2_control c = {
		.id = V4L2_CID_FOCUS_ABSOLUTE, .value = best_pos
	};
	xioctl(lfd, VIDIOC_S_CTRL, &c);
	fprintf(stderr, "AF picked focus=%d\n", best_pos);
	close(lfd);
	return best_pos;
}

int main(int argc, char **argv)
{
	/* Per-camera config; default is front. --camera rear flips to rear. */
	const char *cam_dev    = "/dev/video1";
	const char *cam_subdev = "/dev/v4l-subdev18";
	int cam_w   = FRONT_BAYER_W;
	int cam_h   = FRONT_BAYER_H;
	int crop_x  = FRONT_CROP_X;
	int crop_y  = FRONT_CROP_Y;
	/* Per-camera defaults; front max exposure ~65k, rear ~3172 lines.
	 * Picking values that keep the sensor at 30 fps and look reasonable
	 * indoors. Override with --exposure / --gain. */
	int exposure = 5000, gain = 384;
	int do_autofocus = 0;        /* one-shot AF before streaming (rear only) */
	int bitrate = 0;  /* 0 = pick per-camera default after argv parse */
	/* cam-stream uses _STREAM variants (lower R/B than cam-snap's MHC-
	 * tuned defaults) to compensate for the bilinear demosaic. */
	int wb_r_q8 = CAM_WB_FRONT_R_Q8_STREAM;
	int wb_b_q8 = CAM_WB_FRONT_B_Q8_STREAM;
	/* Chroma saturation: 1.0=neutral. With gamma+S-curve applied to Y,
	 * we only need a mild chroma boost (cam-snap applies its 1.5 in
	 * linear RGB before gamma — visually equivalent to less in YUV). */
	/* Tone curve defaults shared with cam-snap (see cam_pipeline.h). */
	int sat_q8     = (int)(CAM_SATURATION_DEFAULT * 256);
	float gamma_val = CAM_GAMMA_DEFAULT;
	float contrast  = CAM_CONTRAST_DEFAULT;
	float brightness = CAM_BRIGHTNESS_DEFAULT;
	int max_frames = 0;     /* 0 = until SIGINT */
	int listen_port = 0;    /* 0 = stdout; >0 = TCP server */
	int out_nv12 = 0;       /* 1 = write raw NV12 to stdout (skip Venus); pipe to ffmpeg */
	int target_fps = 30;    /* paced output framerate. NEON bayer→NV12 holds 30+
				 * comfortably; pacer enforces exact 1/N inter-arrival so
				 * mpv/VLC don't see bursts. */

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--camera") && i+1 < argc) {
			const char *c = argv[++i];
			if (!strcmp(c, "rear")) {
				cam_dev    = "/dev/video0";
				cam_subdev = "/dev/v4l-subdev16";
				cam_w  = REAR_BAYER_W;
				cam_h  = REAR_BAYER_H;
				crop_x = REAR_CROP_X;
				crop_y = REAR_CROP_Y;
				/* Rear's max exposure is ~3172 lines; pick something
				 * a little under that for 30 fps headroom. */
				exposure = 2500;
				wb_r_q8 = CAM_WB_REAR_R_Q8_STREAM;
				wb_b_q8 = CAM_WB_REAR_B_Q8_STREAM;
				do_autofocus = 1;
				/* Rear sustains ~30 fps natively */
				target_fps = 30;
			} else if (!strcmp(c, "front")) {
				/* Front sensor actually delivers ~27 fps in
				 * practice. We declare 30 to Venus so the
				 * H.264 stream's framerate matches what mpv
				 * picks up; wall-clock timestamps keep playback
				 * locked to real time anyway. */
				target_fps = 30;
			} else {
				fprintf(stderr, "--camera: 'rear' or 'front' (got '%s')\n", c);
				return 1;
			}
		}
		else if (!strcmp(a, "--exposure") && i+1 < argc) exposure = atoi(argv[++i]);
		else if (!strcmp(a, "--gain") && i+1 < argc) gain = atoi(argv[++i]);
		else if (!strcmp(a, "--bitrate") && i+1 < argc) bitrate = atoi(argv[++i]);
		else if (!strcmp(a, "--frames") && i+1 < argc) max_frames = atoi(argv[++i]);
		else if (!strcmp(a, "--listen") && i+1 < argc) listen_port = atoi(argv[++i]);
		else if (!strcmp(a, "--out-nv12")) out_nv12 = 1;
		else if (!strcmp(a, "--fps") && i+1 < argc) target_fps = atoi(argv[++i]);
		else if (!strcmp(a, "--autofocus")) do_autofocus = 1;
		else if (!strcmp(a, "--no-autofocus")) do_autofocus = 0;
		else if (!strcmp(a, "--saturation") && i+1 < argc) {
			sat_q8 = (int)(atof(argv[++i]) * 256);
		}
		else if (!strcmp(a, "--gamma") && i+1 < argc) gamma_val = atof(argv[++i]);
		else if (!strcmp(a, "--contrast") && i+1 < argc) contrast = atof(argv[++i]);
		else if (!strcmp(a, "--brightness") && i+1 < argc) brightness = atof(argv[++i]);
		else if (!strcmp(a, "--wb") && i+3 < argc) {
			/* --wb R G B — manual override of camera WB defaults.
			 * G channel is implicit unity in our pipeline; we still
			 * accept it from the CLI for symmetry with cam-snap. */
			wb_r_q8 = (int)(atof(argv[++i]) * 256);
			(void)atof(argv[++i]);  /* G */
			wb_b_q8 = (int)(atof(argv[++i]) * 256);
		}
		else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
			fprintf(stderr,
				"usage: cam-stream [--camera front|rear] [--exposure N] [--gain N]\n"
				"                  [--bitrate bps]   [--frames N]\n"
				"Streams H.264 NAL units to stdout. Pipe to ffplay/vlc.\n");
			return 0;
		}
	}

	/* Per-camera bitrate default if --bitrate wasn't passed.
	 * Front: wcn36xx wifi shares memory bus with Venus and starves under
	 * sustained encode load — 4 Mbps reliably triggers RCU stalls on the
	 * wifi side, 2 Mbps stays stable. Rear has more thermal/timing room. */
	if (bitrate == 0)
		bitrate = (strcmp(cam_dev, "/dev/video1") == 0) ? 2000000 : 4000000;

	cam_pipeline_build_lut(gamma_lut, 256, gamma_val, contrast, brightness);

	signal(SIGINT, on_sigint);
	signal(SIGPIPE, SIG_IGN);

	/* TCP server: open one client socket, then stream H.264 to it.
	 * If --listen wasn't passed, we use stdout (the original mode). */
	int sink_fd = 1;   /* stdout */
	int srv_fd = -1;
	if (listen_port > 0) {
		srv_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (srv_fd < 0) die("socket: %s", strerror(errno));
		int yes = 1;
		setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
		struct sockaddr_in addr = {0};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(listen_port);
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
			die("bind :%d: %s", listen_port, strerror(errno));
		if (listen(srv_fd, 1) < 0)
			die("listen: %s", strerror(errno));
		fprintf(stderr, "cam-stream: listening on tcp/%d, waiting for client…\n",
			listen_port);
		struct sockaddr_in c_addr;
		socklen_t c_len = sizeof(c_addr);
		sink_fd = accept(srv_fd, (struct sockaddr *)&c_addr, &c_len);
		if (sink_fd < 0) die("accept: %s", strerror(errno));
		fprintf(stderr, "cam-stream: client connected from %s:%d\n",
			inet_ntoa(c_addr.sin_addr), ntohs(c_addr.sin_port));
		int nodelay = 1;
		setsockopt(sink_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
	}

	struct buf cap_bufs[CAP_BUFS];
	struct buf enc_out_bufs[ENC_OUT_BUFS];
	struct buf enc_cap_bufs[ENC_CAP_BUFS];

	int cap_fd = cap_open_and_setup(cam_dev, cam_subdev, cam_w, cam_h,
					exposure, gain, cap_bufs);

	/* Run one-shot autofocus before the encoder is up. Sweeps 16 lens
	 * positions in ~1 s and picks the sharpest. Rear camera only — front
	 * has no VCM. */
	if (do_autofocus) {
		const char *lens_dev = "/dev/v4l-subdev17";
		fprintf(stderr, "cam-stream: one-shot AF sweep…\n");
		do_autofocus_priv(cap_fd, cap_bufs, cam_w, cam_h, lens_dev);
	}

	int enc_fd = -1;
	if (!out_nv12)
		enc_fd = enc_setup(enc_out_bufs, enc_cap_bufs, bitrate, target_fps);

	/* --out-nv12 fast path: V4L2 capture → bayer→NV12 → write directly
	 * to sink_fd. No Venus, no TCP server, no V4L2 m2m bookkeeping. Pipe
	 * to `ffmpeg -f rawvideo -pix_fmt nv12 -s 1920x1080 -r 30 -i -` and
	 * let ffmpeg do everything else (encode, mux, stream). */
	if (out_nv12) {
		uint8_t *nv12 = malloc((size_t)OUT_W * OUT_H * 3 / 2);
		if (!nv12) die("malloc nv12");
		double t0 = now_s();
		int fc = 0;
		while (running && (max_frames == 0 || fc < max_frames)) {
			struct v4l2_plane pl = {0};
			struct v4l2_buffer cb = {0};
			cb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			cb.memory = V4L2_MEMORY_MMAP;
			cb.length = 1;
			cb.m.planes = &pl;
			struct pollfd pf = { .fd = cap_fd, .events = POLLIN };
			int pr = poll(&pf, 1, 1000);
			if (pr <= 0) continue;
			if (xioctl(cap_fd, VIDIOC_DQBUF, &cb) < 0) continue;
			bayer_to_nv12(cap_bufs[cb.index].p, nv12,
				      cam_w, crop_x, crop_y,
				      wb_r_q8, wb_b_q8, sat_q8);
			size_t left = (size_t)OUT_W * OUT_H * 3 / 2;
			uint8_t *p = nv12;
			while (left > 0) {
				ssize_t w = write(sink_fd, p, left);
				if (w <= 0) { running = 0; break; }
				p += w; left -= w;
			}
			memset(&pl, 0, sizeof(pl));
			cb.m.planes = &pl;
			xioctl(cap_fd, VIDIOC_QBUF, &cb);
			fc++;
			if (fc % 30 == 0)
				fprintf(stderr, "\rNV12 frames=%d fps=%.1f  ",
					fc, fc / (now_s() - t0));
		}
		fprintf(stderr, "\nout-nv12: %d frames, %.1f fps avg\n",
			fc, fc / (now_s() - t0));
		int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		xioctl(cap_fd, VIDIOC_STREAMOFF, &t);
		close(cap_fd);
		free(nv12);
		return 0;
	}

	fprintf(stderr,
		"cam-stream: cap=%s %dx%d → enc=/dev/video7 %dx%d H.264 %dbps\n",
		cam_dev, cam_w, cam_h, OUT_W, OUT_H, bitrate);

	/* All enc OUTPUT buffers are free initially. We track that with a
	 * simple free-list: enc_out_free[i] = 1 means buffer i is available
	 * to be filled and queued. */
	int enc_out_free[ENC_OUT_BUFS];
	for (int i = 0; i < ENC_OUT_BUFS; i++) enc_out_free[i] = 1;

	double t0 = now_s();
	int frame_count = 0;
	int out_total = 0;

	/* Frame pacer: drop sensor frames that arrive faster than target_fps,
	 * so the encoder + downstream see exactly 1000/N ms inter-arrival.
	 * When our bayer→NV12 sustains slower than target, no pacing happens
	 * and we just process every frame. */
	uint64_t target_interval_us = 1000000ULL / (uint64_t)target_fps;
	uint64_t last_submit_us = 0;

	/* Wall-clock origin for Venus timestamps. Using real elapsed time
	 * rather than `frame_count / target_fps` keeps the stream's playback
	 * rate locked to physical time when target_fps doesn't match the
	 * sensor's actual delivery rate (front sensor delivers ~27 fps but
	 * we declare 15 fps to Venus' rate controller — without wall clock
	 * timestamps mpv buffers ahead and shows lag). */
	struct timespec _stream_start;
	clock_gettime(CLOCK_MONOTONIC, &_stream_start);
	uint64_t stream_start_us = (uint64_t)_stream_start.tv_sec * 1000000ULL +
				   (uint64_t)_stream_start.tv_nsec / 1000ULL;

	while (running && (max_frames == 0 || frame_count < max_frames)) {
		struct pollfd pfds[2] = {
			{ .fd = cap_fd, .events = POLLIN },
			{ .fd = enc_fd, .events = POLLIN | POLLOUT },
		};
		int pr = poll(pfds, 2, 1000);
		if (pr <= 0) {
			if (pr == 0) fprintf(stderr, "poll timeout\n");
			else fprintf(stderr, "poll: %s\n", strerror(errno));
			continue;
		}

		/* Drain encoder bitstream first so we don't backpressure */
		if (pfds[1].revents & POLLIN) {
			struct v4l2_plane pl = {0};
			struct v4l2_buffer b = {0};
			b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			b.memory = V4L2_MEMORY_MMAP;
			b.length = 1;
			b.m.planes = &pl;
			while (xioctl(enc_fd, VIDIOC_DQBUF, &b) == 0) {
				size_t n = pl.bytesused;
				if (n > 0) {
					const uint8_t *p = enc_cap_bufs[b.index].p;
					size_t left = n;
					while (left > 0 && running) {
						ssize_t w = write(sink_fd, p, left);
						if (w < 0) {
							if (errno == EINTR) continue;
							if (errno == EPIPE) {
								fprintf(stderr, "client disconnected\n");
								running = 0;
								break;
							}
							fprintf(stderr, "write: %s\n", strerror(errno));
							running = 0;
							break;
						}
						p += w; left -= w;
					}
					out_total += n;
				}
				memset(&pl, 0, sizeof(pl));
				xioctl(enc_fd, VIDIOC_QBUF, &b);
				memset(&pl, 0, sizeof(pl));
				b = (struct v4l2_buffer){
					.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
					.memory = V4L2_MEMORY_MMAP,
					.length = 1, .m.planes = &pl
				};
			}
		}

		/* Reclaim finished enc OUTPUT buffers */
		if (pfds[1].revents & POLLOUT) {
			struct v4l2_plane pl = {0};
			struct v4l2_buffer b = {0};
			b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
			b.memory = V4L2_MEMORY_MMAP;
			b.length = 1;
			b.m.planes = &pl;
			while (xioctl(enc_fd, VIDIOC_DQBUF, &b) == 0) {
				enc_out_free[b.index] = 1;
				memset(&pl, 0, sizeof(pl));
				b = (struct v4l2_buffer){
					.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
					.memory = V4L2_MEMORY_MMAP,
					.length = 1, .m.planes = &pl
				};
			}
		}

		/* Got a fresh bayer frame? Process it. */
		if (pfds[0].revents & POLLIN) {
			struct v4l2_plane pl = {0};
			struct v4l2_buffer cb = {0};
			cb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			cb.memory = V4L2_MEMORY_MMAP;
			cb.length = 1;
			cb.m.planes = &pl;
			if (xioctl(cap_fd, VIDIOC_DQBUF, &cb) < 0) {
				if (errno != EAGAIN)
					fprintf(stderr, "cap DQBUF: %s\n", strerror(errno));
				continue;
			}

			/* Find a free enc OUTPUT buffer */
			int oi = -1;
			for (int i = 0; i < ENC_OUT_BUFS; i++)
				if (enc_out_free[i]) { oi = i; break; }
			if (oi < 0) {
				/* All in flight — drop this bayer frame */
				xioctl(cap_fd, VIDIOC_QBUF, &cb);
				continue;
			}

			/* Pacer disabled — let throughput run as fast as the
			 * encoder + sensor allow. target_fps still feeds the
			 * Venus rate controller via VIDIOC_S_PARM. */
			(void)target_interval_us; (void)last_submit_us;

			bayer_to_nv12(cap_bufs[cb.index].p,
				      enc_out_bufs[oi].p,
				      cam_w, crop_x, crop_y,
				      wb_r_q8, wb_b_q8, sat_q8);

			/* Queue NV12 to encoder.
			 * Venus requires monotonic timestamps on input frames
			 * for its rate controller — without them it produces
			 * only one I-frame and goes silent. */
			struct v4l2_plane opl = {0};
			opl.bytesused = OUT_W * OUT_H * 3 / 2;
			opl.length = enc_out_bufs[oi].len;
			struct v4l2_buffer ob = {0};
			ob.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
			ob.memory = V4L2_MEMORY_MMAP;
			ob.index = oi;
			ob.length = 1;
			ob.m.planes = &opl;
			ob.flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
			/* Wall-clock timestamps so playback rate tracks real
			 * time. Venus uses these for rate control + the GOP
			 * timestamps end up in the H.264 PTS, so mpv plays at
			 * the actual capture cadence. */
			struct timespec _ts_now;
			clock_gettime(CLOCK_MONOTONIC, &_ts_now);
			uint64_t ts_us = (uint64_t)_ts_now.tv_sec * 1000000ULL +
					 (uint64_t)_ts_now.tv_nsec / 1000ULL -
					 stream_start_us;
			ob.timestamp.tv_sec  = (long)(ts_us / 1000000ULL);
			ob.timestamp.tv_usec = (long)(ts_us % 1000000ULL);
			if (xioctl(enc_fd, VIDIOC_QBUF, &ob) < 0) {
				fprintf(stderr, "enc OUT QBUF: %s\n", strerror(errno));
			} else {
				enc_out_free[oi] = 0;
			}

			/* Re-queue capture buffer */
			memset(&pl, 0, sizeof(pl));
			cb.m.planes = &pl;
			xioctl(cap_fd, VIDIOC_QBUF, &cb);

			frame_count++;
			if (frame_count % 30 == 0) {
				double elapsed = now_s() - t0;
				fprintf(stderr,
					"\rframes=%d  fps=%.1f  out=%d KB  ",
					frame_count, frame_count / elapsed,
					out_total / 1024);
			}
		}
	}

	int t;
	t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; xioctl(cap_fd, VIDIOC_STREAMOFF, &t);
	t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;  xioctl(enc_fd, VIDIOC_STREAMOFF, &t);
	t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; xioctl(enc_fd, VIDIOC_STREAMOFF, &t);
	close(cap_fd);
	close(enc_fd);
	fprintf(stderr,
		"\ncam-stream: done. frames=%d, %.1f fps avg, %d KB output\n",
		frame_count, frame_count / (now_s() - t0), out_total / 1024);
	return 0;
}

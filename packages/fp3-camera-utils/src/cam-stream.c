// SPDX-License-Identifier: MIT
/*
 * cam-stream: real-time H.264 video from FP3+ rear or front camera.
 *
 * Pipeline:
 *   V4L2 bayer capture (node resolved via fp3-cam-setup)
 *     → bayer → NV12 (1920x1080, integer 2x downscale + center crop + WB)
 *     → Venus H.264 m2m encoder (node found by capability)
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
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/videodev2.h>
#include <math.h>
#include <arm_neon.h>
#include "cam_pipeline.h"

/* cam-stream uses each sensor's 2×2-binned mode (4× less sensor→DRAM
 * bandwidth + better SNR — critical for wcn36xx wifi co-existence)
 * and produces a 1920×1080 NV12 stream via 1:1 bilinear demosaic of a
 * center-cropped sub-rectangle of the binned bayer.
 *
 * Front s5k3p9sp binned: 4608×3456 → 2304×1728 → crop (192, 324) → 1920×1080
 * Rear  s5kgm1sp binned: 4000×3000 → 2000×1500 → crop (40, 210)  → 1920×1080
 *
 * cam-snap still uses full sensor resolution via cam_pipeline.h's
 * CAM_BAYER_W_* constants. */
#define FRONT_BAYER_W_BINNED   2304
#define FRONT_BAYER_H_BINNED   1728
#define REAR_BAYER_W_BINNED    2000
#define REAR_BAYER_H_BINNED    1500

#define OUT_W   1920
#define OUT_H   1080

/*
 * The Bayer phase of the fitted sensor, and the encoder's frame size.
 *
 * A Bayer pattern differs from GRBG only by where the 2x2 tile starts,
 * so rather than teaching the demosaic four phases we shift the crop
 * origin by 0 or 1 pixel and let the existing GRBG path do the work:
 *   GRBG (0,0)   RGGB (1,0)   BGGR (0,1)   GBRG (1,1)
 * The crop is even-aligned, so adding these keeps the phase exact.
 *
 * The output size cannot be a constant either: the Fairphone 3's front
 * sensor binned is 1440x1080, narrower than 1920, so a fixed 1920-wide
 * frame is unreachable there. Derived from the source in main().
 */
static int g_phase_dx = 0, g_phase_dy = 0;
static uint32_t g_cap_fourcc = V4L2_PIX_FMT_SGRBG10P;
static int g_out_w = OUT_W, g_out_h = OUT_H;

/* Sized so two concurrent cam-stream instances (front + rear) don't
 * starve the CAMSS write-master IRQs. With CAP_BUFS=4 the kernel was
 * logging "qcom-camss: Missing ready buf 0 5!" whenever Venus took a
 * jitter-spike encoding the other feed, dropping or corrupting rear
 * sensor frames (visible noise in mpv). Doubling each ring gives the
 * userspace main loop ~270 ms of slack at 30 fps instead of ~133 ms. */
#define CAP_BUFS     8
#define ENC_OUT_BUFS 6
#define ENC_CAP_BUFS 8

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
			      struct buf bufs[CAP_BUFS],
			      int *out_bytesperline)
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
	fmt.fmt.pix_mp.pixelformat = g_cap_fourcc;  /* set from the sensor's Bayer order */
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
		die("cap S_FMT: %s", strerror(errno));

	/* Capture the actual row stride V4L2 gave us — for some packed
	 * bayer widths (e.g., 2000 on the rear ybin mode) the driver pads
	 * each row up to a multiple of 8 bytes, so 2000*5/4=2500 becomes
	 * 2504. Walking the buffer with the unpadded stride would misalign
	 * every row after the first and produce red/blue banding garbage. */
	if (out_bytesperline)
		*out_bytesperline = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;

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

/*
 * Take the capture node, its subdev and the binned geometry from
 * /run/fp3-cam-<cam>.conf, which fp3-cam-setup writes after resolving
 * the media graph. Nothing here may be hardcoded: the modules are
 * user-replaceable, so the sensor and its geometry differ per phone,
 * and /dev/videoN and /dev/v4l-subdevN are renumbered per boot.
 */
static void load_cam_conf(const char *cam, const char **dev, const char **subdev,
			  int *w, int *h, char *bayer, size_t bayerlen)
{
	static char devbuf[64], subbuf[64];
	char path[64];
	snprintf(path, sizeof(path), "/run/fp3-cam-%s.conf", cam);

	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "cam-stream: %s missing, using defaults\n", path);
		return;
	}

	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char *k = line, *v = eq + 1;
		v[strcspn(v, "\r\n")] = '\0';
		if (!strcmp(k, "VIDEO") && *v) {
			snprintf(devbuf, sizeof(devbuf), "%s", v);
			*dev = devbuf;
		} else if (!strcmp(k, "SUBDEV") && *v) {
			snprintf(subbuf, sizeof(subbuf), "%s", v);
			*subdev = subbuf;
		} else if (!strcmp(k, "WIDTH") && *v) {
			*w = atoi(v);
		} else if (!strcmp(k, "HEIGHT") && *v) {
			*h = atoi(v);
		} else if (!strcmp(k, "BAYER") && *v) {
			snprintf(bayer, bayerlen, "%s", v);
		}
	}
	fclose(f);
	fprintf(stderr, "cam-stream: %s -> %s (%s) %dx%d\n", cam, *dev, *subdev, *w, *h);
}

/*
 * Locate the Venus H.264 m2m encoder.
 *
 * This used to be hardcoded to /dev/video7. It is not stable: CAMSS and
 * Venus register their video nodes in whatever order they probe, so the
 * encoder moves between boots and between the two phones — the same
 * reason fp3-cam-setup resolves the capture node from the media graph
 * rather than assuming it. Opening the wrong node here fails obscurely,
 * as "enc OUTPUT S_FMT: Invalid argument", because a CAMSS capture node
 * quite reasonably rejects an encoder's format.
 *
 * Probe by capability instead: an m2m device that accepts H.264 on its
 * CAPTURE queue is the encoder, whatever number it was given.
 */
static const char *find_h264_encoder(char *buf, size_t buflen)
{
	for (int i = 0; i < 64; i++) {
		snprintf(buf, buflen, "/dev/video%d", i);
		int fd = open(buf, O_RDWR | O_NONBLOCK);
		if (fd < 0)
			continue;

		struct v4l2_capability cap = {0};
		if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
			close(fd);
			continue;
		}
		if (!(cap.capabilities & (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_VIDEO_M2M))) {
			close(fd);
			continue;
		}

		/* Does its CAPTURE queue produce H.264? */
		int found = 0;
		for (int j = 0; j < 32 && !found; j++) {
			struct v4l2_fmtdesc fd_ = {0};
			fd_.index = j;
			fd_.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			if (ioctl(fd, VIDIOC_ENUM_FMT, &fd_) < 0)
				break;
			if (fd_.pixelformat == V4L2_PIX_FMT_H264)
				found = 1;
		}
		close(fd);
		if (found)
			return buf;
	}
	return NULL;
}

static int enc_setup(struct buf outs[ENC_OUT_BUFS],
		     struct buf caps[ENC_CAP_BUFS],
		     int bitrate, int target_fps)
{
	char devbuf[32];
	const char *dev = find_h264_encoder(devbuf, sizeof(devbuf));
	if (!dev)
		die("no V4L2 H.264 m2m encoder found");
	int fd = open(dev, O_RDWR | O_NONBLOCK);
	if (fd < 0) die("open %s: %s", dev, strerror(errno));
	fprintf(stderr, "cam-stream: encoder %s\n", dev);

	/* OUTPUT (input frames): NV12 1920x1080 */
	struct v4l2_format fmt = {0};
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	fmt.fmt.pix_mp.width = g_out_w;
	fmt.fmt.pix_mp.height = g_out_h;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
		die("enc OUTPUT S_FMT: %s", strerror(errno));

	/* CAPTURE (encoded bitstream): H.264 */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = g_out_w;
	fmt.fmt.pix_mp.height = g_out_h;
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
		/* Venus pads the NV12 input buffer up to its macroblock-aligned
		 * size (typically rounded to 16-pixel height = 1088 rows for
		 * 1080p). Our kernel writes only 1080 rows; the trailing
		 * uninitialized bytes get read by the encoder and show up as a
		 * thick green band at the bottom of the decoded stream. Pre-
		 * init the buffer to a neutral NV12 black so any unwritten
		 * region looks like padding, not garbage. */
		size_t y_bytes = (size_t)g_out_w * g_out_h;
		if (y_bytes <= outs[i].len) {
			memset(outs[i].p, 0, y_bytes);                /* Y plane = 0 */
			memset(outs[i].p + y_bytes, 128, outs[i].len - y_bytes); /* UV plane = 128 neutral */
		} else {
			memset(outs[i].p, 0, outs[i].len);
		}
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
/* 256-entry LUT: indexed by 8-bit linear bayer value (post-WB clamp),
 * returns 8-bit gamma-encoded + tone-curved value. cam-stream takes the
 * high 8 bits of each 10-bit pixel — the 2 LSBs would only matter for
 * still photography (cam-snap keeps them via its 1024-entry LUT). */
static uint8_t gamma_lut[256];

/* Live-tuning shared state. Updated by the control thread (text-line
 * TCP server); main thread re-syncs into local frame values at the top
 * of each frame's processing pass. The mutex is only contended on the
 * brief sync window, not the hot demosaic loop. */
struct ctrl_state {
	pthread_mutex_t mu;
	int   wb_r_q8;
	int   wb_b_q8;
	int   sat_q8;
	float gamma_val;
	float contrast;
	float brightness;
	int   rebuild_lut;   /* set by control thread when gamma/contrast/
				brightness change; main rebuilds at next
				frame boundary */
};
static struct ctrl_state ctrl;
static int control_port = 0;   /* 0 = no control socket */
static const char *cam_subdev_path = NULL;   /* sensor subdev (exposure/gain ctrl) */
static const char *lens_dev_path   = NULL;   /* VCM subdev (live focus ctrl; rear only) */

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

/* Per-thread context for the bilinear demosaic. Each worker owns a
 * disjoint row range [oy_start, oy_end) of the NV12 output, so workers
 * write to non-overlapping memory and need no mutex. */
struct demosaic_ctx {
	const uint8_t *bayer8;
	int W, H;
	int crop_x, crop_y;
	uint8_t *yp, *uvp;
	int out_w;
	int oy_start, oy_end;
	int wb_r_q8, wb_b_q8, sat_q8;
};

/* Bilinear demosaic + WB + gamma + RGB→NV12 for output rows
 * [oy_start, oy_end). Bayer pattern is SGRBG (row even: G_top R; row
 * odd: B G_bot). 1:1 mapping from binned bayer to NV12 — caller has
 * already done the unpack pass into bayer8. */
static void demosaic_rows(const struct demosaic_ctx *c)
{
	const uint8_t *bayer8 = c->bayer8;
	const int W       = c->W;
	const int crop_x  = c->crop_x;
	const int crop_y  = c->crop_y;
	const int out_w   = c->out_w;
	const int wb_r_q8 = c->wb_r_q8;
	const int wb_b_q8 = c->wb_b_q8;
	const int sat_q8  = c->sat_q8;
	uint8_t *yp  = c->yp;
	uint8_t *uvp = c->uvp;

	/* Direct read of bayer8; no clamping (center-crop keeps us in-bounds). */
	#define BG(yy, xx) bayer8[(size_t)(yy) * W + (xx)]
	#define WBG(v, wb_q8) gamma_lut[ ({ int _t = ((int)(v) * (wb_q8)) >> 8; _t > 255 ? 255 : _t; }) ]
	#define G_GAMMA(v) gamma_lut[(v)]
	#define RGB2Y(R,G,B) (uint8_t)(((77*(int)(R) + 150*(int)(G) + 29*(int)(B) + 128) >> 8))

	for (int oy = c->oy_start; oy < c->oy_end; oy += 2) {
		int by = oy + crop_y;
		uint8_t *y0_row = yp + (size_t)oy       * out_w;
		uint8_t *y1_row = yp + (size_t)(oy + 1) * out_w;
		uint8_t *uv_row = uvp + (size_t)(oy / 2) * out_w;

		for (int ox = 0; ox < out_w; ox += 2) {
			int bx = ox + crop_x;

			int r00 = (BG(by, bx-1) + BG(by, bx+1)) >> 1;
			int g00 = BG(by, bx);
			int b00 = (BG(by-1, bx) + BG(by+1, bx)) >> 1;

			int r01 = BG(by, bx+1);
			int g01 = (BG(by, bx) + BG(by, bx+2) + BG(by-1, bx+1) + BG(by+1, bx+1)) >> 2;
			int b01 = (BG(by-1, bx) + BG(by-1, bx+2) + BG(by+1, bx) + BG(by+1, bx+2)) >> 2;

			int r10 = (BG(by, bx-1) + BG(by, bx+1) + BG(by+2, bx-1) + BG(by+2, bx+1)) >> 2;
			int g10 = (BG(by+1, bx-1) + BG(by+1, bx+1) + BG(by, bx) + BG(by+2, bx)) >> 2;
			int b10 = BG(by+1, bx);

			int r11 = (BG(by, bx+1) + BG(by+2, bx+1)) >> 1;
			int g11 = BG(by+1, bx+1);
			int b11 = (BG(by+1, bx) + BG(by+1, bx+2)) >> 1;

			uint8_t Rg00 = WBG(r00, wb_r_q8), Gg00 = G_GAMMA(g00), Bg00 = WBG(b00, wb_b_q8);
			uint8_t Rg01 = WBG(r01, wb_r_q8), Gg01 = G_GAMMA(g01), Bg01 = WBG(b01, wb_b_q8);
			uint8_t Rg10 = WBG(r10, wb_r_q8), Gg10 = G_GAMMA(g10), Bg10 = WBG(b10, wb_b_q8);
			uint8_t Rg11 = WBG(r11, wb_r_q8), Gg11 = G_GAMMA(g11), Bg11 = WBG(b11, wb_b_q8);

			y0_row[ox  ] = RGB2Y(Rg00, Gg00, Bg00);
			y0_row[ox+1] = RGB2Y(Rg01, Gg01, Bg01);
			y1_row[ox  ] = RGB2Y(Rg10, Gg10, Bg10);
			y1_row[ox+1] = RGB2Y(Rg11, Gg11, Bg11);

			int Ravg = ((int)Rg00 + Rg01 + Rg10 + Rg11) >> 2;
			int Gavg = ((int)Gg00 + Gg01 + Gg10 + Gg11) >> 2;
			int Bavg = ((int)Bg00 + Bg01 + Bg10 + Bg11) >> 2;

			int u = ((-43*Ravg - 85*Gavg + 128*Bavg) >> 8) + 128;
			int v = (( 128*Ravg - 107*Gavg - 21*Bavg) >> 8) + 128;
			u = ((u - 128) * sat_q8 >> 8) + 128;
			v = ((v - 128) * sat_q8 >> 8) + 128;
			if (u < 0) u = 0; else if (u > 255) u = 255;
			if (v < 0) v = 0; else if (v > 255) v = 255;
			uv_row[ox  ] = (uint8_t)u;
			uv_row[ox+1] = (uint8_t)v;
		}
	}

	#undef BG
	#undef WBG
	#undef G_GAMMA
	#undef RGB2Y
}

static void *demosaic_worker(void *arg)
{
	demosaic_rows((const struct demosaic_ctx *)arg);
	return NULL;
}

/* Number of worker threads. msm8953 has 4 Cortex-A53 cores; each chunk
 * of out_h/N rows is independent (no shared writes), so we get close to
 * linear speedup until DRAM contention takes over. */
#define DEMOSAIC_THREADS 4

/* Bilinear demosaic + WB + saturation + gamma + RGB→NV12, parallelized.
 * Designed for 2×2-binned sensor output; each binned bayer pixel is
 * already a 4-photodiode average. Caller passes the bayer's actual
 * (V4L2-reported) row stride so we honor the driver's row padding.
 *
 * One thread does the 5-bytes-to-4-pixels unpack pass over the whole
 * bayer buffer (fast, memory-bandwidth-bound), then DEMOSAIC_THREADS
 * workers run the bilinear+color step over disjoint output row
 * ranges. */
static void bayer_to_nv12(const uint8_t *bayer_packed, uint8_t *nv12,
			  int bayer_w, int bayer_h,
			  int bayer_stride_packed,
			  int out_w, int out_h,
			  int wb_r_q8, int wb_b_q8, int sat_q8)
{
	const int W = bayer_w;
	const int H = bayer_h;
	const int crop_x = (((bayer_w - out_w) / 2) & ~1) + g_phase_dx;
	const int crop_y = (((bayer_h - out_h) / 2) & ~1) + g_phase_dy;
	uint8_t *yp  = nv12;
	uint8_t *uvp = nv12 + (size_t)out_w * out_h;

	/* Unpack SGRBG10P → 8-bit linear bayer, kept across frames. */
	static uint8_t *bayer8 = NULL;
	static size_t bayer8_cap = 0;
	size_t need = (size_t)W * H;
	if (need > bayer8_cap) {
		free(bayer8);
		bayer8 = (uint8_t *)malloc(need);
		if (!bayer8) die("bayer8 malloc: %s", strerror(errno));
		bayer8_cap = need;
	}
	{
		const int row_stride_p = bayer_stride_packed;
		for (int y = 0; y < H; y++) {
			const uint8_t *p = bayer_packed + (size_t)y * row_stride_p;
			uint8_t *q = bayer8 + (size_t)y * W;
			for (int x = 0; x < W; x += 4, p += 5) {
				q[x+0] = p[0];
				q[x+1] = p[1];
				q[x+2] = p[2];
				q[x+3] = p[3];
			}
		}
	}

	/* Build N contexts, one per worker. Each gets out_h/N rows. */
	struct demosaic_ctx ctx[DEMOSAIC_THREADS];
	int chunk = (out_h / DEMOSAIC_THREADS) & ~1;  /* keep chunk size even */
	for (int t = 0; t < DEMOSAIC_THREADS; t++) {
		ctx[t] = (struct demosaic_ctx){
			.bayer8  = bayer8,
			.W       = W,
			.H       = H,
			.crop_x  = crop_x,
			.crop_y  = crop_y,
			.yp      = yp,
			.uvp     = uvp,
			.out_w   = out_w,
			.oy_start = t * chunk,
			.oy_end   = (t == DEMOSAIC_THREADS - 1) ? out_h : (t + 1) * chunk,
			.wb_r_q8 = wb_r_q8,
			.wb_b_q8 = wb_b_q8,
			.sat_q8  = sat_q8,
		};
	}

	/* Spawn N-1 workers; do the first chunk on the calling thread. */
	pthread_t workers[DEMOSAIC_THREADS - 1];
	for (int t = 1; t < DEMOSAIC_THREADS; t++) {
		if (pthread_create(&workers[t-1], NULL, demosaic_worker, &ctx[t]) != 0) {
			/* Fall back to running it inline on a thread-create failure. */
			demosaic_rows(&ctx[t]);
			workers[t-1] = 0;
		}
	}
	demosaic_rows(&ctx[0]);
	for (int t = 0; t < DEMOSAIC_THREADS - 1; t++)
		if (workers[t]) pthread_join(workers[t], NULL);
}

/* ----------------------------------------------------------------------
 * Live tuning control socket
 * --------------------------------------------------------------------*/

/* Accepts one client at a time on `control_port` and parses newline-
 * terminated commands. Commands recognised:
 *
 *   wb <r_gain> <g_gain> <b_gain>       — float, R+B converted to Q8.8
 *   gamma <f>                            — float
 *   contrast <f>                         — float, 0..1+
 *   saturation <f>                       — float, 1.0 = neutral
 *   brightness <f>                       — float, 1.0 = neutral
 *   show                                 — echo current values
 *
 * Unknown commands are silently ignored. The thread runs for the
 * lifetime of cam-stream and re-accepts on disconnect. */
/* Push a V4L2 control on an arbitrary subdev path. Used for sensor
 * exposure/gain (cam_subdev_path) and the rear VCM focus position
 * (lens_dev_path). Bypasses the per-frame pipeline state — reprograms
 * the hardware directly. */
static void set_subdev_ctrl(const char *path, int v4l2_cid, int value,
			    const char *label)
{
	if (!path) return;
	int fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "cam-stream: open %s for %s: %s\n",
			path, label, strerror(errno));
		return;
	}
	struct v4l2_control c = { .id = v4l2_cid, .value = value };
	if (xioctl(fd, VIDIOC_S_CTRL, &c) < 0)
		fprintf(stderr, "cam-stream: S_CTRL %s=%d failed: %s\n",
			label, value, strerror(errno));
	close(fd);
}

static void apply_ctrl_cmd(const char *line, int fd_out)
{
	float a, b, c;
	int  i;
	pthread_mutex_lock(&ctrl.mu);
	if (sscanf(line, " wb %f %f %f", &a, &b, &c) == 3) {
		ctrl.wb_r_q8 = (int)(a * 256.0f);
		ctrl.wb_b_q8 = (int)(c * 256.0f);
		(void)b;  /* G gain implicit at 1.0 */
	} else if (sscanf(line, " gamma %f", &a) == 1) {
		ctrl.gamma_val = a;
		ctrl.rebuild_lut = 1;
	} else if (sscanf(line, " contrast %f", &a) == 1) {
		ctrl.contrast = a;
		ctrl.rebuild_lut = 1;
	} else if (sscanf(line, " saturation %f", &a) == 1) {
		ctrl.sat_q8 = (int)(a * 256.0f);
	} else if (sscanf(line, " brightness %f", &a) == 1) {
		ctrl.brightness = a;
		ctrl.rebuild_lut = 1;
	} else if (sscanf(line, " exposure %d", &i) == 1) {
		pthread_mutex_unlock(&ctrl.mu);
		set_subdev_ctrl(cam_subdev_path, V4L2_CID_EXPOSURE, i, "exposure");
		return;
	} else if (sscanf(line, " gain %d", &i) == 1) {
		pthread_mutex_unlock(&ctrl.mu);
		set_subdev_ctrl(cam_subdev_path, V4L2_CID_ANALOGUE_GAIN, i, "gain");
		return;
	} else if (sscanf(line, " focus %d", &i) == 1) {
		pthread_mutex_unlock(&ctrl.mu);
		if (i < 0) i = 0; else if (i > 1023) i = 1023;
		set_subdev_ctrl(lens_dev_path, V4L2_CID_FOCUS_ABSOLUTE, i, "focus");
		return;
	} else if (strncmp(line, "show", 4) == 0) {
		char buf[256];
		int n = snprintf(buf, sizeof(buf),
			"wb=%.3f/%.3f gamma=%.3f contrast=%.3f saturation=%.3f brightness=%.3f\n",
			ctrl.wb_r_q8 / 256.0f, ctrl.wb_b_q8 / 256.0f,
			ctrl.gamma_val, ctrl.contrast,
			ctrl.sat_q8 / 256.0f, ctrl.brightness);
		if (fd_out >= 0 && n > 0) (void)!write(fd_out, buf, (size_t)n);
	}
	pthread_mutex_unlock(&ctrl.mu);
}

static void *control_thread(void *arg)
{
	(void)arg;
	int srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) return NULL;
	int yes = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(control_port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "cam-stream: control bind :%d failed: %s\n",
			control_port, strerror(errno));
		close(srv);
		return NULL;
	}
	if (listen(srv, 1) < 0) {
		close(srv);
		return NULL;
	}
	fprintf(stderr, "cam-stream: control socket listening on :%d\n", control_port);

	for (;;) {
		int cli = accept(srv, NULL, NULL);
		if (cli < 0) {
			if (errno == EINTR) continue;
			break;
		}
		char buf[256];
		size_t fill = 0;
		for (;;) {
			ssize_t n = read(cli, buf + fill, sizeof(buf) - 1 - fill);
			if (n <= 0) break;
			fill += (size_t)n;
			buf[fill] = '\0';
			/* Process complete lines. */
			char *line = buf;
			char *nl;
			while ((nl = strchr(line, '\n')) != NULL) {
				*nl = '\0';
				apply_ctrl_cmd(line, cli);
				line = nl + 1;
			}
			fill = strlen(line);
			if (fill > 0) memmove(buf, line, fill);
		}
		close(cli);
	}
	close(srv);
	return NULL;
}

/* ----------------------------------------------------------------------
 * Encoder drain (Venus CAPTURE + TCP write) — runs on its own thread so
 * the sensor side never blocks on Venus latency or a slow mpv client.
 * --------------------------------------------------------------------*/

struct drain_ctx {
	int enc_fd;
	int sink_fd;
	struct buf *enc_cap_bufs;
	int n_cap_bufs;
	uint64_t *out_total;
};

static void *drain_thread(void *arg)
{
	struct drain_ctx *c = arg;

	while (running) {
		struct pollfd pf = { .fd = c->enc_fd, .events = POLLIN };
		int pr = poll(&pf, 1, 500);
		if (pr <= 0) continue;
		if (!(pf.revents & POLLIN)) continue;

		struct v4l2_plane pl = {0};
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
			.length = 1,
			.m.planes = &pl,
		};
		while (xioctl(c->enc_fd, VIDIOC_DQBUF, &b) == 0) {
			size_t n = pl.bytesused;
			if (n > 0) {
				const uint8_t *p = c->enc_cap_bufs[b.index].p;
				size_t left = n;
				while (left > 0 && running) {
					ssize_t w = write(c->sink_fd, p, left);
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
				if (c->out_total) *c->out_total += n;
			}
			memset(&pl, 0, sizeof(pl));
			xioctl(c->enc_fd, VIDIOC_QBUF, &b);
			memset(&pl, 0, sizeof(pl));
			b = (struct v4l2_buffer){
				.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
				.memory = V4L2_MEMORY_MMAP,
				.length = 1, .m.planes = &pl
			};
		}
	}
	return NULL;
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
	/* V4L2 pads packed-bayer rows to 8-byte multiples; honor that. */
	const int row_stride = ((bayer_w * 5 / 4) + 7) & ~7;
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
	const char *cam_name   = "front";
	const char *cam_dev    = "/dev/video1";
	const char *cam_subdev = "/dev/v4l-subdev18";
	int cam_w   = FRONT_BAYER_W_BINNED;
	int cam_h   = FRONT_BAYER_H_BINNED;
	/* Per-camera defaults; binning sums 4 photodiodes per output pixel,
	 * so the linear signal is ~4× brighter than at full res. Defaults
	 * here are tuned for binned mode (front). Override with
	 * --exposure / --gain. */
	int exposure = 1250, gain = 384;
	int do_autofocus = 0;        /* one-shot AF before streaming (rear only) */
	int focus_pos = 0;           /* DW9800W DAC value, 0=infinity .. 1023=macro */
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
				cam_name   = "rear";
				cam_dev    = "/dev/video0";
				cam_subdev = "/dev/v4l-subdev16";
				cam_w  = REAR_BAYER_W_BINNED;
				cam_h  = REAR_BAYER_H_BINNED;
				/* Rear binned mode max-exposure scaling: ybin reduces
				 * frame time so the exposure range shrinks ~half vs
				 * full-res; 1250 lines stays in range and matches
				 * typical indoor brightness with binning's 4× gain. */
				exposure = 1250;
				wb_r_q8 = CAM_WB_REAR_R_Q8_STREAM;
				wb_b_q8 = CAM_WB_REAR_B_Q8_STREAM;
				/* Skip the AF sweep on rear by default — the per-frame
				 * Laplacian score's signal-to-noise ratio is too low for
				 * reliable peak detection in binned-bayer mode (all 16
				 * probe positions report scores within ~5% of each
				 * other, so the picked peak is essentially noise — and
				 * lands near macro). Default to lens-at-infinity which
				 * is right for typical room/outdoor framing. Override
				 * with --focus N (0=∞, 1023=macro) or --autofocus to
				 * run the sweep. */
				focus_pos = 0;
				/* Rear sustains ~30 fps natively */
				target_fps = 30;
			} else if (!strcmp(c, "front")) {
				cam_name   = "front";
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
		else if (!strcmp(a, "--control") && i+1 < argc) control_port = atoi(argv[++i]);
		else if (!strcmp(a, "--out-nv12")) out_nv12 = 1;
		else if (!strcmp(a, "--fps") && i+1 < argc) target_fps = atoi(argv[++i]);
		else if (!strcmp(a, "--autofocus")) do_autofocus = 1;
		else if (!strcmp(a, "--no-autofocus")) do_autofocus = 0;
		else if (!strcmp(a, "--focus") && i+1 < argc) focus_pos = atoi(argv[++i]);
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
				"                  [--bitrate bps]   [--frames N] [--listen PORT]\n"
				"                  [--out-nv12]\n"
				"Without --listen, writes H.264 to stdout; pipe it to ffplay/vlc.\n"
				"With --listen PORT, serves the H.264 stream to one TCP client:\n"
				"    ffplay tcp://<phone>:PORT      (or: mpv tcp://<phone>:PORT)\n"
				"This is a raw H.264 stream over TCP, not HTTP -- a browser\n"
				"cannot open it.\n");
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

	/* Seed the live-tuning state with the parsed CLI values and start
	 * the control socket (default port = listen_port + 1 when streaming
	 * over TCP; override with --control PORT). */
	pthread_mutex_init(&ctrl.mu, NULL);
	ctrl.wb_r_q8  = wb_r_q8;
	ctrl.wb_b_q8  = wb_b_q8;
	ctrl.sat_q8   = sat_q8;
	ctrl.gamma_val  = gamma_val;
	ctrl.contrast   = contrast;
	ctrl.brightness = brightness;
	ctrl.rebuild_lut = 0;
	if (control_port == 0 && listen_port > 0) control_port = listen_port + 1;
	cam_subdev_path = cam_subdev;
	if (strcmp(cam_dev, "/dev/video0") == 0)
		lens_dev_path = "/dev/v4l-subdev17";  /* DW9800W rear VCM */
	if (control_port > 0) {
		pthread_t ctrl_th;
		pthread_create(&ctrl_th, NULL, control_thread, NULL);
		pthread_detach(ctrl_th);
	}

	/* Reconfigure the CAMSS media graph for the binned sensor mode.
	 * fp3-cam-setup is idempotent and fast (~50 ms of media-ctl calls).
	 * Running it here means cam-stream always gets the geometry it
	 * expects, regardless of what cam-snap or the Elixir manager
	 * left the subdevs set to. */
	{
		const char *which = cam_name;
		char setup_cmd[128];
		/* Redirect setup output to stderr so it doesn't pollute the
		 * NV12/H.264 stream on stdout in --out-nv12 mode. */
		snprintf(setup_cmd, sizeof(setup_cmd),
			 "/usr/bin/fp3-cam-setup --binned %s 1>&2", which);
		int rc = system(setup_cmd);
		if (rc != 0)
			fprintf(stderr, "cam-stream: fp3-cam-setup returned %d (continuing)\n", rc);

		/* Adopt whatever it resolved, rather than the defaults above. */
		char bayer[8] = "grbg";
		load_cam_conf(cam_name, &cam_dev, &cam_subdev, &cam_w, &cam_h,
			      bayer, sizeof(bayer));

		/* Bayer order decides both the capture fourcc and the crop
		 * phase that normalises the sensor to the GRBG demosaic. */
		if (!strcmp(bayer, "rggb")) {
			g_cap_fourcc = V4L2_PIX_FMT_SRGGB10P;
			g_phase_dx = 1; g_phase_dy = 0;
		} else if (!strcmp(bayer, "bggr")) {
			g_cap_fourcc = V4L2_PIX_FMT_SBGGR10P;
			g_phase_dx = 0; g_phase_dy = 1;
		} else if (!strcmp(bayer, "gbrg")) {
			g_cap_fourcc = V4L2_PIX_FMT_SGBRG10P;
			g_phase_dx = 1; g_phase_dy = 1;
		} else {
			g_cap_fourcc = V4L2_PIX_FMT_SGRBG10P;
			g_phase_dx = 0; g_phase_dy = 0;
		}

		/* Largest 16-aligned frame that fits the source, capped at
		 * 1080p. The phase offset costs a pixel, hence the -1. */
		int fit_w = (cam_w - 1) & ~15;
		int fit_h = (cam_h - 1) & ~15;
		g_out_w = fit_w < OUT_W ? fit_w : OUT_W;
		g_out_h = fit_h < OUT_H ? fit_h : OUT_H;
		fprintf(stderr, "cam-stream: bayer=%s out=%dx%d\n",
			bayer, g_out_w, g_out_h);
	}

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

	int cap_bytesperline = 0;
	int cap_fd = cap_open_and_setup(cam_dev, cam_subdev, cam_w, cam_h,
					exposure, gain, cap_bufs,
					&cap_bytesperline);

	/* Rear lens-position policy:
	 *   - --autofocus     → run the 16-position sweep (noisy in binned
	 *                       mode; off by default)
	 *   - --focus N       → use DAC value N directly (0=∞ .. 1023=macro)
	 *   - neither flag    → DAC = 0 (infinity), good default for normal
	 *                       indoor / outdoor framing
	 * Front cam has no VCM so the path is skipped. */
	const char *lens_dev = "/dev/v4l-subdev17";
	if (strcmp(cam_dev, "/dev/video0") == 0) {
		if (do_autofocus) {
			fprintf(stderr, "cam-stream: one-shot AF sweep…\n");
			do_autofocus_priv(cap_fd, cap_bufs, cam_w, cam_h, lens_dev);
		} else {
			int lfd = open(lens_dev, O_RDWR);
			if (lfd >= 0) {
				struct v4l2_control c = {
					.id = V4L2_CID_FOCUS_ABSOLUTE,
					.value = focus_pos < 0 ? 0 : (focus_pos > 1023 ? 1023 : focus_pos),
				};
				xioctl(lfd, VIDIOC_S_CTRL, &c);
				close(lfd);
				fprintf(stderr, "cam-stream: lens DAC=%d\n", c.value);
			}
		}
	}

	int enc_fd = -1;
	if (!out_nv12)
		enc_fd = enc_setup(enc_out_bufs, enc_cap_bufs, bitrate, target_fps);

	/* --out-nv12 fast path: V4L2 capture → bayer→NV12 → write directly
	 * to sink_fd. No Venus, no TCP server, no V4L2 m2m bookkeeping. Pipe
	 * to `ffmpeg -f rawvideo -pix_fmt nv12 -s 1920x1080 -r 30 -i -` and
	 * let ffmpeg do everything else (encode, mux, stream). */
	if (out_nv12) {
		uint8_t *nv12 = malloc((size_t)g_out_w * g_out_h * 3 / 2);
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
			/* Re-sync any live-tuning updates from the control thread
			 * before processing this frame; rebuild the gamma LUT if
			 * its inputs changed. */
			pthread_mutex_lock(&ctrl.mu);
			wb_r_q8 = ctrl.wb_r_q8;
			wb_b_q8 = ctrl.wb_b_q8;
			sat_q8  = ctrl.sat_q8;
			if (ctrl.rebuild_lut) {
				cam_pipeline_build_lut(gamma_lut, 256,
					ctrl.gamma_val, ctrl.contrast, ctrl.brightness);
				ctrl.rebuild_lut = 0;
			}
			pthread_mutex_unlock(&ctrl.mu);
			bayer_to_nv12(cap_bufs[cb.index].p, nv12,
				      cam_w, cam_h, cap_bytesperline,
				      g_out_w, g_out_h,
				      wb_r_q8, wb_b_q8, sat_q8);
			size_t left = (size_t)g_out_w * g_out_h * 3 / 2;
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
		cam_dev, cam_w, cam_h, g_out_w, g_out_h, bitrate);

	/* All enc OUTPUT buffers are free initially. We track that with a
	 * simple free-list: enc_out_free[i] = 1 means buffer i is available
	 * to be filled and queued. */
	int enc_out_free[ENC_OUT_BUFS];
	for (int i = 0; i < ENC_OUT_BUFS; i++) enc_out_free[i] = 1;

	double t0 = now_s();
	int frame_count = 0;
	uint64_t out_total = 0;
	/* Spawn the encode-drain thread that owns Venus CAPTURE + TCP write.
	 * Decoupling it from the capture/encode-submit loop is what lets the
	 * sensor side stay fed when Venus is contended serving the other
	 * cam-stream instance, or when mpv is briefly slow. */
	struct drain_ctx dctx = {
		.enc_fd = enc_fd,
		.sink_fd = sink_fd,
		.enc_cap_bufs = enc_cap_bufs,
		.n_cap_bufs = ENC_CAP_BUFS,
		.out_total = &out_total,
	};
	pthread_t drain_th;
	pthread_create(&drain_th, NULL, drain_thread, &dctx);

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
			{ .fd = enc_fd, .events = POLLOUT },
		};
		int pr = poll(pfds, 2, 1000);
		if (pr <= 0) {
			if (pr == 0) fprintf(stderr, "poll timeout\n");
			else fprintf(stderr, "poll: %s\n", strerror(errno));
			continue;
		}

		/* Reclaim finished enc OUTPUT buffers (Venus done reading the
		 * NV12 we queued). The drain thread owns the CAPTURE side. */
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

		/* Got a fresh bayer frame? Process it.
		 *
		 * If multiple frames have queued up (we fell behind sensor
		 * cadence), drop the older ones and only process the latest.
		 * This keeps the CAP ring populated so the VFE write-master
		 * always has somewhere to land — no more "Missing ready buf"
		 * starvation than absolutely necessary. */
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

			/* Drain any extra frames that piled up; keep only the
			 * newest one for processing. */
			for (;;) {
				struct v4l2_plane pl2 = {0};
				struct v4l2_buffer cb2 = {0};
				cb2.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
				cb2.memory = V4L2_MEMORY_MMAP;
				cb2.length = 1;
				cb2.m.planes = &pl2;
				if (xioctl(cap_fd, VIDIOC_DQBUF, &cb2) < 0) break;
				/* Stale frame — return it immediately. */
				struct v4l2_plane qpl = {0};
				cb.m.planes = &qpl;
				xioctl(cap_fd, VIDIOC_QBUF, &cb);
				cb = cb2;
				pl = pl2;
			}
			cb.m.planes = &pl;

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

			/* Pull any live tuning updates before this frame. */
			pthread_mutex_lock(&ctrl.mu);
			wb_r_q8 = ctrl.wb_r_q8;
			wb_b_q8 = ctrl.wb_b_q8;
			sat_q8  = ctrl.sat_q8;
			if (ctrl.rebuild_lut) {
				cam_pipeline_build_lut(gamma_lut, 256,
					ctrl.gamma_val, ctrl.contrast, ctrl.brightness);
				ctrl.rebuild_lut = 0;
			}
			pthread_mutex_unlock(&ctrl.mu);

			bayer_to_nv12(cap_bufs[cb.index].p,
				      enc_out_bufs[oi].p,
				      cam_w, cam_h, cap_bytesperline,
				      g_out_w, g_out_h,
				      wb_r_q8, wb_b_q8, sat_q8);

			/* Queue NV12 to encoder.
			 * Venus requires monotonic timestamps on input frames
			 * for its rate controller — without them it produces
			 * only one I-frame and goes silent. */
			struct v4l2_plane opl = {0};
			opl.bytesused = g_out_w * g_out_h * 3 / 2;
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
					"\rframes=%d  fps=%.1f  out=%llu KB  ",
					frame_count, frame_count / elapsed,
					(unsigned long long)(out_total / 1024));
			}
		}
	}

	running = 0;
	pthread_join(drain_th, NULL);

	int t;
	t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; xioctl(cap_fd, VIDIOC_STREAMOFF, &t);
	t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;  xioctl(enc_fd, VIDIOC_STREAMOFF, &t);
	t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; xioctl(enc_fd, VIDIOC_STREAMOFF, &t);
	close(cap_fd);
	close(enc_fd);
	fprintf(stderr,
		"\ncam-stream: done. frames=%d, %.1f fps avg, %llu KB output\n",
		frame_count, frame_count / (now_s() - t0),
		(unsigned long long)(out_total / 1024));
	return 0;
}

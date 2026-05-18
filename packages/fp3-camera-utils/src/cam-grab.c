/*
 * cam-grab — capture raw Bayer frames from a V4L2 multi-planar device,
 * unpack MIPI 10-bit packed Bayer (V4L2_PIX_FMT_SGRBG10P) into 10-bit
 * unpacked little-endian (V4L2_PIX_FMT_SGRBG10), and stream the bytes
 * to stdout in `video/x-bayer,format=grbg10le` order. Designed to feed
 * a GStreamer `fdsrc fd=0` pipeline; the bayer2rgbneon element then
 * demosaics with NEON acceleration.
 *
 * Usage: cam-grab <device> <width> <height> [count]
 *   count=0 means stream forever (until SIGPIPE / broken stdout).
 *
 * Why this exists: GStreamer 1.24's gst-plugins-good v4l2 plugin maps
 * only V4L2_PIX_FMT_SGRBG10 (unpacked) to `grbg10le`. The msm8953
 * CAMSS RDI lane only emits SGRBG10P. Bridging the two without
 * patching upstream is a 200-line C job — this one.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define NUM_BUFFERS 4

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && errno == EINTR);
	return r;
}

static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

/*
 * Unpack one row of SGRBG10P (5 bytes -> 4 pixels) to SGRBG10LE
 * (2 bytes per pixel, little-endian 16-bit container holding a 10-bit
 * value). `width` must be a multiple of 4 for the packed layout to be
 * well-defined; CAMSS picks widths that already satisfy this.
 */
static void unpack_row(const uint8_t *src, uint16_t *dst, unsigned int width)
{
	for (unsigned int x = 0; x < width; x += 4) {
		uint8_t lsbs = src[4];
		dst[0] = (uint16_t)(src[0] << 2) | (lsbs & 0x03);
		dst[1] = (uint16_t)(src[1] << 2) | ((lsbs >> 2) & 0x03);
		dst[2] = (uint16_t)(src[2] << 2) | ((lsbs >> 4) & 0x03);
		dst[3] = (uint16_t)(src[3] << 2) | ((lsbs >> 6) & 0x03);
		src += 5;
		dst += 4;
	}
}

int main(int argc, char **argv)
{
	if (argc < 4)
		die("usage: %s <device> <width> <height> [count]", argv[0]);

	const char *dev = argv[1];
	unsigned int width = (unsigned int)atoi(argv[2]);
	unsigned int height = (unsigned int)atoi(argv[3]);
	unsigned int count = (argc >= 5) ? (unsigned int)atoi(argv[4]) : 1;

	if (width == 0 || height == 0 || width % 4 != 0)
		die("width/height invalid (width must be a multiple of 4)");

	/*
	 * Don't crash on writes to a closed downstream pipe — just exit
	 * cleanly. gst-launch closing its fdsrc is the normal way for
	 * a stream to end.
	 */
	signal(SIGPIPE, SIG_IGN);

	int fd = open(dev, O_RDWR | O_NONBLOCK);
	if (fd < 0)
		die("open %s: %s", dev, strerror(errno));

	/* Set capture format: SGRBG10P, multi-planar. */
	struct v4l2_format fmt = {0};
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = width;
	fmt.fmt.pix_mp.height = height;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_SGRBG10P;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
		die("VIDIOC_S_FMT: %s", strerror(errno));

	unsigned int bytes_per_line = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	unsigned int frame_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
	if (bytes_per_line < width * 10 / 8)
		die("driver reported short bytes_per_line=%u for w=%u",
		    bytes_per_line, width);

	/* Request and mmap buffers. */
	struct v4l2_requestbuffers req = {0};
	req.count = NUM_BUFFERS;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0)
		die("VIDIOC_REQBUFS: %s", strerror(errno));

	struct {
		void *start;
		size_t length;
	} bufs[NUM_BUFFERS];

	for (unsigned int i = 0; i < req.count; i++) {
		struct v4l2_plane planes[1] = {0};
		struct v4l2_buffer b = {0};
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.length = 1;
		b.m.planes = planes;
		if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0)
			die("VIDIOC_QUERYBUF[%u]: %s", i, strerror(errno));

		bufs[i].length = planes[0].length;
		bufs[i].start = mmap(NULL, planes[0].length,
				     PROT_READ | PROT_WRITE, MAP_SHARED,
				     fd, planes[0].m.mem_offset);
		if (bufs[i].start == MAP_FAILED)
			die("mmap[%u]: %s", i, strerror(errno));
	}

	/* Queue all buffers. */
	for (unsigned int i = 0; i < req.count; i++) {
		struct v4l2_plane planes[1] = {0};
		struct v4l2_buffer b = {0};
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.length = 1;
		b.m.planes = planes;
		if (xioctl(fd, VIDIOC_QBUF, &b) < 0)
			die("VIDIOC_QBUF[%u]: %s", i, strerror(errno));
	}

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0)
		die("VIDIOC_STREAMON: %s", strerror(errno));

	uint16_t *row = malloc(width * sizeof(*row));
	if (!row)
		die("malloc row");

	unsigned int captured = 0;
	while (count == 0 || captured < count) {
		/*
		 * Block on the device fd via poll instead of busy looping.
		 * V4L2 sets POLLIN when a buffer is ready to be dequeued.
		 */
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr;
		do {
			pr = poll(&pfd, 1, -1);
		} while (pr == -1 && errno == EINTR);
		if (pr < 0)
			die("poll: %s", strerror(errno));

		struct v4l2_plane planes[1] = {0};
		struct v4l2_buffer b = {0};
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.length = 1;
		b.m.planes = planes;
		if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
			if (errno == EAGAIN)
				continue;
			die("VIDIOC_DQBUF: %s", strerror(errno));
		}

		const uint8_t *base = bufs[b.index].start;
		for (unsigned int y = 0; y < height; y++) {
			unpack_row(base + y * bytes_per_line, row, width);
			ssize_t n = fwrite(row, sizeof(*row), width, stdout);
			if (n != (ssize_t)width) {
				/* downstream closed — clean exit */
				goto done;
			}
		}
		fflush(stdout);

		if (xioctl(fd, VIDIOC_QBUF, &b) < 0)
			die("VIDIOC_QBUF (recycle): %s", strerror(errno));

		captured++;
	}

done:
	xioctl(fd, VIDIOC_STREAMOFF, &type);
	for (unsigned int i = 0; i < req.count; i++)
		munmap(bufs[i].start, bufs[i].length);
	free(row);
	close(fd);
	(void)frame_size;
	return 0;
}

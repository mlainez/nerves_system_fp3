// SPDX-License-Identifier: MIT
//
// rumble — play one force-feedback rumble effect on the first input device
// that supports FF_RUMBLE (on the Fairphone 3: the PMI632 vibrator).
//
//   rumble [-d /dev/input/eventN] DURATION_MS [STRENGTH]
//
// STRENGTH is 1..65535 (default 65535). Exits after the effect has played.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define BITS_PER_LONG (sizeof(long) * 8)
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(bit, arr) (((arr)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1)

static int supports_rumble(int fd)
{
	unsigned long ff[NLONGS(FF_CNT)] = {0};

	if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff)), ff) < 0)
		return 0;
	return TEST_BIT(FF_RUMBLE, ff);
}

static int open_rumble_device(char *path, size_t len)
{
	DIR *dir = opendir("/dev/input");
	struct dirent *de;
	int fd = -1;

	if (!dir)
		return -1;
	while ((de = readdir(dir))) {
		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(path, len, "/dev/input/%s", de->d_name);
		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;
		if (supports_rumble(fd))
			break;
		close(fd);
		fd = -1;
	}
	closedir(dir);
	return fd;
}

int main(int argc, char **argv)
{
	char path[300] = "";
	const char *dev = NULL;
	int opt, fd;
	long ms, strength = 65535;

	while ((opt = getopt(argc, argv, "d:h")) != -1) {
		if (opt == 'd') {
			dev = optarg;
		} else {
			fprintf(stderr, "usage: %s [-d /dev/input/eventN] DURATION_MS [STRENGTH]\n", argv[0]);
			return opt == 'h' ? 0 : 2;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "usage: %s [-d /dev/input/eventN] DURATION_MS [STRENGTH]\n", argv[0]);
		return 2;
	}
	ms = strtol(argv[optind], NULL, 10);
	if (optind + 1 < argc)
		strength = strtol(argv[optind + 1], NULL, 10);
	if (ms < 1 || ms > 10000 || strength < 1 || strength > 65535) {
		fprintf(stderr, "rumble: DURATION_MS must be 1..10000 and STRENGTH 1..65535\n");
		return 2;
	}

	if (dev) {
		snprintf(path, sizeof(path), "%s", dev);
		fd = open(path, O_RDWR);
		if (fd >= 0 && !supports_rumble(fd)) {
			fprintf(stderr, "rumble: %s does not support FF_RUMBLE\n", path);
			return 1;
		}
	} else {
		fd = open_rumble_device(path, sizeof(path));
	}
	if (fd < 0) {
		fprintf(stderr, "rumble: no input device with FF_RUMBLE found\n");
		return 1;
	}

	struct ff_effect effect = {
		.type = FF_RUMBLE,
		.id = -1,
		.replay = { .length = (unsigned short)ms, .delay = 0 },
		.u.rumble = { .strong_magnitude = (unsigned short)strength,
			      .weak_magnitude = (unsigned short)strength },
	};
	if (ioctl(fd, EVIOCSFF, &effect) < 0) {
		fprintf(stderr, "rumble: upload effect on %s: %s\n", path, strerror(errno));
		return 1;
	}

	struct input_event play = { .type = EV_FF, .code = effect.id, .value = 1 };
	if (write(fd, &play, sizeof(play)) != sizeof(play)) {
		fprintf(stderr, "rumble: play effect: %s\n", strerror(errno));
		return 1;
	}

	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);

	ioctl(fd, EVIOCRMFF, effect.id);
	close(fd);
	return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <stdint.h>

/*
 * i2cprobe <bus> [scan|addr] [reg] [value]
 *   scan: walk 0x08..0x77, send a 0-byte write to each, print which ack
 *   addr [reg]: read 1 byte from address `addr` register `reg`
 *   addr [reg] [value]: i2c_master_send write of [reg, value]
 *
 * The write path matches what the ak7375 driver does via
 * i2c_master_send — same controller path, same atomicity.
 */

static int raw_write(int fd, int addr, uint8_t reg, uint8_t val)
{
	if (ioctl(fd, I2C_SLAVE, addr) < 0) return -errno;
	unsigned char buf[2] = { reg, val };
	if (write(fd, buf, 2) != 2) return -errno;
	return 0;
}

/* 16-bit value (big-endian on the wire, matches ak7375 driver). */
static int raw_write16(int fd, int addr, uint8_t reg, uint16_t val16)
{
	if (ioctl(fd, I2C_SLAVE, addr) < 0) return -errno;
	unsigned char buf[3] = { reg, (val16 >> 8) & 0xff, val16 & 0xff };
	if (write(fd, buf, 3) != 3) return -errno;
	return 0;
}

static int smbus_read_byte_data(int fd, int reg)
{
	struct i2c_smbus_ioctl_data ioargs;
	union i2c_smbus_data data;
	data.byte = 0;
	ioargs.read_write = I2C_SMBUS_READ;
	ioargs.command = reg;
	ioargs.size = I2C_SMBUS_BYTE_DATA;
	ioargs.data = &data;
	if (ioctl(fd, I2C_SMBUS, &ioargs) < 0) return -errno;
	return data.byte;
}

static int probe_addr(int fd, int addr)
{
	struct i2c_smbus_ioctl_data ioargs;
	union i2c_smbus_data data;
	if (ioctl(fd, I2C_SLAVE, addr) < 0) return -errno;
	/* I2C_SMBUS_QUICK = 0-byte write with start+ack only */
	ioargs.read_write = I2C_SMBUS_WRITE;
	ioargs.command = 0;
	ioargs.size = I2C_SMBUS_QUICK;
	ioargs.data = NULL;
	if (ioctl(fd, I2C_SMBUS, &ioargs) < 0) return -errno;
	return 0;
}

/*
 * Set a TLMM GPIO line high or low and HOLD it (the kernel ABI releases
 * the line as soon as we close the fd, so we sleep before exiting if
 * `hold_ms` > 0 to give downstream consumers time to do their thing).
 */
static int gpio_set(const char *chip, int line, int val, int hold_ms)
{
	int fd = open(chip, O_RDWR);
	if (fd < 0) { perror(chip); return 1; }
	struct gpiohandle_request req = {0};
	req.lineoffsets[0] = line;
	req.lines = 1;
	req.flags = GPIOHANDLE_REQUEST_OUTPUT;
	req.default_values[0] = !!val;
	snprintf(req.consumer_label, sizeof(req.consumer_label), "i2cprobe-vcm");
	if (ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
		perror("GPIO_GET_LINEHANDLE");
		close(fd);
		return 1;
	}
	if (hold_ms > 0)
		usleep(hold_ms * 1000);
	close(req.fd);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <bus-N> scan|<addr> [reg] [value]\n"
			        "       %s gpio <chip> <line> <0|1> [hold-ms]\n",
			argv[0], argv[0]);
		return 1;
	}

	if (!strcmp(argv[1], "gpio")) {
		if (argc < 5) { fprintf(stderr, "gpio needs chip line val\n"); return 1; }
		int hold = argc > 5 ? atoi(argv[5]) : 0;
		return gpio_set(argv[2], atoi(argv[3]), atoi(argv[4]), hold);
	}

	char path[64];
	snprintf(path, sizeof(path), "/dev/i2c-%d", atoi(argv[1]));
	int fd = open(path, O_RDWR);
	if (fd < 0) { perror(path); return 1; }

	if (!strcmp(argv[2], "scan")) {
		printf("Scanning %s — addresses that ACK:\n", path);
		for (int a = 0x08; a <= 0x77; a++) {
			if (probe_addr(fd, a) == 0)
				printf("  0x%02x\n", a);
		}
	} else {
		int addr = strtol(argv[2], NULL, 0);
		int reg = argc > 3 ? strtol(argv[3], NULL, 0) : 0;
		if (argc > 4) {
			long val = strtol(argv[4], NULL, 0);
			int r;
			if (val > 0xff) {
				/* 16-bit write — big-endian, 3 byte transfer */
				r = raw_write16(fd, addr, reg, (uint16_t)val);
				if (r < 0) {
					fprintf(stderr, "write16 0x%02x reg 0x%02x = 0x%04lx failed: %s\n",
						addr, reg, val, strerror(-r));
					return 1;
				}
				printf("wrote16 0x%04lx to 0x%02x reg 0x%02x\n", val, addr, reg);
			} else {
				r = raw_write(fd, addr, reg, (uint8_t)val);
				if (r < 0) {
					fprintf(stderr, "write 0x%02x reg 0x%02x = 0x%02lx failed: %s\n",
						addr, reg, val, strerror(-r));
					return 1;
				}
				printf("wrote 0x%02lx to 0x%02x reg 0x%02x\n", val, addr, reg);
			}
		} else {
			if (ioctl(fd, I2C_SLAVE, addr) < 0) {
				perror("I2C_SLAVE");
				return 1;
			}
			int r = smbus_read_byte_data(fd, reg);
			if (r < 0) {
				fprintf(stderr, "read 0x%02x reg 0x%02x failed: %s\n",
					addr, reg, strerror(-r));
				return 1;
			}
			printf("0x%02x reg 0x%02x = 0x%02x\n", addr, reg, r);
		}
	}
	close(fd);
	return 0;
}

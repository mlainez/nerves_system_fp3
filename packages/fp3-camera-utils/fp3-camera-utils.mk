################################################################################
#
# fp3-camera-utils
#
# Fairphone 3+ camera tooling: stills (cam-snap), live H.264 stream
# (cam-stream), raw Bayer capture (cam-grab), and an i2c bus probe.
# All four target the msm8953 CAMSS pipeline and the FP3+'s specific
# sensor map; not useful on other boards.
#
################################################################################

FP3_CAMERA_UTILS_VERSION = 0.8.1
FP3_CAMERA_UTILS_SITE = $(NERVES_DEFCONFIG_DIR)/packages/fp3-camera-utils/src
FP3_CAMERA_UTILS_SITE_METHOD = local
FP3_CAMERA_UTILS_LICENSE = MIT
FP3_CAMERA_UTILS_DEPENDENCIES = bayer2rgb-neon jpeg

define FP3_CAMERA_UTILS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-O3 -ffast-math \
		-I$(STAGING_DIR)/usr/include \
		-o $(@D)/cam-snap $(@D)/cam-snap.c \
		-L$(STAGING_DIR)/usr/lib \
		-lbayer2rgb3 -ljpeg -lm
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-O3 -ffast-math -march=armv8-a+simd \
		-I$(STAGING_DIR)/usr/include \
		-o $(@D)/cam-stream $(@D)/cam-stream.c \
		-L$(STAGING_DIR)/usr/lib \
		-lm -lpthread
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/cam-grab $(@D)/cam-grab.c
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/i2cprobe $(@D)/i2cprobe.c
endef

define FP3_CAMERA_UTILS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/cam-snap      $(TARGET_DIR)/usr/bin/cam-snap
	$(INSTALL) -D -m 0755 $(@D)/cam-stream    $(TARGET_DIR)/usr/bin/cam-stream
	$(INSTALL) -D -m 0755 $(@D)/cam-grab      $(TARGET_DIR)/usr/bin/cam-grab
	$(INSTALL) -D -m 0755 $(@D)/i2cprobe      $(TARGET_DIR)/usr/bin/i2cprobe
	$(INSTALL) -D -m 0755 $(@D)/fp3-cam-setup.sh $(TARGET_DIR)/usr/bin/fp3-cam-setup
endef

$(eval $(generic-package))

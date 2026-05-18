################################################################################
#
# cam-snap
#
################################################################################

CAM_SNAP_VERSION = 0.1.0
CAM_SNAP_SITE = $(NERVES_DEFCONFIG_DIR)/packages/cam-snap/src
CAM_SNAP_SITE_METHOD = local
CAM_SNAP_LICENSE = MIT
CAM_SNAP_DEPENDENCIES = bayer2rgb-neon jpeg

define CAM_SNAP_BUILD_CMDS
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
		-lm
endef

define CAM_SNAP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/cam-snap $(TARGET_DIR)/usr/bin/cam-snap
	$(INSTALL) -D -m 0755 $(@D)/cam-stream $(TARGET_DIR)/usr/bin/cam-stream
	$(INSTALL) -D -m 0755 $(@D)/fp3-cam-setup.sh $(TARGET_DIR)/usr/bin/fp3-cam-setup
endef

$(eval $(generic-package))

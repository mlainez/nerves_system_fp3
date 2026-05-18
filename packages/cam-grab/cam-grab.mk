################################################################################
#
# cam-grab
#
################################################################################

CAM_GRAB_VERSION = 0.1.0
CAM_GRAB_SITE = $(NERVES_DEFCONFIG_DIR)/packages/cam-grab/src
CAM_GRAB_SITE_METHOD = local
CAM_GRAB_LICENSE = MIT

define CAM_GRAB_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/cam-grab $(@D)/cam-grab.c
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/i2cprobe $(@D)/i2cprobe.c
endef

define CAM_GRAB_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/cam-grab $(TARGET_DIR)/usr/bin/cam-grab
	$(INSTALL) -D -m 0755 $(@D)/i2cprobe $(TARGET_DIR)/usr/bin/i2cprobe
endef

$(eval $(generic-package))

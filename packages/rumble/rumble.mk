################################################################################
#
# rumble
#
################################################################################

RUMBLE_LICENSE = MIT
RUMBLE_SITE = $(NERVES_DEFCONFIG_DIR)/packages/rumble
RUMBLE_SITE_METHOD = local

define RUMBLE_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/rumble.c -o $(@D)/rumble
endef

define RUMBLE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/rumble $(TARGET_DIR)/usr/bin/rumble
endef

$(eval $(generic-package))

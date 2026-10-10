include $(sort $(wildcard $(NERVES_DEFCONFIG_DIR)/packages/*/*.mk))

# The Qualcomm camss pipeline handler, carried in patches/libcamera
LIBCAMERA_PIPELINES-y += camss

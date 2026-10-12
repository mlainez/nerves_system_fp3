#!/bin/sh

set -e

# Build the boot.img that the bootloader reads from the boot subpartition.
$NERVES_DEFCONFIG_DIR/scripts/create-boot-img.sh

# Pack U-Boot for the boot partition of the phone.
$NERVES_DEFCONFIG_DIR/scripts/create-uboot-img.sh

FWUP_CONFIG=$NERVES_DEFCONFIG_DIR/fwup.conf

# Hand off to nerves-common to bundle the .fw / .img archives.
$BR2_EXTERNAL_NERVES_PATH/board/nerves-common/post-createfs.sh $TARGET_DIR $FWUP_CONFIG

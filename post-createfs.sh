#!/bin/sh

set -e

# Build the boot.img that lk2nd-msm8953 reads from the boot subpartition.
$NERVES_DEFCONFIG_DIR/scripts/create-boot-img.sh

FWUP_CONFIG=$NERVES_DEFCONFIG_DIR/fwup.conf

# Hand off to nerves-common to bundle the .fw / .img archives.
$BR2_EXTERNAL_NERVES_PATH/board/nerves-common/post-createfs.sh $TARGET_DIR $FWUP_CONFIG

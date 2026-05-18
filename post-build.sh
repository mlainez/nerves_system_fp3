#!/bin/sh

set -e

# Build the fwup operations archive (status / factory-reset / revert /
# validate). Ends up on target at /usr/share/fwup/ops.fw.
mkdir -p $TARGET_DIR/usr/share/fwup
$HOST_DIR/usr/bin/fwup -c \
    -f $NERVES_DEFCONFIG_DIR/fwup-ops.conf \
    -o $TARGET_DIR/usr/share/fwup/ops.fw
ln -sf ops.fw $TARGET_DIR/usr/share/fwup/revert.fw

# Make the fwup includes resolvable next to the rootfs image.
cp -rf $NERVES_DEFCONFIG_DIR/fwup_include $BINARIES_DIR

# libcamera ships a small test CLI (`cam`) but the buildroot package
# doesn't install it (auto_features=disabled). It's already built in
# the libcamera output dir, so we drop it into /usr/bin manually —
# zero rebuild cost. Useful as a sanity check of libcamera's pipeline
# discovery + SoftwareISP on the device.
CAM_BIN=$(find $BUILD_DIR/libcamera-*/buildroot-build/src/apps -name cam -type f -executable 2>/dev/null | head -1)
if [ -n "$CAM_BIN" ] && [ -f "$CAM_BIN" ]; then
    cp -f "$CAM_BIN" $TARGET_DIR/usr/bin/lc-cam
    chmod 0755 $TARGET_DIR/usr/bin/lc-cam
fi

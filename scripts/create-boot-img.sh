#!/bin/sh
#
# create-boot-img.sh
#
# Builds the ext2 "boot" image that lk2nd-msm8953 reads at boot.
# Layout inside the image:
#
#   /Image                          — kernel
#   /sdm632-fairphone-fp3*.dtb      — device tree(s)
#   /initramfs.gz                   — citronics initramfs (cpio.gz)
#   /extlinux/extlinux.conf         — bootloader menu
#
# Inputs (env, from Buildroot's post-image stage):
#   $BINARIES_DIR        — buildroot output/images/
#   $NERVES_DEFCONFIG_DIR — this nerves_system_fp3 source tree
#
# Output:
#   $BINARIES_DIR/boot.img          — referenced from fwup.conf

set -e

STAGING="$BINARIES_DIR/boot"
rm -rf "$STAGING"
mkdir -p "$STAGING/extlinux"

cp "$NERVES_DEFCONFIG_DIR/extlinux/extlinux.conf" "$STAGING/extlinux/extlinux.conf"

# Kernel image (aarch64 builds drop `Image`; legacy zImage-style boards
# would drop `zImage` instead — we copy whichever is present).
for k in Image zImage; do
    if [ -f "$BINARIES_DIR/$k" ]; then
        cp "$BINARIES_DIR/$k" "$STAGING/$k"
    fi
done

# Device-tree blob(s) for the Fairphone 3 / 3+.
for dtb in "$BINARIES_DIR"/sdm632-fairphone-fp3*.dtb; do
    [ -f "$dtb" ] && cp "$dtb" "$STAGING/$(basename "$dtb")"
done

# citronics initramfs (built by the citronics-initramfs buildroot
# package; lands here as initramfs.gz).
if [ -f "$BINARIES_DIR/initramfs.gz" ]; then
    cp "$BINARIES_DIR/initramfs.gz" "$STAGING/initramfs.gz"
else
    echo "create-boot-img.sh: WARNING: no initramfs.gz in $BINARIES_DIR — booting without it will hang at rootfs mount." >&2
fi

# 1024-byte blocks * 102400 = 100 MiB; must stay ≤ BOOT_A_PART_COUNT
# (204800 blocks of 512 B each = 100 MiB) defined in fwup-common.conf.
genext2fs -B 1024 -b 102400 -d "$STAGING" "$BINARIES_DIR/boot.img"

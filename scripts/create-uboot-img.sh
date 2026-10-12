#!/bin/sh
#
# create-uboot-img.sh
#
# Packs U-Boot into the Android boot image that the stock LK loads from the
# boot partition: gzipped U-Boot with its device tree appended, a one byte
# ramdisk, 2 KiB pages, base 0x80000000.
#
# Inputs (env, from Buildroot's post-image stage):
#   $BINARIES_DIR         — buildroot output/images/ (u-boot-nodtb.bin, u-boot.dtb)
#   $NERVES_DEFCONFIG_DIR — this nerves_system_fp3 source tree
#
# Output:
#   $BINARIES_DIR/u-boot-fp3.img — flash with `fastboot flash boot`

set -e

WORK="$BINARIES_DIR/u-boot-img"
rm -rf "$WORK"
mkdir -p "$WORK"

gzip -9nc "$BINARIES_DIR/u-boot-nodtb.bin" > "$WORK/u-boot-nodtb.bin.gz"
cat "$WORK/u-boot-nodtb.bin.gz" "$BINARIES_DIR/u-boot.dtb" > "$WORK/kernel"
printf '\0' > "$WORK/ramdisk"

python3 "$NERVES_DEFCONFIG_DIR/scripts/mkbootimg-v0.py" \
	--kernel "$WORK/kernel" \
	--ramdisk "$WORK/ramdisk" \
	--base 0x80000000 \
	--pagesize 2048 \
	--output "$BINARIES_DIR/u-boot-fp3.img"

rm -rf "$WORK"

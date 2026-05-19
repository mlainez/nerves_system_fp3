################################################################################
#
# citronics-initramfs (Nerves-flavoured: source paths adjusted to live under
# $(NERVES_DEFCONFIG_DIR), deviceinfo + modules list shipped inside this
# package rather than under a $(BR2_EXTERNAL)/board/<name>/overlay directory)
#
################################################################################

CITRONICS_INITRAMFS_VERSION = 6e058d7
CITRONICS_INITRAMFS_SITE = https://github.com/Citronics/initramfs.git
CITRONICS_INITRAMFS_SITE_METHOD = git
CITRONICS_INITRAMFS_LICENSE_FILES = LICENSE

CITRONICS_INITRAMFS_DEPENDENCIES = busybox unudhcpd multipath-tools parted

CITRONICS_INITRAMFS_STAGING = $(@D)/initramfs-root
CITRONICS_INITRAMFS_SRC_DIR = initramfs/usr/share/citronics-initramfs
CITRONICS_INITRAMFS_PKG_DIR = $(NERVES_DEFCONFIG_DIR)/packages/citronics-initramfs
# Only the non-busybox programs the upstream init/functions.sh actually
# invoke: kpartx (subpartition mapping), parted (resize_rootfs path,
# optional), unudhcpd (USB-recovery DHCP). Everything else — udevd,
# dmsetup, resize2fs, blkid, sfdisk, lsblk, partprobe, kmod, e2fsck —
# is provided by busybox or simply not called, and was previously
# bloating the cpio.gz past lk2nd's max-initrd ceiling.
CITRONICS_BINARIES = busybox kpartx parted unudhcpd

KERNEL_VERSION = $(shell find $(TARGET_DIR)/lib/modules -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | head -n1)

define CITRONICS_INITRAMFS_BUILD_CMDS
	true
endef

define CITRONICS_INITRAMFS_INSTALL_TARGET_CMDS
	mkdir -p $(CITRONICS_INITRAMFS_STAGING)
	mkdir -p $(CITRONICS_INITRAMFS_STAGING)/{bin,sbin,usr/bin,usr/sbin,lib,etc,proc,sys,dev,tmp,var,run,mnt,root,sysroot}
	chmod 1777 $(CITRONICS_INITRAMFS_STAGING)/tmp
	chmod 1777 $(CITRONICS_INITRAMFS_STAGING)/run

	# CRITICAL: the Nerves aarch64 toolchain's ld-linux.so was built with
	# /lib64 + /usr/lib64 as its compiled-in trusted search paths
	# (`strings ld-linux-aarch64.so.1 | grep '^/lib'` shows /lib64/ and
	# /usr/lib64/). The buildroot target skeleton ships symlinks lib64 ->
	# lib + usr/lib64 -> lib, but the initramfs builder doesn't inherit
	# them. Without these symlinks ld.so cannot find libc.so.6, every
	# dynamic binary in the initramfs exits 127 ("error while loading
	# shared libraries: libc.so.6: cannot open shared object file"),
	# the kernel sees PID 1 exit 127, and panics with
	# `Attempted to kill init! exitcode=0x00007f00` ~1ms after handoff.
	ln -sf lib $(CITRONICS_INITRAMFS_STAGING)/lib64
	ln -sf lib $(CITRONICS_INITRAMFS_STAGING)/usr/lib64

	# Copy the citronics-initramfs scripts into the initramfs root.
	cp -aL $(@D)/$(CITRONICS_INITRAMFS_SRC_DIR)/* $(CITRONICS_INITRAMFS_STAGING)/
	mkdir -p $(CITRONICS_INITRAMFS_STAGING)/usr/share/deviceinfo
	cp -RaL $(CITRONICS_INITRAMFS_STAGING)/misc $(CITRONICS_INITRAMFS_STAGING)/usr/share/misc
	# This Nerves system carries deviceinfo inside the package directory —
	# there is no buildroot board overlay to inherit from.
	$(INSTALL) -D -m 0644 $(CITRONICS_INITRAMFS_PKG_DIR)/deviceinfo \
		$(CITRONICS_INITRAMFS_STAGING)/usr/share/deviceinfo/deviceinfo

	# Overwrite the upstream initramfs.load — it lists FP2-only modules
	# (pm8941_pwrkey, ili210x) that don't exist on the FP3 kernel and just
	# produce noisy modprobe failures. The FP3+ panel / touchscreen / power
	# key drivers are =y in our kernel config and probe via DT, so the
	# initramfs has nothing to modprobe.
	: > $(CITRONICS_INITRAMFS_STAGING)/lib/modules/initramfs.load

	mkdir -p $(CITRONICS_INITRAMFS_STAGING)/lib/modules/$(KERNEL_VERSION)
	@echo "Detected kernel version: $(KERNEL_VERSION)"
	@echo "Copying kernel modules listed in $(CITRONICS_INITRAMFS_PKG_DIR)/modules..."

	while read -r modpath; do \
		case "$$modpath" in ""|\#*) continue;; esac; \
		src="$(TARGET_DIR)/lib/modules/$(KERNEL_VERSION)/$$modpath"; \
		dest="$(CITRONICS_INITRAMFS_STAGING)/lib/modules/$(KERNEL_VERSION)/$$modpath"; \
		if [ -f "$$src" ]; then \
			mkdir -p "$$(dirname "$$dest")"; \
			cp -aL "$$src" "$$dest"; \
		else \
			echo "  warn: module $$modpath not present in target — skipping"; \
		fi; \
	done < $(CITRONICS_INITRAMFS_PKG_DIR)/modules

	@echo "Copying required binaries from target to initramfs..."
	for bin in $(CITRONICS_BINARIES); do \
		src=""; \
		for dir in usr/sbin sbin usr/bin bin; do \
			candidate="$(TARGET_DIR)/$$dir/$$bin"; \
			if [ -x "$$candidate" ]; then \
				src="$$candidate"; \
				break; \
			fi; \
		done; \
		if [ -z "$$src" ]; then \
			echo "  warn: binary $$bin not found in target"; \
		else \
			relpath=$$(realpath --relative-to=$(TARGET_DIR) $$src); \
			dest="$(CITRONICS_INITRAMFS_STAGING)/$$relpath"; \
			mkdir -p "$$(dirname $$dest)"; \
			cp -aL "$$src" "$$dest"; \
			$(CITRONICS_INITRAMFS_PKG_DIR)/scripts/copy_libs_for_binary.sh \
				$$src $(TARGET_DIR) $(CITRONICS_INITRAMFS_STAGING) $(TARGET_READELF) ; \
		fi; \
	done

	cd $(CITRONICS_INITRAMFS_STAGING)/bin && \
		ln -sf busybox sh && \
		ln -sf busybox telnetd && \
		ln -sf busybox getty

	# Bake the Adreno GPU firmware into the initramfs. The in-tree
	# msm DRM driver probes 1c00000.gpu at ~2.7s, BEFORE pivot_root
	# from the initramfs into the squashfs rootfs. adreno_request_fw()
	# uses request_firmware_direct() — no wait, no retry — so if the
	# files aren't visible at probe time, the GPU is dead for the
	# rest of the boot.
	#
	# Generic across Adreno generations:
	#   * a3xx on FP2 (Adreno 305): a300_pm4.fw, a300_pfp.fw, a300_zap.*
	#   * a5xx on FP3+ (Adreno 506): a530_pm4.fw, a530_pfp.fw,
	#     a530v3_gpmu.fw2, a506_zap.* (under the DT firmware-name
	#     subpath qcom/<soc>/<vendor>/<board>/)
	#   * a6xx / a7xx future targets: a*_sqe.fw, a*_gmu.bin
	#
	# Pick up whatever's installed under target/lib/firmware/qcom/
	# matching the Adreno-firmware naming patterns. Skip the modem /
	# ADSP / WCNSS / Venus blobs — those are loaded by remoteproc
	# after pivot_root and don't need to be in the initramfs.
	if [ -d "$(TARGET_DIR)/lib/firmware/qcom" ]; then \
		mkdir -p "$(CITRONICS_INITRAMFS_STAGING)/lib/firmware/qcom"; \
		cd "$(TARGET_DIR)/lib/firmware/qcom" && \
		find . \( -name 'a*_pm4.fw*' \
		       -o -name 'a*_pfp.fw*' \
		       -o -name 'a*_sqe.fw*' \
		       -o -name 'a*_gpmu.fw*' \
		       -o -name 'a*_gmu.bin' \
		       -o -name 'a*_zap.mdt' \
		       -o -name 'a*_zap.mbn' \
		       -o -name 'a*_zap.elf' \
		       -o -name 'a*_zap.b*' \) -print | \
		while read f; do \
			dest="$(CITRONICS_INITRAMFS_STAGING)/lib/firmware/qcom/$$f"; \
			mkdir -p "$$(dirname "$$dest")"; \
			cp -aL "$$f" "$$dest"; \
		done; \
	fi

	# The upstream init uses busybox's `mdev` for device-node hotplug,
	# not full udev — so we deliberately do not copy /usr/lib/udev.
	# Pulling it in dragged in ~19 MiB of libcrypto via udev's
	# dependency chain, blowing past lk2nd's initrd size limit.

	# CPIO -> gzip; nerves-common's post-createfs pipeline picks the
	# resulting initramfs.gz out of $(BINARIES_DIR).
	(cd $(CITRONICS_INITRAMFS_STAGING) && \
		find . | cpio -o -H newc --owner root:root > ../initramfs.cpio)
	gzip -f $(@D)/initramfs.cpio
	cp $(@D)/initramfs.cpio.gz $(BINARIES_DIR)/initramfs.gz
endef

$(eval $(generic-package))

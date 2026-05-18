################################################################################
#
# qbootctl
#
################################################################################

QBOOTCTL_VERSION = 0.2.2
QBOOTCTL_SITE = $(call github,linux-msm,qbootctl,$(QBOOTCTL_VERSION))
QBOOTCTL_DEPENDENCIES = zlib
QBOOTCTL_LICENSE = BSD-3-Clause

$(eval $(meson-package))

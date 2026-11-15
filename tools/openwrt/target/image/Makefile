# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

# Root filesystem archives only; no kernel or flashable device image.
define Image/Build
	$(CP) $(KDIR)/root.$(1) $(BIN_DIR)/$(IMG_ROOTFS).$(1)
endef

$(eval $(call BuildImage))

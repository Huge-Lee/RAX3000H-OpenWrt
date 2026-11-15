include $(TOPDIR)/rules.mk

PKG_NAME:=rax-wifi-scan
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/rax-wifi-scan
  SECTION:=net
  CATEGORY:=Network
  TITLE:=RAX3000H startup channel selection
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Configure
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -o $(PKG_BUILD_DIR)/rax-wifi-scan $(PKG_BUILD_DIR)/main.c $(TARGET_LDFLAGS)
endef

define Package/rax-wifi-scan/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/rax-wifi-scan $(1)/usr/sbin/
endef

$(eval $(call BuildPackage,rax-wifi-scan))

# SPDX-License-Identifier: MIT
include $(TOPDIR)/rules.mk

PKG_NAME:=spectrald
PKG_VERSION:=1.0
PKG_RELEASE:=3

PKG_MAINTAINER:=fmu
PKG_LICENSE:=MIT

include $(INCLUDE_DIR)/package.mk

define Package/spectrald
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=ath10k spectral-scan FFT daemon (LuCI backend)
  URL:=https://github.com/fmu/owrt-spectral
  DEPENDS:=+libubus +libubox +libblobmsg-json
endef

define Package/spectrald/description
  Reads the ath10k spectral-scan FFT stream from debugfs, converts it to
  per-bin dBm and serves a live waterfall to LuCI over a ubus object.
  Requires a kernel with CONFIG_ATH10K_SPECTRAL (CONFIG_PACKAGE_ATH_SPECTRAL
  in OpenWrt) and CONFIG_RELAY.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) \
		-o $(PKG_BUILD_DIR)/spectrald $(PKG_BUILD_DIR)/spectrald.c \
		$(TARGET_LDFLAGS) -lubus -lubox -lblobmsg_json -lm
endef

define Package/spectrald/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/spectrald $(1)/usr/sbin/spectrald
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/spectrald.init $(1)/etc/init.d/spectrald
endef

define Package/spectrald/postinst
#!/bin/sh
if [ -z "$${IPKG_INSTROOT}" ]; then
	/etc/init.d/spectrald enable
	/etc/init.d/spectrald restart
fi
endef

define Package/spectrald/prerm
#!/bin/sh
if [ -z "$${IPKG_INSTROOT}" ]; then
	/etc/init.d/spectrald stop
	/etc/init.d/spectrald disable
fi
endef

$(eval $(call BuildPackage,spectrald))

# SPDX-License-Identifier: MIT
include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-spectral
PKG_VERSION:=1.0
PKG_RELEASE:=4

PKG_MAINTAINER:=fmu
PKG_LICENSE:=MIT

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-spectral
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=Status
  TITLE:=Wi-Fi spectrum analyzer (ath10k FFT waterfall)
  DEPENDS:=+luci-base +spectrald
  PKGARCH:=all
endef

define Package/luci-app-spectral/description
  LuCI view for the spectrald daemon: live FFT waterfall and max-hold
  spectrum of the current ath10k channel.
endef

define Build/Compile
endef

define Package/luci-app-spectral/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/spectral
	$(INSTALL_DATA) ./htdocs/luci-static/resources/view/spectral/waterfall.js \
		$(1)/www/luci-static/resources/view/spectral/waterfall.js
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) ./root/usr/share/luci/menu.d/luci-app-spectral.json \
		$(1)/usr/share/luci/menu.d/luci-app-spectral.json
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./root/usr/share/rpcd/acl.d/luci-app-spectral.json \
		$(1)/usr/share/rpcd/acl.d/luci-app-spectral.json
endef

define Package/luci-app-spectral/postinst
#!/bin/sh
[ -z "$${IPKG_INSTROOT}" ] || exit 0
/etc/init.d/rpcd reload 2>/dev/null || true
rm -f /tmp/luci-indexcache
rm -rf /tmp/luci-modulecache
exit 0
endef

define Package/luci-app-spectral/prerm
#!/bin/sh
[ -z "$${IPKG_INSTROOT}" ] || exit 0
/etc/init.d/rpcd reload 2>/dev/null || true
rm -f /tmp/luci-indexcache
rm -rf /tmp/luci-modulecache
exit 0
endef

$(eval $(call BuildPackage,luci-app-spectral))

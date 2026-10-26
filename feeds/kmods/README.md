# Spectral-enabled ath10k kmod feed

This directory contains the **kernel-version-specific** ath10k-ct kmod that is
published as an extra apk feed, plus a copy of the upstream OpenWrt kmod for
recovery.

```
6.12.112-1-1ef2780854971e23a655b05e5b5c3630/
  kmod-ath10k-ct-6.12.112.2025.12.01~bb84e159-r1.apk   # spectral enabled
stock/
  kmod-ath10k-ct-official-6.12.112.2025.12.01~bb84e159-r1.apk  # upstream
```

The workflow copies both directories to GitHub Pages and generates a signed
`packages.adb` only for the spectral feed; `stock/` is not indexed (it would be a
duplicate of the same package/version) and exists purely so a broken setup can be
recovered from a single URL.

The kmod feed index is signed with the repository's ECDSA P-256 key, the same
key that signs the main feed index; `apk` verifies the kmod package against the
checksum covered by that signed index.  Authorize the public key once (see the
site's Install section); the stock recovery `.apk` is signed by OpenWrt's
release key instead.

## What is different from the stock kmod

The stock OpenWrt `kmod-ath10k-ct` package is built without
`CONFIG_ATH10K_SPECTRAL`, so the driver does not create
`debugfs/.../ath10k/spectral_scan0`.  This build only changes the driver build
flags:

```
CONFIG_ATH10K_SPECTRAL=y
CONFIG_ATH10K_DEBUGFS=y        (implies CONFIG_PACKAGE_MAC80211_DEBUGFS=y)
CONFIG_ATH10K_DEBUG=y
```

The kernel already has `CONFIG_RELAY=y`, so **no kernel rebuild or kernel
patch** is involved.  The driver source is unmodified.

## Provenance and build recipe

| Item | Value |
|---|---|
| OpenWrt SDK | 25.12.6, target `ipq40xx/generic` |
| Kernel | 6.12.112 |
| Kernel ABI | `6.12.112~1ef2780854971e23a655b05e5b5c3630-r1` |
| Driver package | `feeds/base/kernel/ath10k-ct`, version `2025.12.01~bb84e159` |
| Driver upstream | <https://github.com/greearb/ath10k-ct> |
| Package | `kmod-ath10k-ct-6.12.112.2025.12.01~bb84e159-r1` (regular buffers) |

Reproduce with the official SDK and the pinned base/luci feeds:

```sh
# inside the extracted OpenWrt 25.12.6 SDK for ipq40xx/generic
./scripts/feeds update base luci
./scripts/feeds install -a -p base
./scripts/feeds install -a -p luci

cat >> .config <<'EOF'
CONFIG_ALL=n
CONFIG_ALL_KMODS=n
CONFIG_PACKAGE_ATH_DEBUG=y
CONFIG_PACKAGE_ATH_SPECTRAL=y
CONFIG_PACKAGE_MAC80211_DEBUGFS=y
CONFIG_PACKAGE_kmod-ath10k-ct=y
CONFIG_PACKAGE_kmod-mac80211=y
CONFIG_PACKAGE_kmod-cfg80211=y
CONFIG_PACKAGE_kmod-ath=y
EOF
rm -rf tmp && make defconfig
make -j"$(nproc)" package/feeds/base/mac80211/compile
make -j$(nproc) package/feeds/base/ath10k-ct/compile
# result (openwrt 25.12 paths):
#   bin/targets/ipq40xx/generic/packages/kmod-ath10k-ct-*.apk
```

The kmod is built against the SDK's kernel and mac80211 backports, which match
the 25.12.6 release, so vermagic and the kernel dependency are identical to the
running image.  Building for a different kernel requires a matching SDK/feeds
checkout — do not reuse this binary there.

## Checksums

```
ca2aac7f96d2b018d54131fcf9702f26581266610a39f26bcaca59aa634d6c5e  kmod-ath10k-ct-6.12.112.2025.12.01~bb84e159-r1.apk
b1f261de4da11583a77eea4dbe056b38188f15920804d85bb71383e78c335436  stock/kmod-ath10k-ct-official-6.12.112.2025.12.01~bb84e159-r1.apk
```

The stock apk was downloaded from
<https://downloads.openwrt.org/releases/25.12.6/targets/ipq40xx/generic/kmods/6.12.112-1-1ef2780854971e23a655b05e5b5c3630/>.
Verify with `sha256sum` before using it for recovery.

## Enable and recover

Enable:

```sh
echo "https://fmu.github.io/openwrt-spectral/kmods/6.12.112-1-1ef2780854971e23a655b05e5b5c3630/packages.adb" \
  >> /etc/apk/repositories.d/customfeeds.list
apk update
apk add --upgrade kmod-ath10k-ct
reboot
```

Recover to upstream:

```sh
wget -O /tmp/kmod-stock.apk \
  "https://fmu.github.io/openwrt-spectral/kmods/stock/kmod-ath10k-ct-official-6.12.112.2025.12.01~bb84e159-r1.apk"
apk del kmod-ath10k-ct
apk add /tmp/kmod-stock.apk
reboot
```

(The stock package is signed by OpenWrt's release key; use
`--allow-untrusted` only if your image does not trust it.)

**Warning:** this replaces a core kernel module.  Do not hot-reload the
ath10k modules while hostapd is running (a live test left the 5 GHz AP unable
to start until reboot); reboot instead.  Keep LAN/Ethernet access while testing.

## License

The kmod is a build of the GPLv2 **ath10k-ct** driver
(<https://github.com/greearb/ath10k-ct>) as packaged by OpenWrt's
`feeds/base/kernel/ath10k-ct`.  The package metadata and driver source retain
their GPLv2 license; this repository's own code (daemon, LuCI app, site) is MIT.
No firmware is included in these packages.

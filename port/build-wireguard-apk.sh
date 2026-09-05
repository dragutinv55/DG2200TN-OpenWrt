#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
DEVICE_ROOT="$(CDPATH= cd -- "$ROOT/.." && pwd)"
OPENWRT="$DEVICE_ROOT/DG220TN_openwrt"
KERNEL="$ROOT/src/linux-6.12"
BASE_BUILD="$ROOT/build/stock-rui"
BUILD="$ROOT/build/wireguard"
OUTPUT="$ROOT/packages"
APK="$OPENWRT/staging_dir/host/bin/apk"
FAKEROOT="$OPENWRT/staging_dir/host/bin/fakeroot"
PRIVATE_KEY="$OPENWRT/private-key.pem"
PUBLIC_KEY="$OPENWRT/public-key.pem"
KERNEL_CONFIG="$KERNEL/.config"
KERNEL_CONFIG_BACKUP="$ROOT/.config.wireguard-backup"
BASE_CONFIG="$BASE_BUILD/kernel-flash.config"
BASE_MODULE_ROOT="$BASE_BUILD/rootfs/lib/modules/6.12.0"
JOBS="${JOBS:-$(nproc)}"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1788543345}"
. "$ROOT/dg2200tn-kernel-build.env"

PACKAGE_NAME="kmod-wireguard"
PACKAGE_VERSION="6.12.0-r2"
PACKAGE_ARCH="arm_cortex-a9"
KERNEL_RELEASE="6.12.0"
EXPECTED_VERMAGIC="6.12.0 SMP mod_unload ARMv7 p2v8 "
EXPECTED_BUILTINS_SHA256="3e818bfd94fe16d2827223234a13dce012f8c7cb0a32f165983d537361daa650"
EXPECTED_CHACHA_SHA256="1f28b715f3c8bedf0345255003d987d0558bb4643bc96214961f548cce31c384"
EXPECTED_BASE_INDEX_SHA256="c7c6def4d098e2d5273c74489a305d5c2fbfd4b4fdf21451d99ec8577106b415"
PACKAGE_FILE="$OUTPUT/$PACKAGE_NAME-$PACKAGE_VERSION.apk"
PACKAGE_ROOT="$BUILD/package-root"
MODULE_ROOT="$BUILD/modules-root/lib/modules/$KERNEL_RELEASE"

restore_pending=0

restore_config() {
	if [ "$restore_pending" = 1 ] && [ -f "$KERNEL_CONFIG_BACKUP" ]; then
		cp "$KERNEL_CONFIG_BACKUP" "$KERNEL_CONFIG" || return
		make -s -C "$KERNEL" ARCH=arm \
			CROSS_COMPILE=arm-linux-gnueabi- olddefconfig >/dev/null ||
			return
		rm -f "$KERNEL_CONFIG_BACKUP" || return
		restore_pending=0
	fi
}

cleanup() {
	status=$?
	trap - EXIT HUP INT TERM
	restore_config || status=$?
	exit "$status"
}
trap cleanup EXIT HUP INT TERM

fail() {
	echo "error: $*" >&2
	exit 1
}

for tool in "$APK" "$FAKEROOT" "$PRIVATE_KEY" "$PUBLIC_KEY" "$BASE_CONFIG" \
	"$KERNEL_CONFIG" "$ROOT/wireguard-apk-pre-install.sh" \
	"$ROOT/wireguard-apk-post-install.sh" \
	"$ROOT/wireguard-apk-pre-deinstall.sh" \
	"$ROOT/dg2200tn-wireguard-kmod.init"; do
	[ -f "$tool" ] || fail "missing $tool"
done
for tool in cmp modinfo make sed sha256sum; do
	command -v "$tool" >/dev/null || fail "missing host tool: $tool"
done
[ ! -e "$KERNEL_CONFIG_BACKUP" ] ||
	fail "stale config backup exists: $KERNEL_CONFIG_BACKUP"

builtins_sha256="$(sha256sum "$BASE_MODULE_ROOT/modules.builtin" |
	cut -d ' ' -f 1)"
chacha_sha256="$(sha256sum \
	"$BASE_MODULE_ROOT/kernel/arch/arm/crypto/chacha-neon.ko" |
	cut -d ' ' -f 1)"
base_index_sha256="$(sha256sum "$BASE_MODULE_ROOT/modules.dep.bin" |
	cut -d ' ' -f 1)"
[ "$builtins_sha256" = "$EXPECTED_BUILTINS_SHA256" ] ||
	fail "persistent build modules.builtin does not match firmware build #73"
[ "$chacha_sha256" = "$EXPECTED_CHACHA_SHA256" ] ||
	fail "persistent build chacha-neon does not match firmware build #73"
[ "$base_index_sha256" = "$EXPECTED_BASE_INDEX_SHA256" ] ||
	fail "persistent build module index does not match firmware build #73"

rm -rf "$BUILD"
mkdir -p "$BUILD" "$OUTPUT"
cp "$KERNEL_CONFIG" "$KERNEL_CONFIG_BACKUP"
restore_pending=1
cp "$BASE_CONFIG" "$KERNEL_CONFIG"
"$KERNEL/scripts/config" --file "$KERNEL_CONFIG" \
	--module WIREGUARD \
	--disable WIREGUARD_DEBUG
make -s -C "$KERNEL" ARCH=arm \
	CROSS_COMPILE=arm-linux-gnueabi- olddefconfig
cp "$KERNEL_CONFIG" "$BUILD/kernel-wireguard.config"

make -C "$KERNEL" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- \
	-j"$JOBS" modules >"$BUILD/build.log" 2>&1
actual_release="$(make -s -C "$KERNEL" ARCH=arm \
	CROSS_COMPILE=arm-linux-gnueabi- kernelrelease)"
[ "$actual_release" = "$KERNEL_RELEASE" ] ||
	fail "unexpected kernel release: $actual_release"
make -C "$KERNEL" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- \
	INSTALL_MOD_PATH="$BUILD/modules-root" INSTALL_MOD_STRIP=1 \
	modules_install >"$BUILD/install.log" 2>&1

restore_config

modules="
kernel/arch/arm/crypto/poly1305-arm.ko
kernel/drivers/net/wireguard/wireguard.ko
kernel/lib/crypto/libchacha20poly1305.ko
kernel/lib/crypto/libcurve25519-generic.ko
kernel/lib/crypto/libcurve25519.ko
kernel/net/ipv4/udp_tunnel.ko
kernel/net/ipv6/ip6_udp_tunnel.ko
"

for module in $modules; do
	source_module="$MODULE_ROOT/$module"
	target_module="$PACKAGE_ROOT/lib/modules/$KERNEL_RELEASE/$module"
	[ -f "$source_module" ] || fail "missing built module: $module"
	vermagic="$(modinfo -F vermagic "$source_module")"
	[ "$vermagic" = "$EXPECTED_VERMAGIC" ] ||
		fail "unexpected vermagic for $module: $vermagic"
	install -D -m 0644 "$source_module" "$target_module"
done

wireguard="$PACKAGE_ROOT/lib/modules/$KERNEL_RELEASE/kernel/drivers/net/wireguard/wireguard.ko"
chacha_poly="$PACKAGE_ROOT/lib/modules/$KERNEL_RELEASE/kernel/lib/crypto/libchacha20poly1305.ko"
[ "$(modinfo -F depends "$wireguard")" = \
	"libcurve25519-generic,udp_tunnel,ip6_udp_tunnel,libchacha20poly1305,ipv6" ] ||
	fail "unexpected WireGuard dependency set"
[ "$(modinfo -F depends "$chacha_poly")" = \
	"poly1305-arm,chacha-neon" ] ||
	fail "unexpected ChaCha20-Poly1305 dependency set"

mkdir -p "$PACKAGE_ROOT/etc/init.d" \
	"$PACKAGE_ROOT/lib/apk/packages" "$BUILD/keys" "$BUILD/scripts"
install -m 0755 "$ROOT/dg2200tn-wireguard-kmod.init" \
	"$PACKAGE_ROOT/etc/init.d/dg2200tn-wireguard-kmod"

(cd "$PACKAGE_ROOT" &&
	find . -type f -printf '/%P\n' | LC_ALL=C sort) >"$BUILD/package.list"
install -m 0644 "$BUILD/package.list" \
	"$PACKAGE_ROOT/lib/apk/packages/$PACKAGE_NAME.list"
install -m 0644 "$PUBLIC_KEY" "$BUILD/keys/public-key.pem"
install -m 0755 "$ROOT/wireguard-apk-pre-install.sh" \
	"$BUILD/scripts/pre-install"
install -m 0755 "$ROOT/wireguard-apk-post-install.sh" \
	"$BUILD/scripts/post-install"
install -m 0755 "$ROOT/wireguard-apk-pre-deinstall.sh" \
	"$BUILD/scripts/pre-deinstall"

find "$PACKAGE_ROOT" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
find "$PACKAGE_ROOT" -type d -exec chmod 0755 {} +
find "$PACKAGE_ROOT" -type f -exec chmod 0644 {} +
chmod 0755 "$PACKAGE_ROOT/etc/init.d/dg2200tn-wireguard-kmod"
touch -d "@$SOURCE_DATE_EPOCH" \
	"$BUILD/scripts/pre-install" "$BUILD/scripts/post-install" \
	"$BUILD/scripts/pre-deinstall"

PACKAGE_CANDIDATE="$BUILD/$PACKAGE_NAME-$PACKAGE_VERSION.apk"
rm -f "$PACKAGE_CANDIDATE" "$PACKAGE_FILE.sha256"
SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" "$FAKEROOT" sh -c '
	chown -R 0:0 "$1"
	shift
	exec "$@"
' sh "$PACKAGE_ROOT" "$APK" mkpkg \
	--compression deflate:9 \
	--sign-key "$PRIVATE_KEY" \
	--info "name:$PACKAGE_NAME" \
	--info "version:$PACKAGE_VERSION" \
	--info "description:WireGuard kernel modules for DG2200TN kernel 6.12.0 build 73" \
	--info "arch:$PACKAGE_ARCH" \
	--info "license:GPL-2.0-only" \
	--info "origin:DG2200TN-OpenWrt" \
	--info "url:https://github.com/dragutinv55/DG2200TN-OpenWrt" \
	--info "depends:" \
	--script "pre-install:$BUILD/scripts/pre-install" \
	--script "pre-upgrade:$BUILD/scripts/pre-install" \
	--script "post-install:$BUILD/scripts/post-install" \
	--script "post-upgrade:$BUILD/scripts/post-install" \
	--script "pre-deinstall:$BUILD/scripts/pre-deinstall" \
	--files "$PACKAGE_ROOT" \
	--output "$PACKAGE_CANDIDATE"

"$APK" --keys-dir "$BUILD/keys" verify "$PACKAGE_CANDIDATE"
if [ -f "$PACKAGE_FILE" ] &&
	"$APK" --keys-dir "$BUILD/keys" verify "$PACKAGE_FILE" >/dev/null 2>&1 &&
	"$APK" adbdump "$PACKAGE_FILE" |
		sed '/^# sig /d' >"$BUILD/existing.adbdump" &&
	"$APK" adbdump "$PACKAGE_CANDIDATE" |
		sed '/^# sig /d' >"$BUILD/candidate.adbdump" &&
	cmp -s "$BUILD/existing.adbdump" "$BUILD/candidate.adbdump"
then
	rm -f "$PACKAGE_CANDIDATE"
else
	mv "$PACKAGE_CANDIDATE" "$PACKAGE_FILE"
fi

"$APK" --keys-dir "$BUILD/keys" verify "$PACKAGE_FILE"
"$APK" adbdump "$PACKAGE_FILE" >"$BUILD/package.adbdump"
(cd "$OUTPUT" && sha256sum "$(basename "$PACKAGE_FILE")") \
	>"$PACKAGE_FILE.sha256"

echo "Built $PACKAGE_FILE"
cat "$PACKAGE_FILE.sha256"

#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
DEVICE_ROOT="$(CDPATH= cd -- "$ROOT/.." && pwd)"
OPENWRT="$DEVICE_ROOT/DG220TN_openwrt"
OPENWRT_CONFIG="$OPENWRT/.config"
OPENWRT_CONFIG_BACKUP="$ROOT/.config.sqm-userspace-backup"
OPENWRT_CONFIG_CANDIDATE="$ROOT/.config.sqm-userspace-candidate"
OPENWRT_KERNEL_CONFIG="$OPENWRT/build_dir/target-arm_cortex-a9_musl_eabi/linux-armsr_armv7/linux-6.12.94/.config"
OPENWRT_KERNEL_CONFIG_BACKUP="$ROOT/.config.sqm-kernel-backup"
IPTABLES_OUTPUT="$OPENWRT/bin/targets/armsr/armv7/packages"
KERNEL_CONFIG_TOOL="$ROOT/src/linux-6.12/scripts/config"
BUILD="$ROOT/build/sqm-userspace"
OUTPUT="$ROOT/packages"
APK="$OPENWRT/staging_dir/host/bin/apk"
PRIVATE_KEY="$OPENWRT/private-key.pem"
PUBLIC_KEY="$OPENWRT/public-key.pem"
JOBS="${JOBS:-$(nproc)}"

SQM_VERSION="1.7.2-r1"
LUCI_SQM_VERSION="26.246.70755~4fd72fd"
SQM_URL="https://downloads.openwrt.org/releases/25.12.5/packages/arm_cortex-a9/packages/sqm-scripts-$SQM_VERSION.apk"
LUCI_SQM_URL="https://downloads.openwrt.org/releases/25.12.5/packages/arm_cortex-a9/luci/luci-app-sqm-$LUCI_SQM_VERSION.apk"
SQM_SHA256="519891937bbd76869fdcffd2a80f7bfa774e417b2908bb42289470323cc35fec"
LUCI_SQM_SHA256="ecf2fe13a9905ac653af1319f62db9a5ad8cb4f9ede53332e877949ac87fd004"
BUNDLE_MANIFEST="$OUTPUT/dg2200tn-sqm-userspace-build73.sha256"
REPOSITORY_INDEX="$OUTPUT/dg2200tn-sqm-userspace-build73.adb"

restore_pending=0

restore_configs() {
	if [ "$restore_pending" = 1 ]; then
		[ -f "$OPENWRT_CONFIG_BACKUP" ] || return
		[ -f "$OPENWRT_KERNEL_CONFIG_BACKUP" ] || return
		cp "$OPENWRT_CONFIG_BACKUP" "$OPENWRT_CONFIG" || return
		cp "$OPENWRT_KERNEL_CONFIG_BACKUP" \
			"$OPENWRT_KERNEL_CONFIG" || return
		rm -f "$OPENWRT_CONFIG_BACKUP" \
			"$OPENWRT_KERNEL_CONFIG_BACKUP" \
			"$OPENWRT_CONFIG_CANDIDATE" || return
		restore_pending=0
	fi
}

cleanup() {
	status=$?
	trap - EXIT HUP INT TERM
	restore_configs || status=$?
	exit "$status"
}
trap cleanup EXIT HUP INT TERM

fail() {
	echo "error: $*" >&2
	exit 1
}

download_checked() {
	url="$1"
	output="$2"
	expected="$3"

	curl -fsSL --retry 3 --output "$output" "$url"
	actual="$(sha256sum "$output" | cut -d ' ' -f 1)"
	[ "$actual" = "$expected" ] ||
		fail "checksum mismatch for $(basename "$output")"
}

normalize_adb() {
	"$APK" adbdump "$1" | sed '/^# sig /d'
}

publish_signed_package() {
	source_file="$1"
	expected_name="$2"
	expected_arch="$3"
	package_file="$OUTPUT/$(basename "$source_file")"
	candidate="$BUILD/publish/$(basename "$source_file")"
	dump="$BUILD/publish/$expected_name.adbdump"

	cp "$source_file" "$candidate"
	"$APK" --allow-untrusted adbsign --reset-signatures \
		--sign-key "$PRIVATE_KEY" "$candidate"
	"$APK" --keys-dir "$BUILD/keys" verify "$candidate"
	"$APK" adbdump "$candidate" >"$dump"
	grep -qx "  name: $expected_name" "$dump" ||
		fail "unexpected package name in $(basename "$source_file")"
	grep -qx "  arch: $expected_arch" "$dump" ||
		fail "unexpected package architecture in $(basename "$source_file")"

	if [ -f "$package_file" ] &&
		"$APK" --keys-dir "$BUILD/keys" verify "$package_file" >/dev/null 2>&1 &&
		normalize_adb "$package_file" >"$BUILD/publish/existing.adbdump" &&
		normalize_adb "$candidate" >"$BUILD/publish/candidate.adbdump" &&
		cmp -s "$BUILD/publish/existing.adbdump" \
			"$BUILD/publish/candidate.adbdump"
	then
		rm -f "$candidate"
	else
		mv "$candidate" "$package_file"
	fi

	"$APK" --keys-dir "$BUILD/keys" verify "$package_file"
	(
		cd "$OUTPUT"
		sha256sum "$(basename "$package_file")"
	) >"$package_file.sha256"
	cat "$package_file.sha256" >>"$BUNDLE_MANIFEST"
}

for tool in "$APK" "$PRIVATE_KEY" "$PUBLIC_KEY" "$OPENWRT_CONFIG" \
	"$OPENWRT_KERNEL_CONFIG" "$KERNEL_CONFIG_TOOL"; do
	[ -f "$tool" ] || fail "missing $tool"
done
for tool in awk cmp curl make sed sha256sum; do
	command -v "$tool" >/dev/null || fail "missing host tool: $tool"
done
[ ! -e "$OPENWRT_CONFIG_BACKUP" ] ||
	fail "stale config backup exists: $OPENWRT_CONFIG_BACKUP"
[ ! -e "$OPENWRT_KERNEL_CONFIG_BACKUP" ] ||
	fail "stale kernel config backup exists: $OPENWRT_KERNEL_CONFIG_BACKUP"
grep -qx 'CONFIG_TARGET_ARCH_PACKAGES="arm_cortex-a9"' "$OPENWRT_CONFIG" ||
	fail "OpenWrt build tree is not configured for arm_cortex-a9"

rm -rf "$BUILD"
mkdir -p "$BUILD/download" "$BUILD/keys" "$BUILD/publish" "$OUTPUT"
install -m 0644 "$PUBLIC_KEY" "$BUILD/keys/public-key.pem"
rm -f "$BUNDLE_MANIFEST"

cp "$OPENWRT_CONFIG" "$OPENWRT_CONFIG_BACKUP"
cp "$OPENWRT_KERNEL_CONFIG" "$OPENWRT_KERNEL_CONFIG_BACKUP"
restore_pending=1

awk '
	$0 == "# CONFIG_PACKAGE_iptables-nft is not set" {
		print "CONFIG_PACKAGE_iptables-nft=m"
		next
	}
	$0 == "# CONFIG_PACKAGE_iptables-mod-ipopt is not set" {
		print "CONFIG_PACKAGE_iptables-mod-ipopt=m"
		next
	}
	{ print }
' "$OPENWRT_CONFIG" >"$OPENWRT_CONFIG_CANDIDATE"
cp "$OPENWRT_CONFIG_CANDIDATE" "$OPENWRT_CONFIG"
make -s -C "$OPENWRT" defconfig
for package in iptables-nft iptables-mod-ipopt; do
	grep -qx "CONFIG_PACKAGE_$package=m" "$OPENWRT_CONFIG" ||
		fail "OpenWrt did not select $package as a module"
done

for option in \
	NETFILTER_XT_MATCH_DSCP NETFILTER_XT_TARGET_DSCP \
	NETFILTER_XT_MATCH_LENGTH NETFILTER_XT_MATCH_STATISTIC \
	NETFILTER_XT_MATCH_TCPMSS NETFILTER_XT_TARGET_CLASSIFY \
	IP_NF_TARGET_ECN NETFILTER_XT_MATCH_ECN \
	NETFILTER_XT_MATCH_HL NETFILTER_XT_TARGET_HL
do
	"$KERNEL_CONFIG_TOOL" --file "$OPENWRT_KERNEL_CONFIG" \
		--module "$option"
	grep -qx "CONFIG_$option=m" "$OPENWRT_KERNEL_CONFIG" ||
		fail "OpenWrt kernel config did not enable CONFIG_$option=m"
done

make -C "$OPENWRT/package/network/utils/iptables" \
	TOPDIR="$OPENWRT" clean >"$BUILD/clean.log" 2>&1
make -C "$OPENWRT/package/network/utils/iptables" \
	TOPDIR="$OPENWRT" -j"$JOBS" compile >"$BUILD/build.log" 2>&1

restore_configs

download_checked "$SQM_URL" \
	"$BUILD/download/sqm-scripts-$SQM_VERSION.apk" "$SQM_SHA256"
download_checked "$LUCI_SQM_URL" \
	"$BUILD/download/luci-app-sqm-$LUCI_SQM_VERSION.apk" "$LUCI_SQM_SHA256"

for package in \
	libxtables12 libiptext0 libiptext6-0 libiptext-nft0 \
	xtables-nft iptables-nft iptables-mod-ipopt
do
	source_file="$IPTABLES_OUTPUT/$package-1.8.10-r3.apk"
	[ -f "$source_file" ] || fail "missing built package: $source_file"
	publish_signed_package "$source_file" "$package" "arm_cortex-a9"
done

publish_signed_package \
	"$BUILD/download/sqm-scripts-$SQM_VERSION.apk" \
	"sqm-scripts" "noarch"
publish_signed_package \
	"$BUILD/download/luci-app-sqm-$LUCI_SQM_VERSION.apk" \
	"luci-app-sqm" "noarch"

ipopt_dump="$BUILD/publish/iptables-mod-ipopt.adbdump"
grep -q 'installed-size: 63486' "$ipopt_dump" ||
	fail "iptables-mod-ipopt payload is incomplete"
for extension in \
	libipt_ECN.so libipt_TTL.so libipt_ttl.so \
	libxt_CLASSIFY.so libxt_DSCP.so libxt_TOS.so \
	libxt_dscp.so libxt_ecn.so libxt_length.so \
	libxt_statistic.so libxt_tcpmss.so libxt_tos.so
do
	grep -q "name: $extension" "$ipopt_dump" ||
		fail "iptables-mod-ipopt is missing $extension"
done

LC_ALL=C sort -o "$BUNDLE_MANIFEST" "$BUNDLE_MANIFEST"

set -- \
	"$OUTPUT/libxtables12-1.8.10-r3.apk" \
	"$OUTPUT/libiptext0-1.8.10-r3.apk" \
	"$OUTPUT/libiptext6-0-1.8.10-r3.apk" \
	"$OUTPUT/libiptext-nft0-1.8.10-r3.apk" \
	"$OUTPUT/xtables-nft-1.8.10-r3.apk" \
	"$OUTPUT/iptables-nft-1.8.10-r3.apk" \
	"$OUTPUT/iptables-mod-ipopt-1.8.10-r3.apk" \
	"$OUTPUT/sqm-scripts-$SQM_VERSION.apk" \
	"$OUTPUT/luci-app-sqm-$LUCI_SQM_VERSION.apk"

index_candidate="$BUILD/publish/$(basename "$REPOSITORY_INDEX")"
"$APK" --allow-untrusted mkndx \
	--sign-key "$PRIVATE_KEY" \
	--description "DG2200TN Cortex-A9 SQM userspace for kernel build 73" \
	--output "$index_candidate" "$@"
"$APK" --keys-dir "$BUILD/keys" verify "$index_candidate"

if [ -f "$REPOSITORY_INDEX" ] &&
	"$APK" --keys-dir "$BUILD/keys" verify "$REPOSITORY_INDEX" >/dev/null 2>&1 &&
	normalize_adb "$REPOSITORY_INDEX" >"$BUILD/publish/existing-index.adbdump" &&
	normalize_adb "$index_candidate" >"$BUILD/publish/candidate-index.adbdump" &&
	cmp -s "$BUILD/publish/existing-index.adbdump" \
		"$BUILD/publish/candidate-index.adbdump"
then
	rm -f "$index_candidate"
else
	mv "$index_candidate" "$REPOSITORY_INDEX"
fi

"$APK" --keys-dir "$BUILD/keys" verify "$REPOSITORY_INDEX"
(
	cd "$OUTPUT"
	sha256sum "$(basename "$REPOSITORY_INDEX")"
) >"$REPOSITORY_INDEX.sha256"

echo "Built SQM userspace bundle:"
cat "$BUNDLE_MANIFEST"
cat "$REPOSITORY_INDEX.sha256"

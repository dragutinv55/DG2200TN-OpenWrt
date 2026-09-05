#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
DEVICE_ROOT="$(CDPATH= cd -- "$ROOT/.." && pwd)"
OPENWRT="$DEVICE_ROOT/DG220TN_openwrt"
KERNEL="$ROOT/src/linux-6.12"
BASE_BUILD="$ROOT/build/stock-rui"
BASE_MODULE_ROOT="$BASE_BUILD/rootfs/lib/modules/6.12.0"
BUILD="$ROOT/build/network-kmods"
OUTPUT="$ROOT/packages"
APK="$OPENWRT/staging_dir/host/bin/apk"
FAKEROOT="$OPENWRT/staging_dir/host/bin/fakeroot"
PRIVATE_KEY="$OPENWRT/private-key.pem"
PUBLIC_KEY="$OPENWRT/public-key.pem"
KERNEL_CONFIG="$KERNEL/.config"
KERNEL_CONFIG_BACKUP="$ROOT/.config.network-kmods-backup"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1788594200}"
. "$ROOT/dg2200tn-kernel-build.env"

PACKAGE_VERSION="6.12.0-r1"
PACKAGE_ARCH="arm_cortex-a9"
KERNEL_RELEASE="6.12.0"
EXPECTED_VERMAGIC="6.12.0 SMP mod_unload ARMv7 p2v8 "
EXPECTED_BUILTINS_SHA256="3e818bfd94fe16d2827223234a13dce012f8c7cb0a32f165983d537361daa650"
EXPECTED_BASE_INDEX_SHA256="c7c6def4d098e2d5273c74489a305d5c2fbfd4b4fdf21451d99ec8577106b415"
BUNDLE_MANIFEST="$OUTPUT/dg2200tn-network-kmods-6.12.0-build73.sha256"
EXTRA_MODULE_ROOT="$BUILD/modules-root/lib/modules/$KERNEL_RELEASE"

restore_pending=0

restore_config() {
	if [ "$restore_pending" = 1 ] && [ -f "$KERNEL_CONFIG_BACKUP" ]; then
		cp "$KERNEL_CONFIG_BACKUP" "$KERNEL_CONFIG" || return
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

for tool in \
	"$APK" "$FAKEROOT" "$PRIVATE_KEY" "$PUBLIC_KEY" \
	"$BASE_BUILD/kernel-flash.config" "$BASE_MODULE_ROOT/modules.builtin" \
	"$BASE_MODULE_ROOT/modules.dep.bin" \
	"$ROOT/network-kmod-apk-pre-install.sh.in" \
	"$ROOT/network-kmod-apk-post-install.sh.in" \
	"$ROOT/network-kmod-apk-pre-deinstall.sh.in" \
	"$ROOT/dg2200tn-network-kmod.init.in"
do
	[ -f "$tool" ] || fail "missing $tool"
done
for tool in cmp file modinfo sed sha256sum; do
	command -v "$tool" >/dev/null || fail "missing host tool: $tool"
done
[ ! -e "$KERNEL_CONFIG_BACKUP" ] ||
	fail "stale config backup exists: $KERNEL_CONFIG_BACKUP"

builtins_sha256="$(sha256sum "$BASE_MODULE_ROOT/modules.builtin" |
	cut -d ' ' -f 1)"
base_index_sha256="$(sha256sum "$BASE_MODULE_ROOT/modules.dep.bin" |
	cut -d ' ' -f 1)"
[ "$builtins_sha256" = "$EXPECTED_BUILTINS_SHA256" ] ||
	fail "persistent build modules.builtin does not match kernel build #73"
[ "$base_index_sha256" = "$EXPECTED_BASE_INDEX_SHA256" ] ||
	fail "persistent build module index does not match kernel build #73"

for option in \
	IP_MULTIPLE_TABLES IPV6_MULTIPLE_TABLES NF_CONNTRACK_EVENTS \
	NET_SCHED NET_CLS NET_CLS_ACT NET_EMATCH
do
	grep -qx "CONFIG_$option=y" "$BASE_BUILD/kernel-flash.config" ||
		fail "persistent kernel is missing CONFIG_$option=y"
done

rm -rf "$BUILD"
mkdir -p "$BUILD" "$OUTPUT"
rm -f "$BUNDLE_MANIFEST"

cp "$KERNEL_CONFIG" "$KERNEL_CONFIG_BACKUP"
restore_pending=1
cp "$BASE_BUILD/kernel-flash.config" "$KERNEL_CONFIG"
for option in \
	IP_NF_FILTER IP_NF_MANGLE NFT_COMPAT \
	NETFILTER_XT_MATCH_COMMENT NETFILTER_XT_MATCH_DSCP \
	NETFILTER_XT_MATCH_ECN NETFILTER_XT_MATCH_HL \
	NETFILTER_XT_MATCH_LENGTH NETFILTER_XT_MATCH_LIMIT \
	NETFILTER_XT_MATCH_MAC NETFILTER_XT_MATCH_MULTIPORT \
	NETFILTER_XT_MATCH_STATISTIC NETFILTER_XT_MATCH_TCPMSS \
	NETFILTER_XT_MATCH_TIME NETFILTER_XT_MARK \
	NETFILTER_XT_TARGET_CLASSIFY NETFILTER_XT_TARGET_DSCP \
	NETFILTER_XT_TARGET_HL NETFILTER_XT_TARGET_LOG \
	NETFILTER_XT_TARGET_TCPMSS IP_NF_TARGET_ECN \
	IP_NF_TARGET_REJECT
do
	"$KERNEL/scripts/config" --file "$KERNEL_CONFIG" --module "$option"
done
make -C "$KERNEL" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- \
	clean >"$BUILD/clean.log" 2>&1
make -s -C "$KERNEL" ARCH=arm \
	CROSS_COMPILE=arm-linux-gnueabi- olddefconfig
cp "$KERNEL_CONFIG" "$BUILD/kernel-network-modules.config"
make -C "$KERNEL" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- \
	-j"${JOBS:-$(nproc)}" vmlinux modules >"$BUILD/build.log" 2>&1
make -C "$KERNEL" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- \
	INSTALL_MOD_PATH="$BUILD/modules-root" INSTALL_MOD_STRIP=1 \
	modules_install >"$BUILD/install.log" 2>&1
restore_config

render_template() {
	template="$1"
	output="$2"
	package_name="$3"
	service_name="$4"
	start_order="$5"

	sed \
		-e "s|@PACKAGE_NAME@|$package_name|g" \
		-e "s|@SERVICE_NAME@|$service_name|g" \
		-e "s|@START@|$start_order|g" \
		-e "s|@KERNEL_RELEASE@|$KERNEL_RELEASE|g" \
		-e "s|@KERNEL_UNAME_VERSION@|$DG2200TN_KERNEL_UNAME_VERSION|g" \
		-e "s|@KERNEL_BUILD@|#$DG2200TN_KERNEL_BUILD_VERSION|g" \
		-e "s|@BUILTINS_SHA256@|$builtins_sha256|g" \
		"$template" >"$output"
	chmod 0755 "$output"
}

normalize_adb() {
	"$APK" adbdump "$1" | sed '/^# sig /d'
}

build_package() {
	package_name="$1"
	description="$2"
	depends="$3"
	service_name="$4"
	start_order="$5"
	package_build="$BUILD/$package_name"
	package_root="$package_build/root"
	package_file="$OUTPUT/$package_name-$PACKAGE_VERSION.apk"
	candidate="$package_build/$package_name-$PACKAGE_VERSION.apk"
	module_manifest="$package_root/usr/lib/dg2200tn-kmods/$service_name.modules"

	rm -rf "$package_build"
	mkdir -p "$package_root/etc/init.d" \
		"$package_root/lib/apk/packages" \
		"$(dirname "$module_manifest")" \
		"$package_build/keys" "$package_build/scripts"

	while IFS='|' read -r module_name module_path parameters include_module; do
		[ -n "$module_name" ] || continue
		source_module="$BASE_MODULE_ROOT/$module_path"
		if [ ! -f "$source_module" ]; then
			source_module="$EXTRA_MODULE_ROOT/$module_path"
		fi
		[ -f "$source_module" ] ||
			fail "$package_name is missing module $module_path"
		[ "$(modinfo -F name "$source_module")" = "$module_name" ] ||
			fail "$package_name has unexpected module name for $module_path"
		[ "$(modinfo -F vermagic "$source_module")" = "$EXPECTED_VERMAGIC" ] ||
			fail "$package_name has unexpected vermagic for $module_path"
		case "$(file -b "$source_module")" in
			"ELF 32-bit LSB relocatable, ARM,"*) ;;
			*) fail "$package_name has non-ARM module $module_path" ;;
		esac
		printf '%s|%s|%s\n' \
			"$module_name" "$module_path" "$parameters" >>"$module_manifest"
		if [ "$include_module" = 1 ]; then
			install -D -m 0644 "$source_module" \
				"$package_root/lib/modules/$KERNEL_RELEASE/$module_path"
		fi
	done

	render_template "$ROOT/dg2200tn-network-kmod.init.in" \
		"$package_root/etc/init.d/$service_name" \
		"$package_name" "$service_name" "$start_order"
	render_template "$ROOT/network-kmod-apk-pre-install.sh.in" \
		"$package_build/scripts/pre-install" \
		"$package_name" "$service_name" "$start_order"
	render_template "$ROOT/network-kmod-apk-post-install.sh.in" \
		"$package_build/scripts/post-install" \
		"$package_name" "$service_name" "$start_order"
	render_template "$ROOT/network-kmod-apk-pre-deinstall.sh.in" \
		"$package_build/scripts/pre-deinstall" \
		"$package_name" "$service_name" "$start_order"
	install -m 0644 "$PUBLIC_KEY" "$package_build/keys/public-key.pem"

	(
		cd "$package_root"
		find . -type f -printf '/%P\n' | LC_ALL=C sort
	) >"$package_build/package.list"
	install -m 0644 "$package_build/package.list" \
		"$package_root/lib/apk/packages/$package_name.list"

	find "$package_root" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
	find "$package_root" -type d -exec chmod 0755 {} +
	find "$package_root" -type f -exec chmod 0644 {} +
	chmod 0755 "$package_root/etc/init.d/$service_name"
	touch -d "@$SOURCE_DATE_EPOCH" \
		"$package_build/scripts/pre-install" \
		"$package_build/scripts/post-install" \
		"$package_build/scripts/pre-deinstall"

	rm -f "$candidate" "$package_file.sha256"
	SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" "$FAKEROOT" sh -c '
		chown -R 0:0 "$1"
		shift
		exec "$@"
	' sh "$package_root" "$APK" mkpkg \
		--compression deflate:9 \
		--sign-key "$PRIVATE_KEY" \
		--info "name:$package_name" \
		--info "version:$PACKAGE_VERSION" \
		--info "description:$description" \
		--info "arch:$PACKAGE_ARCH" \
		--info "license:GPL-2.0-only" \
		--info "origin:DG2200TN-OpenWrt" \
		--info "url:https://github.com/dragutinv55/DG2200TN-OpenWrt" \
		--info "depends:$depends" \
		--script "pre-install:$package_build/scripts/pre-install" \
		--script "pre-upgrade:$package_build/scripts/pre-install" \
		--script "post-install:$package_build/scripts/post-install" \
		--script "post-upgrade:$package_build/scripts/post-install" \
		--script "pre-deinstall:$package_build/scripts/pre-deinstall" \
		--files "$package_root" \
		--output "$candidate"

	"$APK" --keys-dir "$package_build/keys" verify "$candidate"
	if [ -f "$package_file" ] &&
		"$APK" --keys-dir "$package_build/keys" \
			verify "$package_file" >/dev/null 2>&1 &&
		normalize_adb "$package_file" \
			>"$package_build/existing.adbdump" &&
		normalize_adb "$candidate" \
			>"$package_build/candidate.adbdump" &&
		cmp -s "$package_build/existing.adbdump" \
			"$package_build/candidate.adbdump"
	then
		rm -f "$candidate"
	else
		mv "$candidate" "$package_file"
	fi

	"$APK" --keys-dir "$package_build/keys" verify "$package_file"
	"$APK" adbdump "$package_file" >"$package_build/package.adbdump"
	(
		cd "$OUTPUT"
		sha256sum "$(basename "$package_file")"
	) >"$package_file.sha256"
	cat "$package_file.sha256" >>"$BUNDLE_MANIFEST"
	echo "Built $package_file"
}

build_package \
	"kmod-nf-conntrack-netlink" \
	"Connection tracking netlink module for DG2200TN kernel build 73" \
	"" "dg2200tn-nf-conntrack-netlink-kmod" 20 <<'EOF'
nfnetlink|kernel/net/netfilter/nfnetlink.ko||0
nf_conntrack_netlink|kernel/net/netfilter/nf_conntrack_netlink.ko||1
EOF

build_package \
	"kmod-sched-core" \
	"Traffic scheduler modules for DG2200TN kernel build 73" \
	"" "dg2200tn-sched-core-kmod" 21 <<'EOF'
sch_ingress|kernel/net/sched/sch_ingress.ko||1
sch_hfsc|kernel/net/sched/sch_hfsc.ko||1
sch_htb|kernel/net/sched/sch_htb.ko||1
sch_tbf|kernel/net/sched/sch_tbf.ko||1
sch_fq_codel|kernel/net/sched/sch_fq_codel.ko||1
cls_basic|kernel/net/sched/cls_basic.ko||1
cls_fw|kernel/net/sched/cls_fw.ko||1
cls_route|kernel/net/sched/cls_route.ko||1
cls_flow|kernel/net/sched/cls_flow.ko||1
cls_u32|kernel/net/sched/cls_u32.ko||1
em_u32|kernel/net/sched/em_u32.ko||1
act_gact|kernel/net/sched/act_gact.ko||1
act_mirred|kernel/net/sched/act_mirred.ko||1
act_skbedit|kernel/net/sched/act_skbedit.ko||1
cls_matchall|kernel/net/sched/cls_matchall.ko||1
EOF

build_package \
	"kmod-sched-cake" \
	"CAKE scheduler module for DG2200TN kernel build 73" \
	"kmod-sched-core" "dg2200tn-sched-cake-kmod" 22 <<'EOF'
sch_cake|kernel/net/sched/sch_cake.ko||1
EOF

build_package \
	"kmod-ifb" \
	"Intermediate Functional Block module for DG2200TN kernel build 73" \
	"kmod-sched-core" "dg2200tn-ifb-kmod" 23 <<'EOF'
act_mirred|kernel/net/sched/act_mirred.ko||0
ifb|kernel/drivers/net/ifb.ko|numifbs=0|1
EOF

build_package \
	"kmod-tun" \
	"TUN and TAP module for DG2200TN kernel build 73" \
	"" "dg2200tn-tun-kmod" 24 <<'EOF'
tun|kernel/drivers/net/tun.ko||1
EOF

build_package \
	"kmod-nf-socket" \
	"Netfilter socket lookup modules for DG2200TN kernel build 73" \
	"" "dg2200tn-nf-socket-kmod" 25 <<'EOF'
ipv6|kernel/net/ipv6/ipv6.ko||0
nf_socket_ipv4|kernel/net/ipv4/netfilter/nf_socket_ipv4.ko||1
nf_socket_ipv6|kernel/net/ipv6/netfilter/nf_socket_ipv6.ko||1
EOF

build_package \
	"kmod-nf-tproxy" \
	"Netfilter transparent proxy modules for DG2200TN kernel build 73" \
	"" "dg2200tn-nf-tproxy-kmod" 26 <<'EOF'
ipv6|kernel/net/ipv6/ipv6.ko||0
nf_tproxy_ipv4|kernel/net/ipv4/netfilter/nf_tproxy_ipv4.ko||1
nf_tproxy_ipv6|kernel/net/ipv6/netfilter/nf_tproxy_ipv6.ko||1
EOF

build_package \
	"kmod-nft-socket" \
	"nftables socket expression module for DG2200TN kernel build 73" \
	"kmod-nf-socket" "dg2200tn-nft-socket-kmod" 27 <<'EOF'
ipv6|kernel/net/ipv6/ipv6.ko||0
nf_tables|kernel/net/netfilter/nf_tables.ko||0
nf_socket_ipv4|kernel/net/ipv4/netfilter/nf_socket_ipv4.ko||0
nf_socket_ipv6|kernel/net/ipv6/netfilter/nf_socket_ipv6.ko||0
nft_socket|kernel/net/netfilter/nft_socket.ko||1
EOF

build_package \
	"kmod-nft-tproxy" \
	"nftables transparent proxy module for DG2200TN kernel build 73" \
	"kmod-nf-tproxy" "dg2200tn-nft-tproxy-kmod" 28 <<'EOF'
ipv6|kernel/net/ipv6/ipv6.ko||0
nf_tables|kernel/net/netfilter/nf_tables.ko||0
nf_tproxy_ipv4|kernel/net/ipv4/netfilter/nf_tproxy_ipv4.ko||0
nf_tproxy_ipv6|kernel/net/ipv6/netfilter/nf_tproxy_ipv6.ko||0
nft_tproxy|kernel/net/netfilter/nft_tproxy.ko||1
EOF

build_package \
	"kmod-nf-ipt" \
	"iptables compatibility core for DG2200TN kernel build 73" \
	"" "dg2200tn-nf-ipt-kmod" 29 <<'EOF'
x_tables|kernel/net/netfilter/x_tables.ko||0
ip_tables|kernel/net/ipv4/netfilter/ip_tables.ko||1
EOF

build_package \
	"kmod-ipt-core" \
	"iptables filter and common extensions for DG2200TN kernel build 73" \
	"kmod-nf-ipt" "dg2200tn-ipt-core-kmod" 30 <<'EOF'
x_tables|kernel/net/netfilter/x_tables.ko||0
ip_tables|kernel/net/ipv4/netfilter/ip_tables.ko||0
xt_tcpudp|kernel/net/netfilter/xt_tcpudp.ko||0
iptable_filter|kernel/net/ipv4/netfilter/iptable_filter.ko||1
iptable_mangle|kernel/net/ipv4/netfilter/iptable_mangle.ko||1
xt_limit|kernel/net/netfilter/xt_limit.ko||1
xt_mac|kernel/net/netfilter/xt_mac.ko||1
xt_multiport|kernel/net/netfilter/xt_multiport.ko||1
xt_comment|kernel/net/netfilter/xt_comment.ko||1
xt_LOG|kernel/net/netfilter/xt_LOG.ko||1
xt_TCPMSS|kernel/net/netfilter/xt_TCPMSS.ko||1
ipt_REJECT|kernel/net/ipv4/netfilter/ipt_REJECT.ko||1
xt_time|kernel/net/netfilter/xt_time.ko||1
xt_mark|kernel/net/netfilter/xt_mark.ko||1
EOF

build_package \
	"kmod-ipt-ipopt" \
	"iptables packet option extensions for DG2200TN kernel build 73" \
	"kmod-ipt-core" "dg2200tn-ipt-ipopt-kmod" 31 <<'EOF'
x_tables|kernel/net/netfilter/x_tables.ko||0
xt_dscp|kernel/net/netfilter/xt_dscp.ko||1
xt_DSCP|kernel/net/netfilter/xt_DSCP.ko||1
xt_length|kernel/net/netfilter/xt_length.ko||1
xt_statistic|kernel/net/netfilter/xt_statistic.ko||1
xt_tcpmss|kernel/net/netfilter/xt_tcpmss.ko||1
xt_CLASSIFY|kernel/net/netfilter/xt_CLASSIFY.ko||1
xt_ecn|kernel/net/netfilter/xt_ecn.ko||1
ipt_ECN|kernel/net/ipv4/netfilter/ipt_ECN.ko||1
xt_hl|kernel/net/netfilter/xt_hl.ko||1
xt_HL|kernel/net/netfilter/xt_HL.ko||1
EOF

build_package \
	"kmod-nft-compat" \
	"nftables iptables compatibility module for DG2200TN kernel build 73" \
	"kmod-nf-ipt" "dg2200tn-nft-compat-kmod" 32 <<'EOF'
nf_tables|kernel/net/netfilter/nf_tables.ko||0
x_tables|kernel/net/netfilter/x_tables.ko||0
nft_compat|kernel/net/netfilter/nft_compat.ko||1
EOF

LC_ALL=C sort -o "$BUNDLE_MANIFEST" "$BUNDLE_MANIFEST"
echo "Built network module bundle:"
cat "$BUNDLE_MANIFEST"

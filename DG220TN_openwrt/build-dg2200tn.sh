#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
DEVICE_ROOT="$(CDPATH= cd -- "$ROOT/.." && pwd)"
KERNEL="$DEVICE_ROOT/port/src/linux-6.12"
MKELF="$DEVICE_ROOT/port/mkelf.py"
JOBS="${JOBS:-$(nproc)}"
RUNNER_SOURCE="$DEVICE_ROOT/port/firmware/bcm63138-stock-source"
. "$DEVICE_ROOT/port/dg2200tn-kernel-build.env"

runner_source_ready()
{
	[ -d "$RUNNER_SOURCE" ] || return 1
	(
		cd "$RUNNER_SOURCE"
		# Validate the complete generated source files, not payload-only hashes.
		sha256sum --check --status <<'EOF'
a4ea8ce0f214d2309f646a74f3ac6f486f9514359b2d6c048bf0f73ded98d0ab  runner_fw_a.c
c5ad294d33ab1762cedcc1201ebbd48547f45acbed86367e0c7bde7c3e5a5e56  runner_fw_b.c
ae311b6d9cad2e6d468dfc6461cd2fc487e90657e4972f065e8ad70dc795de57  runner_fw_c.c
9ee351a92a5cd33f5f204d6217fc83edd97de4b6c73b7d10a474523928b3b84a  runner_fw_d.c
8103307c7c31fce83694653ab01f9bacbdcb86ee52ba9e5cf0e8f3d144e579f2  predict_runner_fw_a.c
1f498a184f7d52159242caac7ff76bcdd8b6400064e2f7c486f1cb1b2fc8c447  predict_runner_fw_b.c
fc564f5fe197cf4d42c7caf7ef858d4852c08a4efdcf4a2acc4f8321facabc55  predict_runner_fw_c.c
0d4c0a5a7f89ab47c65b113fe636b3c2f7edc75a8e7e00e39cf310aec95279c4  predict_runner_fw_d.c
EOF
	)
}

KERNEL_CONFIG_BACKUP=

restore_kernel_config()
{
	[ -n "$KERNEL_CONFIG_BACKUP" ] || return 0
	[ -f "$KERNEL_CONFIG_BACKUP" ] || return 0
	cp "$KERNEL_CONFIG_BACKUP" "$KERNEL/.config"
	rm -f "$KERNEL_CONFIG_BACKUP"
	KERNEL_CONFIG_BACKUP=
}

if ! runner_source_ready && [ -n "${DG2200TN_RDPA_KO:-}" ]; then
	python3 "$DEVICE_ROOT/port/prepare-stock-runner-firmware.py" \
		"$DG2200TN_RDPA_KO"
fi

if ! runner_source_ready; then
	echo "Missing or invalid local DG2200TN stock Runner firmware sources." >&2
	echo "Run port/prepare-stock-runner-firmware.py with the stock rdpa.ko," >&2
	echo "or set DG2200TN_RDPA_KO before invoking this build." >&2
	exit 1
fi

cd "$ROOT"

./scripts/feeds update -a
./scripts/feeds install -a

# The armsr CPU settings are customized for BCM63138, so discard OpenWrt's
# cached target metadata before resolving the build configuration.
rm -f tmp/.targetinfo tmp/info/.targetinfo-armsr \
	tmp/.kconfig-armsr tmp/.kconfig-armsr_armv7 \
	tmp/.config-target.in tmp/.config-target.in.prereq

cp dg2200tn.config .config
make defconfig

# An interrupted armsr image build can leave these sidecars behind, while the
# generic image recipes expect to create them from scratch.
rm -f \
	build_dir/target-arm_cortex-a9_musl_eabi/linux-armsr_armv7/tmp/openwrt-armsr-armv7-generic-ext4-combined-efi.img.gz.kernel \
	build_dir/target-arm_cortex-a9_musl_eabi/linux-armsr_armv7/tmp/openwrt-armsr-armv7-generic-squashfs-combined-efi.img.gz.kernel

make -j"$JOBS"

ROOTFS="$(find bin/targets/armsr/armv7 -name '*rootfs.cpio.gz' -print -quit)"
test -n "$ROOTFS"

mkdir -p build_dir/dg2200tn
ROOTFS_CPIO="$ROOT/build_dir/dg2200tn/rootfs.cpio"
ROOTFS_STAGE="$ROOT/build_dir/dg2200tn/rootfs-stage"

pack_rootfs()
{
	(
		cd "$ROOTFS_STAGE"
		find . -print0 |
			LC_ALL=C sort -z |
			cpio --null --create --format=newc --owner=0:0 --quiet
	) > "$ROOTFS_CPIO"
}

rm -rf "$ROOTFS_STAGE"
mkdir -p "$ROOTFS_STAGE"
gzip -dc "$ROOTFS" |
	(cd "$ROOTFS_STAGE" &&
		cpio --extract --make-directories --unconditional \
			--preserve-modification-time --no-absolute-filenames --quiet)

# OpenWrt's armsr modules target a different kernel release and cannot be
# loaded by this board-specific kernel. Keep only modules built below.
rm -rf "$ROOTFS_STAGE/lib/modules"
install -m 0644 "$DEVICE_ROOT/port/dg2200tn-conntrack.conf" \
	"$ROOTFS_STAGE/etc/sysctl.d/12-dg2200tn-conntrack.conf"
install -m 0755 "$DEVICE_ROOT/port/dg2200tn-wireless.init" \
	"$ROOTFS_STAGE/etc/init.d/dg2200tn-wireless"
ln -sf ../init.d/dg2200tn-wireless \
	"$ROOTFS_STAGE/etc/rc.d/S18dg2200tn-wireless"
install -m 0755 "$DEVICE_ROOT/port/dg2200tn-network-finalize.init" \
	"$ROOTFS_STAGE/etc/init.d/dg2200tn-network-finalize"
ln -sf ../init.d/dg2200tn-network-finalize \
	"$ROOTFS_STAGE/etc/rc.d/S99dg2200tn-network-finalize"
pack_rootfs

KERNEL_CONFIG_BACKUP="$(mktemp "$ROOT/build_dir/dg2200tn/kernel-config.XXXXXX")"
cp "$KERNEL/.config" "$KERNEL_CONFIG_BACKUP"
trap restore_kernel_config 0

"$KERNEL/scripts/config" --file "$KERNEL/.config" \
	--set-str INITRAMFS_SOURCE "$ROOTFS_CPIO" \
	--enable NETFILTER_XTABLES
"$DEVICE_ROOT/port/configure-network-kernel.sh" \
	"$KERNEL" "$KERNEL/.config"
make -s -C "$KERNEL" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- olddefconfig
cp "$KERNEL/.config" "$ROOT/build_dir/dg2200tn/kernel-tftp.config"
make -C "$KERNEL" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- \
	-j"$JOBS" vmlinux broadcom/bcm63138-dg2200tn.dtb

BUTTON_SRC="$ROOT/package/kernel/gpio-button-hotplug/src"
make -C "$KERNEL" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- \
	M="$BUTTON_SRC" clean
make -C "$KERNEL" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- \
	M="$BUTTON_SRC" modules

KERNEL_RELEASE="$(make -s -C "$KERNEL" ARCH=arm \
	CROSS_COMPILE=arm-linux-gnueabi- kernelrelease)"
mkdir -p "$ROOTFS_STAGE/lib/modules/$KERNEL_RELEASE" \
	"$ROOTFS_STAGE/etc/modules.d"
cp "$BUTTON_SRC/gpio-button-hotplug.ko" \
	"$ROOTFS_STAGE/lib/modules/$KERNEL_RELEASE/"
cp "$KERNEL/modules.builtin" "$KERNEL/modules.builtin.modinfo" \
	"$ROOTFS_STAGE/lib/modules/$KERNEL_RELEASE/"
touch "$ROOTFS_STAGE/lib/modules/$KERNEL_RELEASE/modules.order"
printf '%s\n' gpio-button-hotplug \
	> "$ROOTFS_STAGE/etc/modules.d/20-gpio-button-hotplug"
depmod -b "$ROOTFS_STAGE" "$KERNEL_RELEASE"
pack_rootfs
rm -rf "$ROOTFS_STAGE"
make -C "$KERNEL" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- \
	M="$BUTTON_SRC" clean

make -C "$KERNEL" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- \
	-j"$JOBS" zImage

mkdir -p bin/targets/bcm63138/dg2200tn
python3 "$MKELF" \
	--dtb "$KERNEL/arch/arm/boot/dts/broadcom/bcm63138-dg2200tn.dtb" \
	"$KERNEL/arch/arm/boot/zImage" \
	bin/targets/bcm63138/dg2200tn/openwrt-dg2200tn-tftp.elf

restore_kernel_config
trap - 0

echo "Created bin/targets/bcm63138/dg2200tn/openwrt-dg2200tn-tftp.elf"

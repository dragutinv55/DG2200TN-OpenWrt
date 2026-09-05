#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
DEVICE_ROOT="$(CDPATH= cd -- "$ROOT/.." && pwd)"
OPENWRT="$DEVICE_ROOT/DG220TN_openwrt"
KERNEL="$ROOT/src/linux-6.12"
HOST_BIN="$OPENWRT/staging_dir/host/bin"
JOBS="${JOBS:-$(nproc)}"
. "$ROOT/dg2200tn-kernel-build.env"

ERASE_SIZE=131072
BOOTFS_SIZE=4325376
JFFS2_SIZE=4194304
ROOTFS_SIZE=66846720
IMAGE_SIZE=71172096
UBIFS_LEB_SIZE=126976
UBIFS_MAX_LEBS=466

BUILD="$ROOT/build/stock-rui"
OUTPUT="$ROOT/images"
ROOTFS_TAR="$OPENWRT/bin/targets/armsr/armv7/openwrt-armsr-armv7-generic-rootfs.tar.gz"
KERNEL_CONFIG="$KERNEL/.config"
KERNEL_CONFIG_BACKUP="$ROOT/.config.stock-rui-backup"
KERNEL_CMDLINE="console=ttyS0,115200 earlyprintk ubi.mtd=ubi-bank2 root=ubi0:rootfs rootfstype=ubifs rw"

if [ "${RESCUE_DISABLE_BRCMFMAC:-0}" = 1 ]; then
	KERNEL_CMDLINE="$KERNEL_CMDLINE module_blacklist=brcmfmac"
fi

cleanup() {
	if [ -f "$KERNEL_CONFIG_BACKUP" ]; then
		cp "$KERNEL_CONFIG_BACKUP" "$KERNEL_CONFIG"
		rm -f "$KERNEL_CONFIG_BACKUP"
	fi
}
trap cleanup EXIT INT TERM

for tool in mkfs.jffs2 mkfs.ubifs ubinize; do
	test -x "$HOST_BIN/$tool" || {
		echo "error: missing $HOST_BIN/$tool; build OpenWrt first" >&2
		exit 1
	}
done
test -f "$ROOTFS_TAR" || {
	echo "error: missing $ROOTFS_TAR; run DG220TN_openwrt/build-dg2200tn.sh first" >&2
	exit 1
}
test -f "$ROOT/jffs2_out/cferam.001"

rm -rf "$BUILD"
mkdir -p "$BUILD/rootfs" "$BUILD/bootfs" "$OUTPUT"
rm -f "$OUTPUT/openwrt-dg2200tn-usb.rui" \
	"$OUTPUT/openwrt-dg2200tn-usb.rui.sha256"
cp "$KERNEL_CONFIG" "$KERNEL_CONFIG_BACKUP"

"$KERNEL/scripts/config" --file "$KERNEL_CONFIG" \
	--set-str INITRAMFS_SOURCE "" \
	--set-str CMDLINE "$KERNEL_CMDLINE" \
	--enable CMDLINE_FORCE \
	--disable KALLSYMS

# The RAM-boot development kernel uses a generic multi-platform configuration.
# Remove unrelated ARM SoCs so the CFE-compressed kernel fits in bootfs.
for option in \
	ARCH_VIRT ARCH_AIROHA ARCH_SUNPLUS ARCH_UNIPHIER ARCH_ACTIONS \
	ARCH_ALPINE ARCH_ARTPEC ARCH_ASPEED ARCH_AT91 ARCH_BERLIN \
	ARCH_DIGICOLOR ARCH_EXYNOS ARCH_HIGHBANK ARCH_HISI ARCH_HPE \
	ARCH_MXC ARCH_KEYSTONE ARCH_MEDIATEK ARCH_MESON ARCH_MILBEAUT \
	ARCH_MMP ARCH_MVEBU ARCH_OMAP ARCH_QCOM ARCH_ROCKCHIP \
	ARCH_RENESAS ARCH_INTEL_SOCFPGA ARCH_SPEAR13XX ARCH_STI ARCH_STM32 \
	ARCH_SUNXI ARCH_TEGRA ARCH_U8500 ARCH_VEXPRESS ARCH_VT8500 \
	ARCH_ZYNQ ARCH_BCM_IPROC ARCH_BCM_MOBILE ARCH_BCM2835 \
	ARCH_BCM_53573 ARCH_BRCMSTB ARCH_BCMBCA_CORTEXA7 \
	ARCH_BCMBCA_BRAHMAB15
do
	"$KERNEL/scripts/config" --file "$KERNEL_CONFIG" --disable "$option"
done

for option in \
	CFG80211 MAC80211 B43 BRCMFMAC BRCMUTIL BCMA SSB \
	NF_TABLES NETFILTER_XTABLES IP_NF_IPTABLES IP6_NF_IPTABLES \
	USB_STORAGE EXT4_FS VFAT_FS SQUASHFS NFS_FS IPV6 BRIDGE VLAN_8021Q
do
	"$KERNEL/scripts/config" --file "$KERNEL_CONFIG" --module "$option"
done

for option in \
	DRM SOUND MEDIA_SUPPORT IIO HID_SUPPORT ATA \
	ARCH_SUPPORTS_CFI_CLANG FUNCTION_TRACER DEBUG_KERNEL TRACING FTRACE \
	PERF_EVENTS BPF_SYSCALL EFI CAN CHROME_PLATFORMS \
	RPMSG TEE HWMON THERMAL WATCHDOG CPU_FREQ CPU_IDLE UBIFS_FS_ZSTD \
	ARM_UNWIND STACKTRACE DEBUG_BUGVERBOSE BLK_DEV_INITRD \
	MQ_IOSCHED_KYBER IOSCHED_BFQ AUDIT SECCOMP IO_URING CGROUPS \
	PRINTK_INDEX DEBUG_FS ZSTD_COMPRESS ZSTD_DECOMPRESS
do
	"$KERNEL/scripts/config" --file "$KERNEL_CONFIG" --disable "$option"
done
"$KERNEL/scripts/config" --file "$KERNEL_CONFIG" \
	--module SCSI \
	--module BLK_DEV_SD \
	--module USB_STORAGE \
	--enable NF_CONNTRACK \
	--enable NAMESPACES \
	--enable UTS_NS \
	--enable IPC_NS \
	--enable PID_NS \
	--enable BRIDGE_VLAN_FILTERING \
	--disable CFG80211_REQUIRE_SIGNED_REGDB \
	--disable CFG80211_USE_KERNEL_REGDB_KEYS \
	--set-str EXTRA_FIRMWARE "brcm/bcm43217_DG2200TN_map.bin" \
	--set-str EXTRA_FIRMWARE_DIR "$OPENWRT/files/lib/firmware"
"$ROOT/configure-network-kernel.sh" "$KERNEL" "$KERNEL_CONFIG"
make -C "$KERNEL" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- olddefconfig
cp "$KERNEL_CONFIG" "$BUILD/kernel-flash.config"
make -C "$KERNEL" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- \
	-j"$JOBS" zImage modules broadcom/bcm63138-dg2200tn.dtb

KERNEL_RELEASE="$(make -s -C "$KERNEL" ARCH=arm \
	CROSS_COMPILE=arm-linux-gnueabi- kernelrelease)"
BUTTON_SRC="$OPENWRT/package/kernel/gpio-button-hotplug/src"
make -C "$KERNEL" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- \
	M="$BUTTON_SRC" clean
make -C "$KERNEL" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- \
	M="$BUTTON_SRC" modules

tar -xzf "$ROOTFS_TAR" -C "$BUILD/rootfs"
# wpad cannot register its ubus objects inside this platform's ujail profile.
rm -f "$BUILD/rootfs/etc/capabilities/wpad.json"
rm -rf "$BUILD/rootfs/lib/modules"
make -C "$KERNEL" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- \
	INSTALL_MOD_PATH="$BUILD/rootfs" modules_install
rm -f "$BUILD/rootfs/lib/modules/$KERNEL_RELEASE/build" \
	"$BUILD/rootfs/lib/modules/$KERNEL_RELEASE/source"
mkdir -p "$BUILD/rootfs/lib/modules/$KERNEL_RELEASE" \
	"$BUILD/rootfs/etc/modules.d"
cp "$BUTTON_SRC/gpio-button-hotplug.ko" \
	"$BUILD/rootfs/lib/modules/$KERNEL_RELEASE/"
printf '%s\n' gpio-button-hotplug \
	> "$BUILD/rootfs/etc/modules.d/20-gpio-button-hotplug"
touch "$BUILD/rootfs/lib/modules/$KERNEL_RELEASE/modules.order"
cp "$KERNEL/modules.builtin" "$KERNEL/modules.builtin.modinfo" \
	"$BUILD/rootfs/lib/modules/$KERNEL_RELEASE/"
depmod -b "$BUILD/rootfs" "$KERNEL_RELEASE"
install -m 0644 "$ROOT/dg2200tn-conntrack.conf" \
	"$BUILD/rootfs/etc/sysctl.d/12-dg2200tn-conntrack.conf"
install -m 0755 "$ROOT/dg2200tn-wireless.init" \
	"$BUILD/rootfs/etc/init.d/dg2200tn-wireless"
ln -sf ../init.d/dg2200tn-wireless \
	"$BUILD/rootfs/etc/rc.d/S18dg2200tn-wireless"
install -m 0755 "$ROOT/dg2200tn-storage.init" \
	"$BUILD/rootfs/etc/init.d/dg2200tn-storage"
ln -sf ../init.d/dg2200tn-storage \
	"$BUILD/rootfs/etc/rc.d/S95dg2200tn-storage"
ln -sf ../init.d/dg2200tn-storage \
	"$BUILD/rootfs/etc/rc.d/K05dg2200tn-storage"
install -m 0755 "$ROOT/dg2200tn-network-finalize.init" \
	"$BUILD/rootfs/etc/init.d/dg2200tn-network-finalize"
ln -sf ../init.d/dg2200tn-network-finalize \
	"$BUILD/rootfs/etc/rc.d/S99dg2200tn-network-finalize"

"$HOST_BIN/mkfs.ubifs" -q -r "$BUILD/rootfs" \
	-m 2048 -e "$UBIFS_LEB_SIZE" -c "$UBIFS_MAX_LEBS" \
	-x zlib -o "$BUILD/rootfs.ubifs"
cat > "$BUILD/ubinize.cfg" <<EOF
[rootfs]
mode=ubi
image=$BUILD/rootfs.ubifs
vol_id=0
vol_type=dynamic
vol_name=rootfs
vol_flags=autoresize
EOF
"$HOST_BIN/ubinize" -o "$BUILD/rootfs.ubi" \
	-m 2048 -p "$ERASE_SIZE" -s 2048 "$BUILD/ubinize.cfg"
ROOTFS_ACTUAL="$(stat -c %s "$BUILD/rootfs.ubi")"
test "$ROOTFS_ACTUAL" -le "$ROOTFS_SIZE" || {
	echo "error: UBI image is too large ($ROOTFS_ACTUAL > $ROOTFS_SIZE)" >&2
	exit 1
}

python3 - "$BUILD/rootfs.ubi" "$ROOTFS_SIZE" "$ERASE_SIZE" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
target_size = int(sys.argv[2])
erase_size = int(sys.argv[3])
image = bytearray(path.read_bytes())
if len(image) % erase_size:
    raise SystemExit("error: UBI image is not eraseblock-aligned")
if len(image) > target_size:
    raise SystemExit("error: UBI image exceeds rootfs partition")

# Fill the unused partition with valid free UBI PEBs. Unlike raw 0xff
# padding, these blocks are written by the stock updater and erase stale
# vendor UBI volume/layout headers without reserving them for rootfs.
ec_header = bytes(image[:64])
free_peb = ec_header + b"\xff" * (erase_size - len(ec_header))
while len(image) < target_size:
    image += free_peb
if len(image) != target_size:
    raise SystemExit("error: rootfs size is not eraseblock-aligned")
path.write_bytes(image)
PY

python3 "$ROOT/mkcfeimg.py" \
	--dtb "$KERNEL/arch/arm/boot/dts/broadcom/bcm63138-dg2200tn.dtb" \
	"$KERNEL/arch/arm/boot/zImage" "$BUILD/bootfs/vmlinux.lz"
# CFE ignores a cferam directory entry with JFFS2 version 0. Ensure the
# alphabetically first dummy entry consumes version 0 instead.
touch "$BUILD/bootfs/1-openwrt"
arm-linux-gnueabi-gcc \
	-Os -march=armv7-a -marm -ffreestanding -fno-builtin \
	-fno-stack-protector -nostdlib -static -no-pie \
	-Wl,--build-id=none,-T,"$ROOT/cfe-uart-loader.ld" \
	-o "$BUILD/cfe-ethernet-autoboot.elf" \
	"$ROOT/cfe-ethernet-autoboot.S"
arm-linux-gnueabi-objcopy -O binary \
	"$BUILD/cfe-ethernet-autoboot.elf" \
	"$BUILD/cfe-ethernet-autoboot.bin"
python3 "$ROOT/patch-cfe-ethernet-autoboot.py" \
	"$ROOT/jffs2_out/cferam.001" \
	"$BUILD/cfe-ethernet-autoboot.bin" \
	"$BUILD/bootfs/cferam.001"
printf '%s\n' "99.0.0.0" > "$BUILD/bootfs/cfev.99.0.0.0"

"$HOST_BIN/mkfs.jffs2" -q -l -n -m none -e "$ERASE_SIZE" -s 2048 \
	-p "$JFFS2_SIZE" -r "$BUILD/bootfs" -o "$BUILD/boot.jffs2"
test "$(stat -c %s "$BUILD/boot.jffs2")" -eq "$JFFS2_SIZE"

python3 - "$BUILD/fs-transition.bin" "$ERASE_SIZE" <<'PY'
import sys
from pathlib import Path

output = bytearray(b"\xff" * int(sys.argv[2]))
marker = b"BcmFs-ubifs\x00"
for offset in range(0x1FF00, 0x1FF30, 12):
    output[offset:offset + len(marker)] = marker
Path(sys.argv[1]).write_bytes(output)
PY

cat "$BUILD/boot.jffs2" "$BUILD/fs-transition.bin" \
	"$BUILD/rootfs.ubi" > "$BUILD/flash-image.bin"
test "$(stat -c %s "$BUILD/flash-image.bin")" -eq "$IMAGE_SIZE"

python3 - "$BUILD/flash-image.bin" "$BUILD/hardco-image.bin" <<'PY'
import struct
import sys
import zlib
from pathlib import Path

image = Path(sys.argv[1]).read_bytes()
hardco_header = struct.pack(">HH", 0x5A5D, 0)
crc = struct.pack("<I", zlib.crc32(image) & 0xFFFFFFFF)
Path(sys.argv[2]).write_bytes(hardco_header + image + crc)
PY

RUI="$OUTPUT/openwrt-dg2200tn-usb.rui"
python3 "$ROOT/mkrui.py" create "$BUILD/hardco-image.bin" "$RUI"
python3 "$ROOT/mkrui.py" inspect "$RUI"
(cd "$OUTPUT" && sha256sum "$(basename "$RUI")" \
	> "$(basename "$RUI").sha256")

echo "Created $RUI"

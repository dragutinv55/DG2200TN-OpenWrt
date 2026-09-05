#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build/stock-rui"
OUTPUT="$ROOT/images"
BOOTSTRAP_SOURCE="$ROOT/cfe-uart-bootstrap.c"
LOADER_SOURCE="$ROOT/cfe-uart-loader.c"
ROOTFS_LOADER_SOURCE="$ROOT/cfe-rootfs-writer.c"
FLASH_IMAGE="$BUILD/flash-image.bin"
BOOTFS_IMAGE="$BUILD/boot.jffs2"
WFI="$OUTPUT/openwrt-dg2200tn-cfe.wfi"
REPAIR_WFI="$OUTPUT/openwrt-dg2200tn-cfe-bootfs-repair.wfi"
REPAIR_WFI_XZ="$REPAIR_WFI.xz"
BOOTSTRAP="$OUTPUT/dg2200tn-cfe-uart-bootstrap.bin"
LOADER="$OUTPUT/dg2200tn-cfe-uart-loader.bin"
ROOTFS_LOADER="$OUTPUT/dg2200tn-cfe-rootfs-loader.bin"
ROOTFS_IMAGE="$OUTPUT/openwrt-dg2200tn-cfe-rootfs.bin"
ROOTFS_IMAGE_XZ="$ROOTFS_IMAGE.xz"

test -f "$FLASH_IMAGE" || {
	echo "error: missing $FLASH_IMAGE; run port/build-stock-rui.sh first" >&2
	exit 1
}
test -f "$BOOTFS_IMAGE" || {
	echo "error: missing $BOOTFS_IMAGE; run port/build-stock-rui.sh first" >&2
	exit 1
}
command -v arm-linux-gnueabi-gcc >/dev/null
command -v arm-linux-gnueabi-objcopy >/dev/null
command -v xz >/dev/null
mkdir -p "$OUTPUT"

arm-linux-gnueabi-gcc \
	-Os -march=armv7-a -marm -ffreestanding -fno-builtin \
	-fno-stack-protector -nostdlib -static -no-pie \
	-DCFE_COMMAND_HOOK \
	-Wl,--build-id=none,-T,"$ROOT/cfe-uart-loader.ld" \
	-o "$BUILD/cfe-uart-bootstrap.elf" "$BOOTSTRAP_SOURCE"
arm-linux-gnueabi-objcopy -O binary \
	"$BUILD/cfe-uart-bootstrap.elf" "$BOOTSTRAP"

arm-linux-gnueabi-gcc \
	-Os -march=armv7-a -marm -ffreestanding -fno-builtin \
	-fno-stack-protector -nostdlib -static -no-pie \
	-DCFE_COMMAND_HOOK \
	-Wl,--build-id=none,-T,"$ROOT/cfe-uart-stage.ld" \
	-o "$BUILD/cfe-uart-loader.elf" "$LOADER_SOURCE"
arm-linux-gnueabi-objcopy -O binary \
	"$BUILD/cfe-uart-loader.elf" "$LOADER"

arm-linux-gnueabi-gcc \
	-Os -march=armv7-a -marm -ffreestanding -fno-builtin \
	-fno-stack-protector -nostdlib -static -no-pie \
	-Wl,--build-id=none,-T,"$ROOT/cfe-uart-loader.ld" \
	-o "$BUILD/cfe-rootfs-loader.elf" "$ROOTFS_LOADER_SOURCE"
arm-linux-gnueabi-objcopy -O binary \
	"$BUILD/cfe-rootfs-loader.elf" "$ROOTFS_LOADER"

python3 - "$FLASH_IMAGE" "$WFI" "$BOOTFS_IMAGE" "$REPAIR_WFI" <<'PY'
import binascii
from pathlib import Path
import struct
import sys

def create(source_name, output_name, expected_size):
    payload = Path(source_name).read_bytes()
    if len(payload) != expected_size:
        raise SystemExit(
            f"error: unexpected image size {len(payload):#x}, "
            f"expected {expected_size:#x}"
        )
    if payload[:2] != b"\x85\x19":
        raise SystemExit("error: image does not begin with JFFS2 magic")

    crc = (~binascii.crc32(payload)) & 0xFFFFFFFF
    # BCM63138 CFE runs little-endian ARM code and reads this tag natively.
    # The OpenWrt cfe-wfi-tag.py helper uses big-endian fields for BMIPS.
    tail = struct.pack("<IIIII", crc, 0x5732, 0x63138, 3, 0)
    Path(output_name).write_bytes(payload + tail)

create(sys.argv[1], sys.argv[2], 0x043E0000)
create(sys.argv[3], sys.argv[4], 0x00400000)
PY

python3 - "$FLASH_IMAGE" "$ROOTFS_IMAGE" <<'PY'
from pathlib import Path
import sys

erase_size = 0x20000
rootfs = Path(sys.argv[1]).read_bytes()[0x420000:]
last_used = 0
for offset in range(0, len(rootfs), erase_size):
    peb = rootfs[offset:offset + erase_size]
    if len(peb) != erase_size:
        raise SystemExit("error: rootfs is not eraseblock-aligned")
    if peb[0x800:0x804] == b"UBI!":
        last_used = offset + erase_size
if last_used < 3 * erase_size:
    raise SystemExit("error: rootfs UBI has too few populated eraseblocks")
Path(sys.argv[2]).write_bytes(rootfs[:last_used])
PY

xz -9e -T1 -c "$REPAIR_WFI" > "$REPAIR_WFI_XZ"
xz -9e -T1 -c "$ROOTFS_IMAGE" > "$ROOTFS_IMAGE_XZ"

(cd "$OUTPUT" && sha256sum \
	"$(basename "$BOOTSTRAP")" > "$(basename "$BOOTSTRAP").sha256")
(cd "$OUTPUT" && sha256sum \
	"$(basename "$LOADER")" > "$(basename "$LOADER").sha256")
(cd "$OUTPUT" && sha256sum \
	"$(basename "$ROOTFS_LOADER")" > "$(basename "$ROOTFS_LOADER").sha256")
(cd "$OUTPUT" && sha256sum \
	"$(basename "$ROOTFS_IMAGE_XZ")" > "$(basename "$ROOTFS_IMAGE_XZ").sha256")
(cd "$OUTPUT" && sha256sum \
	"$(basename "$WFI")" > "$(basename "$WFI").sha256")
(cd "$OUTPUT" && sha256sum \
	"$(basename "$REPAIR_WFI_XZ")" > "$(basename "$REPAIR_WFI_XZ").sha256")

echo "Created $BOOTSTRAP"
echo "Created $LOADER"
echo "Created $ROOTFS_LOADER"
echo "Created $ROOTFS_IMAGE_XZ"
echo "Created $WFI"
echo "Created $REPAIR_WFI_XZ"

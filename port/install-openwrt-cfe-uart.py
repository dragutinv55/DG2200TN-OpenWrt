#!/usr/bin/env python3

import argparse
import atexit
import binascii
import glob
import hashlib
import lzma
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import time

import serial


BAUD = 115200
TRANSFER_BAUD = 57600
TRANSFER_SYNC = b"\x55\xaa\x3c\xc3"
TRANSFER_BLOCK_SIZE = 1024
TRANSFER_FRAME_MAGIC = b"FRM!"
TRANSFER_ESTIMATED_RATE = 3.7 * 1024
TRANSFER_ACK = b"ACK!"
TRANSFER_NAK = b"NAK!"
TRANSFER_RETRIES = 10
ERASE_SIZE = 0x20000
EXPECTED_PAYLOAD_SIZES = {
    0x00400000: "bootfs",
    0x043E0000: "full image",
}
IMAGE_ADDRESS = 0x10000000
STAGE_ADDRESS = 0x0E000000
ROOTFS_FLASH_SIZE = 0x03FC0000
LOADER_ADDRESS = 0x00F00584
LOADER_ENTRY = LOADER_ADDRESS
LOADER_LIMIT = 0x400
GO_HANDLER = 0x00F21F4C
GO_HANDLER_INSTRUCTION = 0xE92D4800
GO_HANDLER_POINTER = 0x00F6CE04
NAND_WRITER = 0x00F1757C
NAND_WRITER_INSTRUCTIONS = (
    0xE92D4810,
    0xE28DB008,
    0xE24DDE43,
    0xE24DD004,
)
DCACHE_CLEAN = 0x00F363A4
ICACHE_INVALIDATE = 0x00F36448
WFI_VERSION = 0x00005732
WFI_CHIP_ID = 0x00063138
WFI_FLASH_TYPE = 3
WFI_FLAGS = 0


class TransferRejected(RuntimeError):
    pass


class SerialLink:
    def __init__(self, path):
        self.path = path
        self.port = None
        self.baudrate = BAUD
        self.pending = bytearray()

    def connect(self):
        while self.port is None:
            try:
                self.port = serial.Serial(
                    self.path,
                    self.baudrate,
                    timeout=0.05,
                    write_timeout=30,
                )
            except serial.SerialException:
                time.sleep(0.25)

    def close(self):
        if self.port is not None:
            self.port.close()
            self.port = None

    def read(self, size=4096):
        if self.pending:
            data = bytes(self.pending[:size])
            del self.pending[:size]
            return data
        self.connect()
        try:
            return self.port.read(size)
        except serial.SerialException:
            self.close()
            return b""

    def write(self, data):
        self.connect()
        try:
            self.port.write(data)
            self.port.flush()
        except serial.SerialException as error:
            self.close()
            raise RuntimeError("UART disconnected during a write") from error

    def reset_input(self):
        self.pending.clear()
        self.connect()
        self.port.reset_input_buffer()

    def unread(self, data):
        if data:
            self.pending[:0] = data

    def set_baudrate(self, baudrate):
        self.baudrate = baudrate
        self.connect()
        self.port.baudrate = baudrate


def autodetect_serial():
    devices = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
    if len(devices) != 1:
        raise RuntimeError(
            "specify --serial; expected exactly one /dev/ttyUSB* or /dev/ttyACM*"
        )
    return devices[0]


def materialize_artifact(path):
    path = path.resolve()
    if path.suffix != ".xz":
        return path

    suffix = Path(path.stem).suffix
    materialized = None
    try:
        with lzma.open(path, "rb") as source:
            with tempfile.NamedTemporaryFile(
                prefix="dg2200tn-",
                suffix=suffix,
                delete=False,
            ) as output:
                materialized = Path(output.name)
                shutil.copyfileobj(source, output, 1024 * 1024)
    except (OSError, lzma.LZMAError) as error:
        if materialized is not None:
            materialized.unlink(missing_ok=True)
        raise RuntimeError(f"cannot decompress {path}: {error}") from error

    atexit.register(materialized.unlink, missing_ok=True)
    return materialized


def crc32_file(path, length=None):
    crc = 0
    remaining = length
    with path.open("rb") as source:
        while remaining is None or remaining:
            size = 1024 * 1024 if remaining is None else min(1024 * 1024, remaining)
            chunk = source.read(size)
            if not chunk:
                break
            crc = binascii.crc32(chunk, crc)
            if remaining is not None:
                remaining -= len(chunk)
    if remaining not in (None, 0):
        raise RuntimeError("image ended while calculating CRC")
    return crc & 0xFFFFFFFF


def validate_wfi(path):
    size = path.stat().st_size
    payload_size = size - 20
    if payload_size not in EXPECTED_PAYLOAD_SIZES:
        raise RuntimeError(
            f"unexpected WFI size {size:#x}; expected "
            + " or ".join(f"{value + 20:#x}" for value in EXPECTED_PAYLOAD_SIZES)
        )
    with path.open("rb") as source:
        if source.read(2) != b"\x85\x19":
            raise RuntimeError("WFI payload does not begin with JFFS2 magic")
        source.seek(-20, os.SEEK_END)
        tail = source.read(20)
    crc, version, chip_id, flash_type, flags = struct.unpack("<IIIII", tail)
    expected = (WFI_VERSION, WFI_CHIP_ID, WFI_FLASH_TYPE, WFI_FLAGS)
    if (version, chip_id, flash_type, flags) != expected:
        raise RuntimeError(
            "unexpected WFI tag: "
            f"version={version:#x} chip={chip_id:#x} "
            f"flash_type={flash_type} flags={flags:#x}"
        )
    payload_crc = (~crc32_file(path, payload_size)) & 0xFFFFFFFF
    if crc != payload_crc:
        raise RuntimeError(
            f"invalid WFI CRC {crc:#010x}; expected {payload_crc:#010x}"
        )
    return (
        size,
        crc32_file(path),
        hashlib.sha256(path.read_bytes()).hexdigest(),
        EXPECTED_PAYLOAD_SIZES[payload_size],
    )


def validate_rootfs(path):
    size = path.stat().st_size
    if size < 3 * ERASE_SIZE or size > ROOTFS_FLASH_SIZE:
        raise RuntimeError(f"unexpected compact rootfs size {size:#x}")
    if size % ERASE_SIZE:
        raise RuntimeError("compact rootfs is not eraseblock-aligned")
    image = path.read_bytes()
    for offset in range(0, size, ERASE_SIZE):
        peb = image[offset : offset + ERASE_SIZE]
        if peb[:4] != b"UBI#" or peb[0x800:0x804] != b"UBI!":
            raise RuntimeError(
                f"compact rootfs has an invalid UBI PEB at {offset:#x}"
            )
    if image[0x1010:0x1016] != b"rootfs":
        raise RuntimeError("compact rootfs has no rootfs volume record")
    return (
        size,
        crc32_file(path),
        hashlib.sha256(image).hexdigest(),
    )


def wait_for(link, marker, timeout, echo=True):
    output = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        data = link.read()
        if data:
            output += data
            if echo:
                print(data.decode("latin1", "replace"), end="", flush=True)
            if marker in output:
                return output
    raise RuntimeError(f"timed out waiting for {marker!r}")


def wait_for_cfe(link):
    output = b""
    deadline = time.monotonic() + 600
    while time.monotonic() < deadline:
        try:
            link.write(b"\r")
        except RuntimeError:
            time.sleep(0.25)
            continue
        data = link.read()
        if data:
            output = (output + data)[-32768:]
            print(data.decode("latin1", "replace"), end="", flush=True)
            if b"CFE>" in output:
                return output
        time.sleep(0.015)
    raise RuntimeError("CFE prompt was not reached")


def command(link, text, timeout=5):
    link.reset_input()
    link.write(text.encode() + b"\r")
    output = b""
    deadline = time.monotonic() + timeout
    idle_since = None
    while time.monotonic() < deadline:
        data = link.read()
        if data:
            output += data
            idle_since = time.monotonic()
            if b"CFE>" in output:
                return output
        elif idle_since is not None and time.monotonic() - idle_since >= 0.3:
            return output
    raise RuntimeError(f"timed out running CFE command {text!r}")


def parse_words(output):
    words = []
    for line in output.decode("latin1", "replace").splitlines():
        match = re.match(r"^[0-9a-fA-F]{8}:((?: [0-9a-fA-F]{8})+)", line)
        if match:
            words.extend(int(value, 16) for value in match.group(1).split())
    return words


def read_words(link, address, count):
    words = []
    while len(words) < count:
        chunk_count = min(4, count - len(words))
        chunk_address = address + len(words) * 4
        chunk = []
        for _ in range(5):
            try:
                output = command(link, f"dw {chunk_address:x} {chunk_count}")
            except RuntimeError:
                continue
            chunk = parse_words(output)
            if len(chunk) >= chunk_count:
                break
        if len(chunk) < chunk_count:
            raise RuntimeError(
                f"short CFE memory read at {chunk_address:#x} after retries"
            )
        words.extend(chunk[:chunk_count])
    return words


def write_word(link, address, value):
    link.reset_input()
    link.write(f"sm {address:x} 0x{value:08x} 4\r".encode())
    time.sleep(0.1)


def run_quiet_command(link, text):
    link.reset_input()
    link.write(text.encode() + b"\r")
    time.sleep(0.1)


def activate_loader(link, address):
    write_word(link, GO_HANDLER_POINTER, DCACHE_CLEAN)
    run_quiet_command(link, "go")
    write_word(link, GO_HANDLER_POINTER, ICACHE_INVALIDATE)
    run_quiet_command(link, "go")
    write_word(link, GO_HANDLER_POINTER, address)
    if read_words(link, GO_HANDLER_POINTER, 1)[0] != address:
        raise RuntimeError("failed to activate the verified UART loader")


def install_loader(link, loader):
    if len(loader) > LOADER_LIMIT:
        raise RuntimeError("UART loader does not fit the verified CFE executable gap")
    padded = loader + b"\0" * (-len(loader) % 4)
    expected = list(struct.unpack(f"<{len(padded) // 4}I", padded))
    pending = list(range(len(expected)))
    for _ in range(5):
        for index in pending:
            write_word(link, LOADER_ADDRESS + index * 4, expected[index])
        actual = read_words(link, LOADER_ADDRESS, len(expected))
        pending = [
            index
            for index, (wanted, found) in enumerate(zip(expected, actual))
            if wanted != found
        ]
        if not pending:
            break
    if pending:
        raise RuntimeError("CFE UART loader readback verification failed")
    activate_loader(link, LOADER_ENTRY)


def select_stage_receiver(link):
    write_word(link, GO_HANDLER_POINTER, STAGE_ADDRESS)
    if read_words(link, GO_HANDLER_POINTER, 1)[0] != STAGE_ADDRESS:
        raise RuntimeError("failed to select the verified stage receiver")


def restore_go_handler(link):
    write_word(link, GO_HANDLER_POINTER, GO_HANDLER)
    if read_words(link, GO_HANDLER_POINTER, 1)[0] != GO_HANDLER:
        raise RuntimeError("failed to restore the CFE go command handler")


def enter_transfer_baud(link, marker):
    console_baudrate = link.baudrate
    link.set_baudrate(TRANSFER_BAUD)
    link.reset_input()
    time.sleep(0.05)
    link.write(TRANSFER_SYNC)
    wait_for(link, marker, 10)
    return console_baudrate


def finish_transfer(link, marker, console_baudrate):
    output = wait_for(link, marker, 60)
    time.sleep(0.1)
    link.set_baudrate(console_baudrate)
    return output + command(link, "", 10)


def wait_for_block_response(link, block, timeout=10):
    output = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        data = link.read()
        if not data:
            continue
        output += data
        for offset in range(max(0, len(output) - len(data) - 11),
                            len(output) - 11):
            magic = output[offset : offset + 4]
            if magic not in (TRANSFER_ACK, TRANSFER_NAK):
                continue
            response_block, inverse = struct.unpack_from("<II", output, offset + 4)
            if response_block != block or inverse != (block ^ 0xFFFFFFFF):
                continue
            link.unread(output[offset + 12 :])
            return magic == TRANSFER_ACK
        output = output[-4096:]
    raise RuntimeError(f"timed out waiting for transfer block {block}")


def transfer_blocks(link, image, size):
    sent = 0
    block = 0
    started = time.monotonic()
    next_report = started
    with image.open("rb") as source:
        while sent < size:
            chunk = source.read(min(TRANSFER_BLOCK_SIZE, size - sent))
            if not chunk:
                raise RuntimeError("image ended during UART transfer")
            chunk_crc = binascii.crc32(chunk) & 0xFFFFFFFF
            frame = TRANSFER_FRAME_MAGIC + struct.pack(
                "<IIII",
                block,
                block ^ 0xFFFFFFFF,
                chunk_crc,
                chunk_crc ^ 0xFFFFFFFF,
            )
            for attempt in range(1, TRANSFER_RETRIES + 1):
                time.sleep(0.025)
                link.reset_input()
                link.write(frame)
                link.write(chunk)
                try:
                    accepted = wait_for_block_response(link, block)
                except RuntimeError:
                    accepted = False
                if accepted:
                    break
                print(
                    f"\nRetrying transfer block {block} "
                    f"({attempt}/{TRANSFER_RETRIES})",
                    flush=True,
                )
            else:
                raise RuntimeError(
                    f"router did not accept transfer block {block}"
                )

            sent += len(chunk)
            block += 1
            now = time.monotonic()
            if now >= next_report or sent == size:
                elapsed = max(now - started, 0.001)
                rate = sent / elapsed
                remaining = (size - sent) / rate if rate else 0
                print(
                    f"\rTransferred {sent / 1048576:.1f}/{size / 1048576:.1f} MiB "
                    f"({sent * 100 / size:5.1f}%), "
                    f"{rate / 1024:.1f} KiB/s, ETA {remaining / 60:.1f} min",
                    end="",
                    flush=True,
                )
                next_report = now + 1
        if source.read(1):
            raise RuntimeError("image size changed after validation")
    print()


def transfer_stage_receiver(link, receiver):
    size = len(receiver)
    if size < 64 or size > 0x10000:
        raise RuntimeError("stage receiver has an invalid size")
    transfer_crc = binascii.crc32(receiver) & 0xFFFFFFFF
    link.reset_input()
    link.write(b"go\r")
    wait_for(link, b"DG2200TN_READY", 10)
    link.write(
        struct.pack(
            "<IIII",
            size,
            size ^ 0xFFFFFFFF,
            transfer_crc,
            transfer_crc ^ 0xFFFFFFFF,
        )
    )
    wait_for(link, b"DG2200TN_HEADER_OK", 10)
    console_baudrate = enter_transfer_baud(link, b"DG2200TN_B")
    link.write(receiver)
    output = finish_transfer(link, b"DG2200TN_D", console_baudrate)
    if b"DG2200TN_CRC_OK" not in output:
        raise TransferRejected("router rejected the stage receiver CRC")


def transfer_image(link, image, size, transfer_crc):
    link.reset_input()
    link.write(b"go\r")
    wait_for(link, b"DG2200TN_READY", 10)
    link.write(
        struct.pack(
            "<IIII",
            size,
            size ^ 0xFFFFFFFF,
            transfer_crc,
            transfer_crc ^ 0xFFFFFFFF,
        )
    )
    wait_for(link, b"DG2200TN_HEADER_OK", 10)
    console_baudrate = enter_transfer_baud(link, b"DG2200TN_B")
    transfer_blocks(link, image, size)
    output = finish_transfer(link, b"DG2200TN_D", console_baudrate)
    if b"DG2200TN_CRC_OK" not in output:
        raise TransferRejected("router rejected the UART transfer CRC")


def transfer_rootfs(link, image, size, transfer_crc):
    link.reset_input()
    link.write(b"go\r")
    wait_for(link, b"DG2200TN_READY", 10)
    link.write(
        struct.pack(
            "<IIII",
            size,
            size ^ 0xFFFFFFFF,
            transfer_crc,
            transfer_crc ^ 0xFFFFFFFF,
        )
    )
    wait_for(link, b"DG2200TN_HEADER_OK", 10)
    console_baudrate = enter_transfer_baud(link, b"DG2200TN_B")
    transfer_blocks(link, image, size)
    output = finish_transfer(link, b"DG2200TN_D", console_baudrate)
    if b"DG2200TN_CRC_OK" not in output:
        raise TransferRejected("router rejected the rootfs transfer CRC")


def monitor_cfe_write(link):
    deadline = time.monotonic() + 1800
    output = b""
    while time.monotonic() < deadline:
        data = link.read()
        if not data:
            continue
        output = (output + data)[-32768:]
        print(data.decode("latin1", "replace"), end="", flush=True)
        lower = output.lower()
        if b"illegal whole flash image" in lower or b"failed to flash" in lower:
            raise RuntimeError("CFE rejected or failed to write the WFI image")
        statuses = re.findall(rb"\*\*\* command status = (-?\d+)", output)
        if statuses and int(statuses[-1]) != 0:
            raise RuntimeError(
                f"CFE NAND writer returned status {int(statuses[-1])}"
            )
        if b"*** command status = 0" in output and b"CFE>" in output:
            return
    raise RuntimeError("timed out waiting for CFE flashing and reboot")


def monitor_rootfs_write(link):
    output = wait_for(link, b"CFE>", 1800)
    if b"DG2200TN_ROOTFS_WRITE_OK" not in output:
        raise RuntimeError("CFE lower-level rootfs writer failed")


def monitor_openwrt(link):
    output = b""
    deadline = time.monotonic() + 300
    while time.monotonic() < deadline:
        data = link.read()
        if not data:
            continue
        output = (output + data)[-65536:]
        print(data.decode("latin1", "replace"), end="", flush=True)
        lower = output.lower()
        if b"kernel panic" in lower:
            raise RuntimeError("OpenWrt kernel panicked during first boot")
        if (
            b"please press enter to activate this console" in lower
            or b"root@openwrt:" in lower
        ):
            return
    raise RuntimeError("timed out waiting for the OpenWrt console")


def reset_into_openwrt(link):
    link.reset_input()
    link.write(b"reset\r")
    time.sleep(0.05)
    link.set_baudrate(BAUD)
    link.reset_input()
    monitor_openwrt(link)


def load_stage_receiver(link, bootstrap, receiver):
    install_loader(link, bootstrap)
    transfer_stage_receiver(link, receiver)
    activate_loader(link, STAGE_ADDRESS)


def main():
    root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description="Install DG2200TN OpenWrt through UART and native CFE ws"
    )
    parser.add_argument("--serial", help="UART device, for example /dev/ttyUSB0")
    parser.add_argument(
        "--bootstrap",
        type=Path,
        default=root / "images" / "dg2200tn-cfe-uart-bootstrap.bin",
    )
    parser.add_argument(
        "--image",
        type=Path,
        default=root / "images" / "openwrt-dg2200tn-cfe-bootfs-repair.wfi.xz",
    )
    parser.add_argument(
        "--loader",
        type=Path,
        default=root / "images" / "dg2200tn-cfe-uart-loader.bin",
    )
    parser.add_argument(
        "--rootfs-image",
        type=Path,
        default=root / "images" / "openwrt-dg2200tn-cfe-rootfs.bin.xz",
    )
    parser.add_argument(
        "--rootfs-loader",
        type=Path,
        default=root / "images" / "dg2200tn-cfe-rootfs-loader.bin",
    )
    install_mode = parser.add_mutually_exclusive_group()
    install_mode.add_argument(
        "--bootfs-only",
        action="store_true",
        help=(
            "write only bank-2 bootfs; CFE also reinitializes bank-2 UBI, "
            "so rootfs must be rewritten afterward"
        ),
    )
    install_mode.add_argument(
        "--rootfs-only",
        action="store_true",
        help="write only bank-2 rootfs, preserving the existing bootfs",
    )
    parser.add_argument(
        "--acknowledge-bootfs-erases-rootfs",
        action="store_true",
        help=(
            "required with --bootfs-only to acknowledge that CFE erases "
            "the bank-2 rootfs UBI metadata"
        ),
    )
    args = parser.parse_args()

    if args.bootfs_only and not args.acknowledge_bootfs_erases_rootfs:
        parser.error(
            "--bootfs-only reinitializes the bank-2 rootfs; also pass "
            "--acknowledge-bootfs-erases-rootfs only for staged recovery"
        )
    if args.acknowledge_bootfs_erases_rootfs and not args.bootfs_only:
        parser.error(
            "--acknowledge-bootfs-erases-rootfs requires --bootfs-only"
        )

    serial_path = args.serial or autodetect_serial()
    bootfs_image = materialize_artifact(args.image)
    size, wfi_crc, sha256, image_type = validate_wfi(bootfs_image)
    if image_type != "bootfs":
        raise RuntimeError("the staged installer requires the 4 MiB bootfs WFI")
    bootstrap = args.bootstrap.read_bytes()
    loader = args.loader.read_bytes()
    if args.bootfs_only:
        rootfs_size = 0
        rootfs_crc = 0
        rootfs_sha256 = ""
        rootfs_loader = b""
    else:
        rootfs_image = materialize_artifact(args.rootfs_image)
        rootfs_size, rootfs_crc, rootfs_sha256 = validate_rootfs(
            rootfs_image
        )
        rootfs_loader = args.rootfs_loader.read_bytes()
    if size % ERASE_SIZE != 20:
        raise RuntimeError("WFI payload is not NAND eraseblock-aligned")

    print("DG2200TN CFE UART installer")
    print(f"  UART:          {serial_path} ({BAUD} 8N1)")
    print(f"  Bootfs:        {args.image}")
    print(f"  Bootfs SHA256: {sha256}")
    if args.bootfs_only:
        print("  Rootfs:        CFE will reinitialize bank-2 UBI")
    else:
        print(f"  Rootfs:        {args.rootfs_image}")
        print(f"  Rootfs SHA256: {rootfs_sha256}")
    transfer_size = rootfs_size if args.rootfs_only else size + rootfs_size
    print(
        f"  Transfer:      {transfer_size / 1048576:.1f} MiB, approximately "
        f"{transfer_size / TRANSFER_ESTIMATED_RATE / 60:.0f} minutes"
    )
    print()
    print("Connect only GND, TX, and RX to a 3.3 V TTL adapter.")
    print("Do not connect the UART adapter VCC pin.")
    input("Press Enter, then immediately power-cycle the router: ")

    link = SerialLink(serial_path)
    required = (
        b"Chip ID: BCM63138B0",
        b"NAND ECC BCH-4",
        b"page size 0x800",
        b"block 128KB",
        b"size 262144KB",
        b"Board Id (0-25)                   : DG2200TN",
    )
    banner = bytearray(wait_for_cfe(link))
    missing = [text.decode() for text in required if text not in banner]
    banner_attempts = 8
    for attempt in range(banner_attempts):
        if not missing:
            break
        print(
            "CFE banner was incomplete or corrupted; resetting to recapture "
            f"it ({attempt + 1}/{banner_attempts})."
        )
        link.reset_input()
        link.write(b"reset\r")
        banner.extend(wait_for_cfe(link))
        missing = [text.decode() for text in required if text not in banner]
    if missing:
        raise RuntimeError("unexpected CFE/device geometry: " + ", ".join(missing))
    if read_words(link, GO_HANDLER, 1)[0] != GO_HANDLER_INSTRUCTION:
        raise RuntimeError("unsupported or modified CFE go handler")
    if read_words(link, GO_HANDLER_POINTER, 1)[0] != GO_HANDLER:
        raise RuntimeError("unexpected CFE go command pointer")
    if tuple(read_words(link, NAND_WRITER, 4)) != NAND_WRITER_INSTRUCTIONS:
        raise RuntimeError("unexpected CFE lower-level NAND writer")

    if args.rootfs_only:
        print("The existing bank-2 bootfs will be preserved.")
        if input("Type INSTALL to start the rootfs transfer: ") != "INSTALL":
            print("Installation cancelled; NAND remains unchanged.")
            return
        print("Injecting and verifying the UART bootstrap...")
        try:
            load_stage_receiver(link, bootstrap, loader)
        except (RuntimeError, OSError, serial.SerialException) as error:
            raise RuntimeError(
                "CFE loader setup was interrupted. No NAND write was started. "
                "Power-cycle the router before retrying."
            ) from error
    else:
        print("Injecting and verifying the UART bootstrap...")
        try:
            load_stage_receiver(link, bootstrap, loader)
        except (RuntimeError, OSError, serial.SerialException) as error:
            raise RuntimeError(
                "CFE loader setup was interrupted. No NAND write was started. "
                "Power-cycle the router before retrying."
            ) from error
        try:
            transfer_image(link, bootfs_image, size, wfi_crc)
        except TransferRejected:
            restore_go_handler(link)
            raise
        except (RuntimeError, OSError, serial.SerialException) as error:
            raise RuntimeError(
                "UART transfer was interrupted while the binary receiver may "
                "still be active. No NAND write was started. Power-cycle the "
                "router before retrying; do not send additional serial "
                "commands."
            ) from error
        restore_go_handler(link)

        print()
        print("The bootfs image is in RAM and its UART CRC was verified.")
        print("NAND has not been modified.")
        confirmation = "ERASE-ROOTFS" if args.bootfs_only else "INSTALL"
        if input(f"Type {confirmation} to start the NAND write: ") != (
            confirmation
        ):
            print("Installation cancelled; NAND remains unchanged.")
            return

        print("Writing bank-2 bootfs through CFE. Do not disconnect power.")
        link.reset_input()
        link.write(f"ws {IMAGE_ADDRESS:x} {size:x}\r".encode())
        monitor_cfe_write(link)

        if args.bootfs_only:
            print(
                "Bootfs completed. CFE reinitialized bank-2 UBI; keep the "
                "router at CFE and run --rootfs-only next."
            )
            return

    print("Injecting the compact rootfs receiver...")
    write_word(link, 0x00F0097C, 0)
    write_word(link, 0x00F00980, 0)
    select_stage_receiver(link)
    try:
        transfer_rootfs(link, rootfs_image, rootfs_size, rootfs_crc)
    except TransferRejected:
        restore_go_handler(link)
        raise
    except (RuntimeError, OSError, serial.SerialException) as error:
        raise RuntimeError(
            "rootfs transfer was interrupted after bootfs was written. "
            "Power-cycle, stop CFE autoboot, and rerun this installer."
        ) from error

    print("Rootfs CRC verified. Writing only bank-2 rootfs_update.")
    restore_go_handler(link)
    install_loader(link, rootfs_loader)
    link.reset_input()
    link.write(b"go\r")
    monitor_rootfs_write(link)
    restore_go_handler(link)

    print("Both NAND stages completed; resetting into OpenWrt.")
    reset_into_openwrt(link)
    print("\nOpenWrt installation completed successfully.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print(
            "\nInterrupted. If transfer had started, send nothing else and "
            "power-cycle the router before retrying.",
            file=sys.stderr,
        )
        raise SystemExit(130)
    except (RuntimeError, OSError, serial.SerialException) as error:
        print(f"\nERROR: {error}", file=sys.stderr)
        raise SystemExit(1)

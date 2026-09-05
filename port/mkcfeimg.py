#!/usr/bin/env python3
"""
Package an ARM zImage into the vmlinux.lz format that the DG2200TN's
CFE 1.0.38 expects.

Layout, reverse engineered from the stock vmlinux.lz found in the JFFS2
boot partition of the NAND dump:

    offset  size  field
    0x00    4     load address   (0xc0008000)
    0x04    4     entry point    (0xc0008000)
    0x08    4     payload length (bytes after this 20-byte header)
    0x0c    4     magic          ("BRCM")
    0x10    4     reserved       (0)
    0x14    ...   LZMA "alone" stream

The stock image uses LZMA properties byte 0x6d, i.e. lc=1, lp=2, pb=2 with
a 4 MiB dictionary, rather than the 0x5d (lc=3, lp=0, pb=2) that the lzma
CLI emits by default. Broadcom's in-CFE decoder is built for those values,
so we must match them exactly or decompression fails.
"""

import argparse
import struct
import subprocess
import sys

BRCM_MAGIC = b"BRCM"
HEADER_LEN = 20
DEFAULT_LOAD = 0xC0008000
EXPECTED_PROPS = 0x6D  # lc=1, lp=2, pb=2
DICT_SIZE = 4 << 20


def lzma_compress(payload: bytes) -> bytes:
    """Compress with the exact LZMA parameters CFE's decoder expects."""
    try:
        return subprocess.run(
            [
                "xz", "--format=lzma", "--stdout",
                f"--lzma1=preset=6,lc=1,lp=2,pb=2,dict={DICT_SIZE}",
            ],
            input=payload,
            stdout=subprocess.PIPE,
            check=True,
        ).stdout
    except FileNotFoundError:
        sys.exit("error: xz not found; install the xz-utils package")
    except subprocess.CalledProcessError as exc:
        sys.exit(f"error: xz failed with exit code {exc.returncode}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("zimage", help="ARM zImage to package")
    parser.add_argument("output", help="path to write vmlinux.lz to")
    parser.add_argument("--dtb", help="device tree blob to append to the zImage")
    parser.add_argument(
        "--load",
        type=lambda v: int(v, 0),
        default=DEFAULT_LOAD,
        help="load and entry address (default: 0x%08x)" % DEFAULT_LOAD,
    )
    args = parser.parse_args()

    with open(args.zimage, "rb") as fh:
        payload = fh.read()

    # CONFIG_ARM_APPENDED_DTB makes the zImage decompressor look for a DTB
    # immediately after its own end, so the blob simply gets concatenated.
    if args.dtb:
        with open(args.dtb, "rb") as fh:
            dtb = fh.read()
        if dtb[:4] != b"\xd0\x0d\xfe\xed":
            sys.exit(f"error: {args.dtb} is not a device tree blob")
        payload += dtb

    compressed = lzma_compress(payload)
    compressed = compressed[:5] + struct.pack("<Q", len(payload)) + compressed[13:]

    props = compressed[0]
    if props != EXPECTED_PROPS:
        sys.exit(
            f"error: got LZMA properties byte 0x{props:02x}, expected "
            f"0x{EXPECTED_PROPS:02x}; CFE would refuse this image"
        )

    header = struct.pack(
        "<III4sI", args.load, args.load, len(compressed), BRCM_MAGIC, 0
    )
    with open(args.output, "wb") as fh:
        fh.write(header + compressed)

    print(f"zImage      : {len(payload):>9} bytes (with DTB)" if args.dtb
          else f"zImage      : {len(payload):>9} bytes")
    print(f"compressed  : {len(compressed):>9} bytes")
    print(f"load / entry: 0x{args.load:08x}")
    print(f"wrote       : {args.output} ({HEADER_LEN + len(compressed)} bytes)")


if __name__ == "__main__":
    main()

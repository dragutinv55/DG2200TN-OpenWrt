#!/usr/bin/env python3
"""Patch fixed-length password hashes in UBIFS data nodes and repair CRCs."""

import argparse
import struct
import zlib

UBIFS_NODE_MAGIC = b"\x31\x18\x10\x06"


def containing_node(image: bytearray, payload_offset: int) -> tuple[int, int]:
    search_end = payload_offset
    while True:
        start = image.rfind(UBIFS_NODE_MAGIC, max(0, search_end - 65536), search_end)
        if start < 0:
            raise ValueError(f"no UBIFS node contains offset {payload_offset}")
        length = struct.unpack_from("<I", image, start + 16)[0]
        if length >= 24 and start + length > payload_offset:
            return start, length
        search_end = start


def replace_all(image: bytearray, old: bytes, new: bytes) -> None:
    if len(old) != len(new):
        raise ValueError("replacement must have exactly the same length")
    offsets = []
    offset = 0
    while True:
        offset = image.find(old, offset)
        if offset < 0:
            break
        offsets.append(offset)
        offset += len(old)
    if not offsets:
        raise ValueError("old value was not found")

    for offset in offsets:
        node_start, node_len = containing_node(image, offset)
        image[offset : offset + len(old)] = new

        body = image[node_start + 8 : node_start + node_len]
        crc = zlib.crc32(body) ^ 0xFFFFFFFF
        struct.pack_into("<I", image, node_start + 4, crc)

        stored = struct.unpack_from("<I", image, node_start + 4)[0]
        if stored != (zlib.crc32(body) ^ 0xFFFFFFFF):
            raise ValueError("CRC verification failed")

        print(
            f"patched payload at 0x{offset:x}, node 0x{node_start:x}, "
            f"length {node_len}, CRC 0x{crc:08x}"
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("old_passwd")
    parser.add_argument("new_passwd")
    parser.add_argument("old_shadow")
    parser.add_argument("new_shadow")
    args = parser.parse_args()

    image = bytearray(open(args.input, "rb").read())
    replace_all(image, args.old_passwd.encode(), args.new_passwd.encode())
    replace_all(image, args.old_shadow.encode(), args.new_shadow.encode())
    open(args.output, "wb").write(image)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Create and inspect unsigned SoftAtHome RUI firmware containers."""

import argparse
import struct
from pathlib import Path


RUI_MAGIC = b"rui\x00"

TAG_PROJECT = 0
TAG_HARDWARE = 1
TAG_CONFIG_TYPE = 2
TAG_VERSION = 3
TAG_IMAGE_TYPE = 4
TAG_IMAGE = 5
TAG_NAME = 7
TAG_COMPONENT_VERSION = 8
TAG_REBOOT = 10


def tag(tag_id: int, value: bytes) -> bytes:
    limit = 0x8000000 if tag_id == TAG_IMAGE else 0x200
    if len(value) >= limit:
        raise ValueError(f"tag {tag_id} is too large")
    return struct.pack(">II", tag_id, len(value)) + value


def string_tag(tag_id: int, value: str) -> bytes:
    return tag(tag_id, value.encode("ascii"))


def uint_tag(tag_id: int, value: int) -> bytes:
    return tag(tag_id, struct.pack(">I", value))


def create(args: argparse.Namespace) -> None:
    image = Path(args.image).read_bytes()
    output = bytearray(RUI_MAGIC)
    output += string_tag(TAG_PROJECT, args.project)
    output += string_tag(TAG_HARDWARE, args.hardware)
    output += string_tag(TAG_CONFIG_TYPE, args.config_type)
    output += string_tag(TAG_VERSION, args.version)
    output += uint_tag(TAG_IMAGE_TYPE, args.image_type)
    output += tag(TAG_IMAGE, image)
    output += string_tag(TAG_NAME, args.name)
    output += string_tag(TAG_COMPONENT_VERSION, args.component_version)
    output += uint_tag(TAG_REBOOT, 1)
    Path(args.output).write_bytes(output)
    print(f"wrote {args.output} ({len(output)} bytes)")


def inspect(args: argparse.Namespace) -> None:
    data = Path(args.input).read_bytes()
    if data[:4] != RUI_MAGIC:
        raise SystemExit("error: invalid RUI magic")

    names = {
        TAG_PROJECT: "project",
        TAG_HARDWARE: "hardware",
        TAG_CONFIG_TYPE: "config-type",
        TAG_VERSION: "version",
        TAG_IMAGE_TYPE: "image-type",
        TAG_IMAGE: "image",
        TAG_NAME: "name",
        TAG_COMPONENT_VERSION: "component-version",
        TAG_REBOOT: "reboot",
    }
    offset = 4
    while offset < len(data):
        if offset + 8 > len(data):
            raise SystemExit("error: truncated tag header")
        tag_id, length = struct.unpack_from(">II", data, offset)
        offset += 8
        end = offset + length
        if end > len(data):
            raise SystemExit(f"error: truncated tag {tag_id}")
        value = data[offset:end]
        label = names.get(tag_id, f"tag-{tag_id}")
        if tag_id == TAG_IMAGE:
            rendered = f"{length} bytes"
        elif tag_id in (TAG_IMAGE_TYPE, TAG_REBOOT):
            if length != 4:
                raise SystemExit(f"error: integer tag {tag_id} has length {length}")
            rendered = str(struct.unpack(">I", value)[0])
        else:
            rendered = value.decode("ascii", errors="replace")
        print(f"{label:12}: {rendered}")
        offset = end

    if offset != len(data):
        raise SystemExit("error: trailing data")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    create_parser = subparsers.add_parser("create")
    create_parser.add_argument("image")
    create_parser.add_argument("output")
    create_parser.add_argument("--project", default="DG2200TN")
    create_parser.add_argument("--hardware", default="DG2200TN")
    create_parser.add_argument("--config-type", default="operational")
    create_parser.add_argument("--image-type", type=int, default=0)
    create_parser.add_argument("--version", default="99.0.0.0")
    create_parser.add_argument("--name", default="OpenWrt")
    create_parser.add_argument("--component-version", default="25.12.5")
    create_parser.set_defaults(func=create)

    inspect_parser = subparsers.add_parser("inspect")
    inspect_parser.add_argument("input")
    inspect_parser.set_defaults(func=inspect)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()

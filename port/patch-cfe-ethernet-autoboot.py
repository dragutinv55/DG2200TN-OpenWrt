#!/usr/bin/env python3

import argparse
import struct
from pathlib import Path


IMAGE_HEADER_SIZE = 12
CFE_BASE = 0x00F00000
HOOK_ADDRESS = 0x00F00584
HOOK_SIZE = 0x400
AUTORUN_CALL_ADDRESS = 0x00F2291C
EXPECTED_AUTORUN_CALL = 0xEBFFF33A


def image_offset(address: int) -> int:
    return IMAGE_HEADER_SIZE + address - CFE_BASE


def branch_link(source: int, target: int) -> int:
    displacement = target - (source + 8)
    if displacement % 4:
        raise ValueError("unaligned ARM branch")
    word_offset = displacement // 4
    if not -(1 << 23) <= word_offset < (1 << 23):
        raise ValueError("ARM branch target is out of range")
    return 0xEB000000 | (word_offset & 0xFFFFFF)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Patch bank-2 CFE to initialize Ethernet before autorun"
    )
    parser.add_argument("input", type=Path)
    parser.add_argument("hook", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    image = bytearray(args.input.read_bytes())
    hook = args.hook.read_bytes()
    if len(hook) > HOOK_SIZE:
        raise SystemExit(
            f"error: hook is {len(hook)} bytes, exceeds {HOOK_SIZE}-byte gap"
        )

    hook_offset = image_offset(HOOK_ADDRESS)
    hook_gap = image[hook_offset:hook_offset + HOOK_SIZE]
    if hook_gap != b"\x00" * HOOK_SIZE:
        raise SystemExit("error: CFE hook gap is not empty")

    call_offset = image_offset(AUTORUN_CALL_ADDRESS)
    original_call = struct.unpack_from("<I", image, call_offset)[0]
    if original_call != EXPECTED_AUTORUN_CALL:
        raise SystemExit(
            f"error: unexpected autorun instruction 0x{original_call:08x}"
        )

    image[hook_offset:hook_offset + len(hook)] = hook
    patched_call = branch_link(AUTORUN_CALL_ADDRESS, HOOK_ADDRESS)
    struct.pack_into("<I", image, call_offset, patched_call)
    args.output.write_bytes(image)

    print(
        f"patched autorun BL at 0x{AUTORUN_CALL_ADDRESS:08x} "
        f"to hook 0x{HOOK_ADDRESS:08x}"
    )


if __name__ == "__main__":
    main()

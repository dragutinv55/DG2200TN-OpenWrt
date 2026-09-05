#!/usr/bin/env python3
"""
Wrap a raw ARM binary (zImage, optionally with an appended DTB) in a minimal
ET_EXEC ELF32 so that CFE's "r" command can load and execute it.

CFE's run command is built around an ELF loader -- the stock "Default host run
file name" is "vmlinux", which is an ELF. Handing it a raw or BRCM/LZMA blob
makes it fall back to a plain load without ever jumping to the entry point.

The ELF produced here has a single PT_LOAD segment mapped at the load address
with the entry point at its start, which is all CFE needs.
"""

import argparse
import struct
import sys

EM_ARM = 40
ET_EXEC = 2
PT_LOAD = 1
PF_RWX = 7
EHDR_SIZE = 52
PHDR_SIZE = 32
PAYLOAD_OFFSET = 0x1000  # keeps p_offset congruent to p_vaddr modulo p_align
DEFAULT_LOAD = 0x00008000


def build_elf(payload: bytes, load_addr: int) -> bytes:
    ehdr = struct.pack(
        "<16sHHIIIIIHHHHHH",
        b"\x7fELF\x01\x01\x01" + b"\x00" * 9,
        ET_EXEC,
        EM_ARM,
        1,               # e_version
        load_addr,       # e_entry
        EHDR_SIZE,       # e_phoff
        0,               # e_shoff
        0x05000000,      # e_flags: EF_ARM_EABI_VER5
        EHDR_SIZE,
        PHDR_SIZE,
        1,               # e_phnum
        40,              # e_shentsize
        0,               # e_shnum
        0,               # e_shstrndx
    )

    phdr = struct.pack(
        "<IIIIIIII",
        PT_LOAD,
        PAYLOAD_OFFSET,
        load_addr,       # p_vaddr
        load_addr,       # p_paddr
        len(payload),    # p_filesz
        len(payload),    # p_memsz
        PF_RWX,
        0x1000,          # p_align
    )

    pad = b"\x00" * (PAYLOAD_OFFSET - EHDR_SIZE - PHDR_SIZE)
    return ehdr + phdr + pad + payload


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", help="raw ARM image, e.g. zImage")
    parser.add_argument("output", help="path to write the ELF to")
    parser.add_argument("--dtb", help="device tree blob to append to the image")
    parser.add_argument(
        "--load",
        type=lambda v: int(v, 0),
        default=DEFAULT_LOAD,
        help="physical load and entry address (default: 0x%08x)" % DEFAULT_LOAD,
    )
    args = parser.parse_args()

    with open(args.binary, "rb") as fh:
        payload = fh.read()

    if args.dtb:
        with open(args.dtb, "rb") as fh:
            dtb = fh.read()
        if dtb[:4] != b"\xd0\x0d\xfe\xed":
            sys.exit(f"error: {args.dtb} is not a device tree blob")
        payload += dtb

    with open(args.output, "wb") as fh:
        fh.write(build_elf(payload, args.load))

    print(f"payload     : {len(payload)} bytes")
    print(f"load / entry: 0x{args.load:08x}")
    print(f"wrote       : {args.output}")


if __name__ == "__main__":
    main()

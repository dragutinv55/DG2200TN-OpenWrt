#!/usr/bin/env python3
"""Generate local Runner C sources from the owner's stock rdpa.ko."""

import argparse
import hashlib
import os
from pathlib import Path
import struct


MODULE_SIZE = 1_212_744
MODULE_SHA256 = "69ae453a8b3c5c4bb0736a4e18f627ef850053c253f759c13e1e4dd0d91d3a7f"
FIRMWARE_BASE = 0x99E20

SEGMENTS = (
    (
        "runner_fw_a.c",
        "uint32_t",
        "firmware_binary_A",
        0x6CDC,
        28_672,
        "a27cde265e900e1889c080947a9570e78152c811f833ca820a98cf8255a1e971",
    ),
    (
        "runner_fw_b.c",
        "uint32_t",
        "firmware_binary_B",
        0xDCE0,
        28_672,
        "cd0e72170e70a00e08025076a36a45ceda6b86283b63bd7b05fd03c2c11c59d2",
    ),
    (
        "runner_fw_c.c",
        "uint32_t",
        "firmware_binary_C",
        0x14CE4,
        16_384,
        "1aa9ab56b9cdd9521fdbae9ecf216a537045dcd3244141b0b4c92f2ad37b6fd7",
    ),
    (
        "runner_fw_d.c",
        "uint32_t",
        "firmware_binary_D",
        0x18CE8,
        16_384,
        "f8b08e7b7943476a0a17fbefef0114c124da781280f56ffbf1dbefe642da717c",
    ),
    (
        "predict_runner_fw_a.c",
        "uint16_t",
        "firmware_predict_A",
        0x1CCE8,
        896,
        "2b408fd977627d07dc6065d709ec576740078fc8978f73a031c68fe05685347d",
    ),
    (
        "predict_runner_fw_b.c",
        "uint16_t",
        "firmware_predict_B",
        0x1D068,
        896,
        "48345faad3b1e6a5a6c4bcd6f8a33bd486ccc39162550292e446dcd6d5fd47d2",
    ),
    (
        "predict_runner_fw_c.c",
        "uint16_t",
        "firmware_predict_C",
        0x1D3E8,
        512,
        "825bf79c91b00273d4396d5aa2fce998c7a1b64be8bae74ef8f36285cab5b883",
    ),
    (
        "predict_runner_fw_d.c",
        "uint16_t",
        "firmware_predict_D",
        0x1D5E8,
        512,
        "59d909a689e88ea32b6d86ea32ed8d55399a34f6d94721f616c49f7e3a2cd2f0",
    ),
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def render_source(c_type: str, symbol: str, payload: bytes) -> str:
    if c_type == "uint32_t":
        unpack_format = "<I"
        value_format = "    0x{value:08X},"
    else:
        unpack_format = "<H"
        value_format = "    0x{value:04X},"

    values = (
        value_format.format(value=value[0])
        for value in struct.iter_unpack(unpack_format, payload)
    )
    return (
        "/* Generated locally from the owner's DG2200TN stock rdpa.ko. */\n"
        "/* This file is intentionally excluded from version control. */\n\n"
        f"{c_type} {symbol}[] = {{\n"
        + "\n".join(values)
        + "\n};\n"
    )


def main() -> None:
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description=(
            "Extract the DG2200TN Runner microcode needed for a local build. "
            "The proprietary bytes remain under the ignored port/firmware directory."
        )
    )
    parser.add_argument("rdpa_module", type=Path, help="stock firmware rdpa.ko")
    parser.add_argument(
        "--output",
        type=Path,
        default=script_dir / "firmware" / "bcm63138-stock-source",
        help="generated source directory",
    )
    args = parser.parse_args()

    module = args.rdpa_module.read_bytes()
    module_digest = sha256(module)
    if len(module) != MODULE_SIZE or module_digest != MODULE_SHA256:
        raise SystemExit(
            "unsupported rdpa.ko: "
            f"size={len(module)} sha256={module_digest}; "
            "expected the DG2200TN stock module documented by this port"
        )

    args.output.mkdir(parents=True, exist_ok=True)
    for filename, c_type, symbol, offset, size, expected_digest in SEGMENTS:
        start = FIRMWARE_BASE + offset
        payload = module[start : start + size]
        digest = sha256(payload)
        if len(payload) != size or digest != expected_digest:
            raise SystemExit(
                f"{filename}: stock segment verification failed "
                f"(size={len(payload)} sha256={digest})"
            )

        output = args.output / filename
        output.write_text(render_source(c_type, symbol, payload))
        os.chmod(output, 0o600)
        print(f"{filename}: {size} bytes sha256={digest}")


if __name__ == "__main__":
    main()

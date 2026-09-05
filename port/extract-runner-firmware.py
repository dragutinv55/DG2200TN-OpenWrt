#!/usr/bin/env python3
"""Extract BCM63138 Runner firmware blobs from the released GPL source tree."""

import argparse
import hashlib
import pathlib
import re
import struct
import subprocess


SOURCE_BASE = (
    "extern/broadcom-bsp-4.16L05/shared/broadcom/rdp/impl2/"
    "firmware_dsl_63138"
)


def git_show(repository: pathlib.Path, path: str) -> str:
    return subprocess.check_output(
        ["git", "-C", str(repository), "show", f"HEAD:{path}"],
        text=True,
    )


def parse_array(source: str, c_type: str, name: str) -> list[int]:
    match = re.search(
        rf"{c_type}\s+{name}\s*\[\]\s*=\s*\{{(.*?)\}};",
        source,
        re.DOTALL,
    )
    if not match:
        raise ValueError(f"array {name} not found")
    return [int(value, 16) for value in re.findall(r"0x[0-9a-fA-F]+", match.group(1))]


def write_blob(path: pathlib.Path, values: list[int], width: int) -> None:
    format_string = ">I" if width == 32 else ">H"
    payload = b"".join(struct.pack(format_string, value) for value in values)
    path.write_bytes(payload)
    digest = hashlib.sha256(payload).hexdigest()
    print(f"{path.name}: {len(payload)} bytes sha256={digest}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repository",
        type=pathlib.Path,
        default=pathlib.Path("vendor-bcm63138"),
    )
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=pathlib.Path("firmware/bcm63138"),
    )
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)

    for runner in ("b", "c", "d"):
        suffix = runner.upper()
        source = git_show(
            args.repository, f"{SOURCE_BASE}/runner_fw_{runner}.c"
        )
        values = parse_array(source, "uint32_t", f"firmware_binary_{suffix}")
        write_blob(args.output / f"runner-{runner}.bin", values, 32)

        source = git_show(
            args.repository, f"{SOURCE_BASE}/predict_runner_fw_{runner}.c"
        )
        values = parse_array(source, "uint16_t", f"firmware_predict_{suffix}")
        write_blob(args.output / f"runner-{runner}-predict.bin", values, 16)


if __name__ == "__main__":
    main()

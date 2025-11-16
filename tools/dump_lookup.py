#!/usr/bin/env python3
"""Dump a human-readable view of a lookup_<start>.bin file."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

ENTRY_STRUCT = struct.Struct("<H H Q I")
HEADER_STRUCT = struct.Struct("<4sIIQ5s3sI")


def decode_word(value: int) -> str:
    letters = []
    for _ in range(5):
        code = value & 0x1F
        letters.append(chr(code + 96) if code else "-")
        value >>= 5
    return "".join(reversed(letters))


def walk(buffer: bytes, offset: int, depth: int, prefix: list[int]) -> None:
    count = struct.unpack_from("<I", buffer, offset)[0]
    print("  " * depth + f"node@{offset}: entries={count}")
    cursor = offset + 4
    for _ in range(count):
        feedback, _, guess, child = ENTRY_STRUCT.unpack_from(buffer, cursor)
        cursor += ENTRY_STRUCT.size
        print(
            "  " * (depth + 1)
            + f"fb={feedback:03} guess={decode_word(guess)} child={child}"
        )
        if child:
            walk(buffer, child, depth + 2, prefix + [feedback])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", help="lookup binary file", default="lookup_roate.bin")
    args = parser.parse_args()

    data = Path(args.path).read_bytes()
    magic, version, depth, start_enc, start_str, _, root_off = HEADER_STRUCT.unpack(
        data[: HEADER_STRUCT.size]
    )
    print(
        f"magic={magic.decode()} version={version} depth={depth} start={start_str.decode()} root={root_off}"
    )
    walk(data, root_off, 0, [])


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Generate a multi-level precomputed lookup table for a given start word."""

from __future__ import annotations

import argparse
import json
import struct
import subprocess
from pathlib import Path
from typing import Dict, List, Tuple


def encode_word(word: str) -> int:
    value = 0
    for c in word:
        value = (value << 5) | (ord(c) - ord("a") + 1)
    return value


class Node:
    def __init__(self, guess: int | None = None) -> None:
        self.guess = guess
        self.children: Dict[int, Node] = {}


def collect_trace(solver: Path, target: str) -> List[Dict[str, int | str]]:
    result = subprocess.run(
        [solver, "--disable-lookup", "--dump-json", "--word", target],
        check=True,
        capture_output=True,
        text=True,
    )
    # last non-empty line is the JSON array
    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    data = json.loads(lines[-1])
    return data


def build_tree(
    traces: Dict[str, List[Dict[str, int | str]]], depth: int, start_word: str
) -> Node:
    root = Node()
    for steps in traces.values():
        if not steps or steps[0]["guess"] != start_word:
            continue
        limit = min(depth, len(steps))
        current = root
        for level in range(1, limit):
            feedback = int(steps[level - 1]["feedback"])
            guess_word = steps[level]["guess"]
            guess_encoded = encode_word(guess_word)
            child = current.children.get(feedback)
            if child is None:
                child = Node(guess_encoded)
                current.children[feedback] = child
            current = child
    return root


def serialize_tree(root: Node, depth: int, start_word: str, output: Path) -> None:
    buffer = bytearray()

    def write_node(node: Node) -> int:
        offset = len(buffer)
        buffer.extend(struct.pack("<I", len(node.children)))
        placeholders: List[Tuple[int, Node]] = []
        for feedback, child in sorted(node.children.items()):
            buffer.extend(struct.pack("<H", feedback))
            buffer.extend(struct.pack("<H", 0))
            buffer.extend(struct.pack("<Q", child.guess or 0))
            placeholder_pos = len(buffer)
            buffer.extend(struct.pack("<I", 0))
            placeholders.append((placeholder_pos, child))
        for pos, child in placeholders:
            child_offset = write_node(child)
            buffer[pos : pos + 4] = struct.pack("<I", child_offset + HEADER_SIZE)
        return offset

    HEADER_SIZE = 32
    write_node(root)
    start_encoded = encode_word(start_word)
    header = struct.pack(
        "<4sIIQ5s3sI",
        b"PLUT",
        1,
        depth,
        start_encoded,
        start_word.encode("ascii"),
        b"\x00\x00\x00",
        HEADER_SIZE,
    )
    with output.open("wb") as fh:
        fh.write(header)
        fh.write(buffer)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--start", default="roate", help="starting word (default: roate)")
    parser.add_argument("--depth", type=int, default=3, help="lookup depth (default: 3)")
    parser.add_argument("--solver", default="build/solver_cpp", help="path to solver binary")
    parser.add_argument("--output", default="lookup_roate.bin", help="output binary filename")
    args = parser.parse_args()

    solver = Path(args.solver)
    answers = Path("official_answers.txt").read_text().splitlines()
    traces = {word: collect_trace(solver, word) for word in answers}
    tree = build_tree(traces, args.depth, args.start)
    serialize_tree(tree, args.depth, args.start, Path(args.output))
    print(f"Wrote lookup table to {args.output}")


if __name__ == "__main__":
    main()

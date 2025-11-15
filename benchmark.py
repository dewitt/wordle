#!/usr/bin/env python3
"""Benchmark the Wordle solver by timing ten fixed target words."""

from __future__ import annotations

import statistics
import subprocess
import sys
import time
from pathlib import Path


WORDS = [
    "cigar",
    "rebut",
    "sissy",
    "humph",
    "awake",
    "blush",
    "focal",
    "evade",
    "naval",
    "serve",
]


def main() -> None:
    repo_root = Path(__file__).resolve().parent
    solver_path = repo_root / "build" / "solver_cpp"

    if not solver_path.exists():
        print(f"solver binary not found at {solver_path}. Build the project first.", file=sys.stderr)
        sys.exit(1)

    timings: list[float] = []

    for word in WORDS:
        start = time.perf_counter()
        result = subprocess.run(
            [str(solver_path), "--word", word],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        elapsed = time.perf_counter() - start

        if result.returncode != 0:
            print(f"Failed solving '{word}': {result.stderr.strip()}", file=sys.stderr)
            sys.exit(result.returncode)

        timings.append(elapsed)
        print(f"{word:>5}: {elapsed:.4f} s")

    avg = statistics.fmean(timings)
    print(f"\nAverage solve time over {len(WORDS)} words: {avg:.4f} s")


if __name__ == "__main__":
    main()

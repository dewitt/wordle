#!/usr/bin/env python3
"""Benchmark the Wordle solver by timing a fixed set of target words."""

from __future__ import annotations

import argparse
import statistics
import subprocess
import sys
import time
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--limit",
        type=int,
        default=10,
        help="Number of target words to benchmark (default: 10).",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent
    solver_path = repo_root / "build" / "solver_cpp"
    answers_path = repo_root / "official_answers.txt"

    if not solver_path.exists():
        print(f"solver binary not found at {solver_path}. Build the project first.", file=sys.stderr)
        sys.exit(1)
    if not answers_path.exists():
        print(f"answers list not found at {answers_path}", file=sys.stderr)
        sys.exit(1)

    with answers_path.open() as fh:
        words = [line.strip() for line in fh if len(line.strip()) == 5]
    if len(words) < args.limit:
        print(f"answers list only has {len(words)} entries, cannot take {args.limit}", file=sys.stderr)
        sys.exit(1)

    words = words[: args.limit]

    timings: list[float] = []

    for word in words:
        start = time.perf_counter()
        result = subprocess.run(
            [str(solver_path), "--word", word],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        elapsed = time.perf_counter() - start

        if result.returncode != 0 or "Solved" not in result.stdout:
            stderr = result.stderr.strip()
            if not stderr:
                stderr = result.stdout.strip()
            print(f"Failed solving '{word}': {stderr}", file=sys.stderr)
            sys.exit(result.returncode or 1)

        timings.append(elapsed)
        print(f"{word:>5}: {elapsed:.4f} s")

    avg = statistics.fmean(timings) if timings else 0.0
    print(f"\nAverage solve time over {len(words)} words: {avg:.4f} s")


if __name__ == "__main__":
    main()

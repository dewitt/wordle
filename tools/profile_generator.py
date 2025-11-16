#!/usr/bin/env python3
import argparse
import subprocess
import tempfile
from pathlib import Path
import time

PINNED_WORDS = ["sissy", "baker", "aback", "cigar", "roate"]

def read_words(words_path: Path):
    with words_path.open() as fh:
        words = [line.strip() for line in fh if len(line.strip()) == 5]
    lower = []
    for w in words:
        lower.append(w.lower())
    return lower

def build_subset(words, size, pinned):
    subset = []
    seen = set()
    for p in pinned:
        if p in words and p not in seen:
            subset.append(p)
            seen.add(p)
    for w in words:
        if w in seen:
            continue
        subset.append(w)
        seen.add(w)
        if len(subset) >= size:
            break
    return subset

def run_generate(binary, subset_file, start_word, depth, timeout):
    output_file = subset_file.with_suffix('.bin')
    cmd = [binary, 'generate', '--word-list', str(subset_file),
           '--lookup-start', start_word, '--lookup-depth', str(depth),
           '--lookup-output', str(output_file)]
    start = time.perf_counter()
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    duration = time.perf_counter() - start
    summary = None
    for line in (proc.stdout + '\n' + proc.stderr).splitlines():
        if line.startswith('Wrote lookup table'):
            summary = line.strip()
    return duration, summary

def main():
    parser = argparse.ArgumentParser(description='Profile lookup generator over subsets')
    parser.add_argument('--solver', default='./build/solver', help='Path to solver binary')
    parser.add_argument('--sizes', nargs='+', type=int, default=[50, 100, 200],
                        help='Subset sizes to test')
    parser.add_argument('--start', default='roate', help='Start word for generation')
    parser.add_argument('--depth', type=int, default=6)
    parser.add_argument('--timeout', type=int, default=300, help='Timeout per run in seconds')
    parser.add_argument('--words-file', default='words.txt', help='Base word list')
    args = parser.parse_args()

    words = read_words(Path(args.words_file))
    for size in args.sizes:
        subset = build_subset(words, size, PINNED_WORDS)
        if len(subset) < size:
            print(f"Requested size {size} but only {len(subset)} words available")
        with tempfile.NamedTemporaryFile('w', delete=False) as tmp:
            for w in subset:
                tmp.write(w + '\n')
            tmp_path = Path(tmp.name)
        try:
            duration, summary = run_generate(args.solver, tmp_path, args.start, args.depth, args.timeout)
            print(f"subset={size} time={duration:.2f}s summary={summary}")
        finally:
            tmp_path.unlink(missing_ok=True)
    
if __name__ == '__main__':
    main()

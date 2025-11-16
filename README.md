# C++ Wordle Solver

This is a highly optimized, multithreaded command-line tool to solve Wordle puzzles. It uses an entropy-reduction algorithm to find the best possible guess at each step of the game.

This project serves as a case study in C++ performance optimization, demonstrating several advanced techniques to achieve maximum speed for a computationally intensive task.

## Features

-   **High-Performance Solver:** The core logic is written in modern C++17 for maximum performance.
-   **Entropy-Reduction Algorithm:** Suggests the optimal guess to narrow down the list of possible words as quickly as possible.
-   **Six-Turn Lookup:** Every possible feedback path for the first six guesses (starting at `roate`) is precomputed ahead of time. Solving is therefore just “walk the lookup tree, compute feedback, repeat”, which keeps each turn effectively constant time.
-   **Optimized Data Structures:** 5-letter words are encoded into 64-bit integers, allowing for extremely fast, cache-friendly comparisons using bitwise operations.
-   **Weighted Tie-Breaks:** When multiple candidates score the same entropy, the solver prefers words whose letters are most common across the dictionary, leading to more human-like decisions on tricky branches.
-   **Official Word Lists:** Uses the official, community-verified Wordle answer and guess lists for accuracy.

## Data Files

- `words.txt` is the canonical list of every valid five-letter guess. The solver encodes this file (via the embedded `word_lists.h`) and never consults historical answer lists at runtime. Edit this file to change the allowed vocabulary.
- `official_answers.txt` is retained solely for benchmarking. The solver never reads it; `benchmark.py` draws targets from this file so we can track performance against the historical puzzle set.
- `official_guesses.txt` mirrors the original Wordle guess list. Keep it around if you want to rebuild `words.txt` from the old separation of guesses vs. answers.

For additional performance the solver uses a few precomputed assets:

- `word_lists.h` is generated once from `words.txt` and embedded directly into the binary. You generally do not need to touch this file, but keep `words.txt` up to date so the embedded data stays accurate.
- `feedback_table.bin` is an optional binary cache containing the results of `calculate_feedback_encoded` for every pair of valid words (≈167 MB). Refresh it by passing `--feedback-table` to any mode (for example `./build/solver generate --feedback-table`). When present, the solver memory-maps this cache at startup and skips recomputing feedback in the hot loops. If the file is absent, the solver falls back to the slower but correct on-the-fly calculations.
- `lookup_roate.bin` is the precomputed six-turn decision tree (≈27 MB) rooted at `roate`. Build it via the solver itself: `./build/solver generate --lookup-start roate --lookup-depth 6 --lookup-output lookup_roate.bin`. The solver requires this file at runtime; if a feedback sequence is missing from the tree the run aborts and reports the missing path.

## Modes of Operation

The `solver` binary exposes four explicit modes so you always know which workflow is active:

- `solve <word>`: non-interactively solve a single target. Pass `--debug` for verbose, turn-by-turn output plus lookup diagnostics, and `--dump-json` to emit a structured trace instead of human-readable text.
- `start`: exhaustively analyze all guesses to report the best opening word.
- `generate`: build `lookup_<word>.bin` files (and optionally rebuild `feedback_table.bin`) entirely inside the C++ binary. Flags such as `--lookup-depth` (default 6), `--lookup-output`, `--lookup-start`, `--feedback-table`, and `--word-list FILE` (override dictionary for experiments) customize the generated assets. You must run this mode at least once (to produce `lookup_roate.bin`) before using `solve`.
- `help`: display a concise usage summary. `--help` is equivalent and may appear anywhere.

## Code Layout

The project is now organized into focused translation units so future changes are easier to reason about:

- `solver_main.cpp` – parses CLI flags, dispatches to modes, and glues the other modules together.
- `solver_runtime.{h,cpp}` – loads `lookup_<start>.bin` and streams guesses from the sparse tree. Contains `run_non_interactive` plus the binary header definition.
- `lookup_generator.{h,cpp}` – rebuilds lookup trees inside the solver binary (still using the legacy heuristic while we iterate on NEW_DESIGN.md).
- `solver_core.{h,cpp}` – shared algorithms such as feedback computation and the multithreaded entropy search helper that generation still leans on.
- `feedback_cache.{h,cpp}` – memory-maps or rebuilds `feedback_table.bin`.
- `words_data.{h,cpp}` – owns the encoded word list, encoding helpers, and letter-frequency weights.
- `solver_types.h` – centralizes common typedefs so every module speaks the same API.

If you need to tweak the solver, start by locating the relevant module in this list rather than editing `solver_main.cpp` directly.

## Building the Solver

This project uses CMake for building. You will need a C++ compiler (like `g++` or `clang`) and `cmake` installed.

### 1. Create a Build Directory

```bash
mkdir build
cd build
```

### 2. Configure the Project with CMake

```bash
cmake ..
```

### 3. Compile the Code

```bash
make
```

This will create an executable named `solver` inside the `build` directory.

## How to Use

All commands should be run from the project's root directory.

Examples:

```bash
# Solve today's puzzle with the default heuristics
./build/solver solve cigar

# See the entire solving trace plus lookup vs. entropy fallbacks
./build/solver solve clung --debug

# Find the globally optimal start word
./build/solver start

# Build the depth-6 lookup tree for ROATE (required before solving)
./build/solver generate --lookup-start roate --lookup-depth 6 --lookup-output lookup_roate.bin --feedback-table

# Experiment with a tiny word list (useful for generator debugging)
./build/solver generate --word-list test_words.txt --lookup-start roate --lookup-output lookup_test.bin

# Profile generator performance across subset sizes
python3 tools/profile_generator.py --sizes 50 100 250 --timeout 120
```

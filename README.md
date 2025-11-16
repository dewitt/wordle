# C++ Wordle Solver

This is a highly optimized, multithreaded command-line tool to solve Wordle puzzles. It uses an entropy-reduction algorithm to find the best possible guess at each step of the game.

This project serves as a case study in C++ performance optimization, demonstrating several advanced techniques to achieve maximum speed for a computationally intensive task.

## Features

-   **High-Performance Solver:** The core logic is written in modern C++17 for maximum performance.
-   **Entropy-Reduction Algorithm:** Suggests the optimal guess to narrow down the list of possible words as quickly as possible.
-   **Multithreaded:** The search for the best guess is parallelized across all available CPU cores, dramatically reducing calculation time.
-   **Optimized Data Structures:** 5-letter words are encoded into 64-bit integers, allowing for extremely fast, cache-friendly comparisons using bitwise operations.
-   **Official Word Lists:** Uses the official, community-verified Wordle answer and guess lists for accuracy.

## Data Files

- `words.txt` is the canonical list of every valid five-letter guess. The solver encodes this file (via the embedded `word_lists.h`) and never consults historical answer lists at runtime. Edit this file to change the allowed vocabulary.
- `official_answers.txt` is retained solely for benchmarking. The solver never reads it; `benchmark.py` draws targets from this file so we can track performance against the historical puzzle set.
- `official_guesses.txt` mirrors the original Wordle guess list. Keep it around if you want to rebuild `words.txt` from the old separation of guesses vs. answers.

For additional performance the solver uses a few precomputed assets:

- `word_lists.h` is generated once from `words.txt` and embedded directly into the binary. You generally do not need to touch this file, but keep `words.txt` up to date so the embedded data stays accurate.
- `feedback_table.bin` is an optional binary cache containing the results of `calculate_feedback_encoded` for every pair of valid words (≈167 MB). Refresh it by passing `--feedback-table` to any mode (for example `./build/solver generate --feedback-table`). When present, the solver memory-maps this cache at startup and skips recomputing feedback in the hot loops. If the file is absent, the solver falls back to the slower but correct on-the-fly calculations.
- `lookup_roate.bin` is a sparse lookup table capturing the optimal next guesses for the default start word (`roate`). Generate it via the solver itself: `./build/solver generate --lookup-start roate --lookup-depth 6 --lookup-output lookup_roate.bin`. The solver automatically loads this file (when present) and follows the precomputed tree before falling back to the entropy search. Delete the file if you want to force the solver to recompute guesses dynamically.

## Modes of Operation

The `solver` binary exposes four explicit modes so you always know which workflow is active:

- `solve <word>`: non-interactively solve a single target. Pass `--debug` for verbose, turn-by-turn output plus lookup/entropy diagnostics, `--hard-mode` to enforce Wordle hard mode, and `--disable-lookup` to fall back to pure entropy search.
- `start`: exhaustively analyze all guesses to report the best opening word.
- `generate`: build `lookup_<word>.bin` files (and optionally rebuild `feedback_table.bin`) entirely inside the C++ binary. Flags such as `--lookup-depth`, `--lookup-output`, `--lookup-start`, and `--feedback-table` customize the generated assets.
- `help`: display a concise usage summary. `--help` is equivalent and may appear anywhere.

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

# Build a depth-4 lookup tree for ROATE and refresh feedback_table.bin
./build/solver generate --lookup-start roate --lookup-depth 6 --lookup-output lookup_roate.bin --feedback-table
```

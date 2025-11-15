# C++ Wordle Solver

This is a highly optimized, multithreaded command-line tool to solve Wordle puzzles. It uses an entropy-reduction algorithm to find the best possible guess at each step of the game.

This project serves as a case study in C++ performance optimization, demonstrating several advanced techniques to achieve maximum speed for a computationally intensive task.

## Features

-   **High-Performance Solver:** The core logic is written in modern C++17 for maximum performance.
-   **Entropy-Reduction Algorithm:** Suggests the optimal guess to narrow down the list of possible words as quickly as possible.
-   **Multithreaded:** The search for the best guess is parallelized across all available CPU cores, dramatically reducing calculation time.
-   **Optimized Data Structures:** 5-letter words are encoded into 64-bit integers, allowing for extremely fast, cache-friendly comparisons using bitwise operations.
-   **Official Word Lists:** Uses the official, community-verified Wordle answer and guess lists for accuracy.

## Modes of Operation

The solver can be run in two modes:

1.  **Solve Mode:** Solves for a specific target word and prints its process.
2.  **Find Best Start Mode:** Performs a one-off, exhaustive analysis to determine the single best starting word according to the algorithm.

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

This will create an executable named `solver_cpp` inside the `build` directory.

## How to Use

All commands should be run from the project's root directory.

### To Solve for a Specific Word

Use the `--word` flag followed by the 5-letter word you want to solve.

```bash
./build/solver_cpp --word cigar
```

### To Find the Best Starting Word

Use the `--find-best-start` flag. This will run a full analysis, which may take a minute or two depending on your machine.

```bash
./build/solver_cpp --find-best-start
```
## Updating Precomputed Data

The solver embeds the official answer/guess lists and a lookup table for the optimal second guess after opening with `roate`. If you need to refresh these tables (for example, when the official lists change), run:

```bash
python3 tools/generate_tables.py
```

This script regenerates `word_lists.h` and `opening_table.h` using `official_answers.txt`, `official_guesses.txt`, and the current `build/solver_cpp` binary.

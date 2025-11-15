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

Two plain-text files live at the repository root: `official_answers.txt` and `official_guesses.txt`. Each contains one five-letter lowercase word per line. The solver loads and encodes these lists at startup, so you can swap in custom dictionaries by editing the text files—no code changes or regeneration steps are required.

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

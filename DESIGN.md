# Design Overview

## Runtime architecture

1. **Word encoding** – `words.txt` is embedded in `word_lists.h` as `kEncodedWords`. Each five-letter word is encoded in 25 bits (5 bits/letter) so comparisons and table lookups can operate entirely on integers.
2. **Lookup tables** – At startup the solver builds an `unordered_map` (`LookupTables::word_index`) from encoded word → row/column index. This single map feeds the feedback cache, lookup tables, and CLI validation.
3. **Feedback computation** – `calculate_feedback_encoded` matches the official Wordle rules by running two passes (greens, then yellows) over the encoded letters. This is the only “dynamic” work the runtime does once the lookup table has been generated.
4. **Lookup tree generation** – `generate_lookup_table` walks every reachable feedback branch (rooted at the fixed opener `roate`) up to a user-specified depth (default: 6 turns). For each intermediate subset it invokes the entropy search once to record the optimal next guess. Leaves are either (a) singleton subsets (“the word is known, stop recursing”) or (b) depth-exhausted branches (which store a sentinel so the runtime reports failure). Runtime never falls back to entropy: it simply streams guesses from this tree and aborts if a branch is missing.
5. **Lookup acceleration** – Optional `lookup_<start>.bin` files store a sparse prefix tree of optimal guesses for the default starting word. During a solve the binary walks this tree before falling back to the entropy search.

## Command-line interface

The `solver` binary accepts a primary mode followed by flag arguments:

- `solve <word>` – one-shot solve of a target from `words.txt`. Flags: `--hard-mode`, `--debug` (verbose output + lookup diagnostics), `--disable-lookup`, `--dump-json`.
- `start` – exhaustively analyze all words to report the best opening word. Primarily used when experimenting with new heuristics or data sets.
- `generate` – create auxiliary assets. Flags: `--lookup-start`, `--lookup-depth` (default 6), `--lookup-output`, and `--feedback-table` (rebuilds `feedback_table.bin` first).
- `help` / `--help` – print usage.

All flags are mode-agnostic and may appear before or after the mode token. `--feedback-table` is honored by every mode so you can refresh caches while solving or benchmarking.

## Benchmarking workflow

`benchmark.py` times the solver across a subset of `official_answers.txt` to keep historical comparisons consistent. Although the solver never consults `official_answers.txt`, we retain the file to drive benchmarks and to double-check regression results. The canonical runtime vocabulary is `words.txt` (the union of official answers and guesses).

# Word Sources

- `words.txt` contains every valid guess (official answers ∪ guesses). The build embeds this list in `word_lists.h`, so solver logic never needs to read the historical answer set directly. Tools like `benchmark.py` may still open `official_answers.txt` to pick historical targets, but the solver only sees the unified vocabulary.

# Lookup Table Format

Binary files `lookup_<start>.bin` encode a sparse feedback tree for a fixed start word. All integers are little-endian.

## Header (32 bytes)
```
struct Header {
    char     magic[4];      // "PLUT"
    uint32_t version;       // currently 1
    uint32_t depth;         // number of guesses captured per sequence
    uint32_t root_offset;   // byte offset of the root node (always 32)
    uint64_t start_encoded; // encoded form of the start word
    char     start_word[5]; // ASCII start word (no null terminator)
    char     reserved[3];   // padding (zero)
};
static_assert(sizeof(Header) == 32);
```

## Node
Each node begins with a 32-bit entry count followed by that many entries:
```
struct Entry {
    uint16_t feedback;     // feedback code (0..242) in base-3 encoding
    uint16_t reserved;     // zero
    uint64_t guess;        // encoded_word for the next guess
    uint32_t child_offset; // absolute byte offset of the child node (0 if leaf)
};
```
Entries are sorted by `feedback`. Offsets point to other nodes within the same file. Leaves set `child_offset` to 0. The root node immediately follows the 32-byte header.

## Generator semantics

- Generation lives in the solver binary (`./build/solver generate --lookup-start roate --lookup-depth N --lookup-output lookup_roate.bin`).
- For each node/state (defined by a feedback history), the generator filters the candidate indices to what remains viable:
  * If the subset is empty, no entry is emitted for that feedback.
  * If subset size is 1, the guess stored is that remaining word.
- Otherwise, it runs `find_best_guess_encoded` to choose the optimal next guess (reusing `feedback_table.bin` when available). If the depth limit is reached while more than one candidate remains, the entry records a zero guess so the runtime can report failure for that feedback branch.
- This process repeats recursively until the requested depth is reached; the last stored guess corresponds to the depth-th turn (start word counts as turn 1). In practice we always generate with `--lookup-depth 6` so every Wordle turn is covered.
- Because every possible feedback code (0..242) is iterated, the resulting tree covers all branches for the selected depth. Runtime lookup never needs to revert to entropy search until it exhausts the precomputed depth.

## Regeneration
```
./build/solver generate --lookup-start roate --lookup-depth 6 --lookup-output lookup_roate.bin
```
The solver filters candidates and runs the search internally for each branch, ensuring the lookup mirrors the exact runtime behavior. Adjust `--lookup-start`, `--lookup-depth`, and `--lookup-output` as needed (though the shipping binary assumes depth 6 and `roate`).

# Feedback Cache Format (`feedback_table.bin`)

`feedback_table.bin` caches the result of `calculate_feedback_encoded` for every ordered pair of valid words drawn from `words.txt`. The layout stays minimal so it can be memory-mapped efficiently (~167 MB for 12,947² entries):

- No header; the size implicitly equals `kWordsCount * kWordsCount` bytes.
- Rows and columns follow the exact order of `words.txt` (and therefore `kEncodedWords`).
- Each byte stores a single feedback code in `uint8_t` form (0–242), identical to the runtime base-3 encoding (`ggggg` = 242).

When present, the solver memory-maps the file and indexes it via the pre-built lookup tables, so fetching `(guess_idx, answer_idx)` is an O(1) byte read. If the file is missing or stale, rebuild it with:

```
./build/solver generate --feedback-table
```
or any other mode plus `--feedback-table`. The generator rewrites the file atomically by iterating every `(guess, answer)` pair and writing the byte matrix in row-major order.

# Lookup Table Format

Binary files `lookup_<start>.bin` encode a sparse feedback tree for a fixed start word. All integers are little-endian.

## Word Sources

`words.txt` contains every valid guess (official answers ∪ guesses). The build embeds this list in `word_lists.h`, so solver logic never needs to read the historical answer set directly. Tools like `benchmark.py` may still open `official_answers.txt` to pick historical targets, but the solver only sees the unified vocabulary.

## Header (32 bytes)
```
struct Header {
    char     magic[4];      // "PLUT"
    uint32_t version;       // currently 1
    uint32_t depth;         // number of guesses captured per sequence
    uint64_t start_encoded; // encoded form of the start word
    char     start_word[5]; // ASCII start word (no null terminator)
    char     reserved[3];   // padding (zero)
    uint32_t root_offset;   // byte offset of the root node (always 32)
};
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
- For each node/state (defined by a feedback history), the generator filters the answer indices to what remains viable:
  * If the subset is empty, no entry is emitted for that feedback.
  * If subset size is 1, the guess stored is that remaining answer.
  * Otherwise, it runs `find_best_guess_encoded` to choose the optimal next guess.
- This process repeats recursively until the requested depth is reached; the last stored guess corresponds to the depth-th turn (start word counts as turn 1).
- Because every possible feedback code (0..242) is iterated, the resulting tree covers all branches for the selected depth. Runtime lookup never needs to revert to entropy search until it exhausts the precomputed depth.

## Regeneration
```
./build/solver generate --lookup-start roate --lookup-depth 4 --lookup-output lookup_roate.bin
```
The solver filters candidates and runs the search internally for each branch, ensuring the lookup mirrors the exact runtime behavior. Adjust `--lookup-start`, `--lookup-depth`, and `--lookup-output` as needed.

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

# Lookup Table Format

Binary files `lookup_<start>.bin` encode a sparse feedback tree for a fixed start word. All integers are little-endian.

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

- Generation lives in the solver (`--generate-lookup --start roate --depth N --output lookup_roate.bin`).
- For each node/state (defined by a feedback history), the generator filters the answer indices to what remains viable:
  * If the subset is empty, no entry is emitted for that feedback.
  * If subset size is 1, the guess stored is that remaining answer.
  * Otherwise, it runs `find_best_guess_encoded` to choose the optimal next guess.
- This process repeats recursively until the requested depth is reached; the last stored guess corresponds to the depth-th turn (start word counts as turn 1).
- Because every possible feedback code (0..242) is iterated, the resulting tree covers all branches for the selected depth. Runtime lookup never needs to revert to entropy search until it exhausts the precomputed depth.

## Regeneration
```
./build/solver_cpp --generate-lookup --start roate --depth 4 --output lookup_roate.bin
```
The solver filters candidates and runs the search internally for each branch, ensuring the lookup mirrors the exact runtime behavior. Adjust `--start`, `--depth`, and `--output` as needed.

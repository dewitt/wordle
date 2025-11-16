# Design Overview

## Source layout

- `solver_main.cpp` routes CLI modes (`solve`, `start`, `generate`, `help`) and holds zero business logic beyond flag parsing.
- `words_data.{h,cpp}` exposes the embedded dictionary (`kEncodedWords`), the encode/decode helpers, and the letter-frequency weighting table used for tie-breakers.
- `feedback_cache.{h,cpp}` implements the optional `feedback_table.bin` cache plus the `FeedbackTable` helper used by entropy sampling during generation.
- `solver_core.{h,cpp}` contains the reusable algorithms: feedback math, entropy-based `find_best_guess_encoded`, and the `LookupTables` (word → index) helper.
- `solver_runtime.{h,cpp}` loads `lookup_<start>.bin`, defines the file header/entry structures, and exposes `run_non_interactive`.
- `lookup_generator.{h,cpp}` builds the sparse lookup trees using the shared core routines and serializes them via the runtime’s header/entry definitions.
- `solver_types.h` centralizes shared typedefs (`encoded_word`, `feedback_int`) so every module agrees on data representations.

This split keeps the CLI lightweight and ensures generator/runtime changes can be reasoned about independently.

## Runtime architecture

1. **Word encoding** – `words.txt` is embedded in `word_lists.h` as
   `kEncodedWords`. Each five-letter word is encoded in 25 bits (5
   bits/letter) so comparisons and table lookups can operate entirely on
   integers.
1. **Lookup tables** – At startup the solver builds an `unordered_map`
   (`LookupTables::word_index`) from encoded word → row/column index. This
   single map feeds the feedback cache, lookup tables, and CLI validation.
1. **Feedback computation** – `calculate_feedback_encoded` matches the
   official Wordle rules by running two passes (greens, then yellows) over the
   encoded letters. This is the only “dynamic” work the runtime does once the
   lookup table has been generated.
1. **Lookup tree generation** – `generate_lookup_table` explores every
   reachable branch (rooted at the fixed opener `roate` by default) up to a
   configured depth (6 turns for Wordle). Instead of delegating to the entropy
   solver on demand, the generator precomputes the entire decision tree so
   solving becomes a pure table walk.
1. **Lookup acceleration** – The emitted `lookup_<start>.bin` file embodies
   the solver’s strategy: at runtime we only traverse this sparse tree.
   Entropy search is no longer used during normal solves; any missing branch
   simply causes the run to fail (which should not happen once the tree is
   complete).

## Command-line interface

The `solver` binary accepts a primary mode followed by flag arguments:

- `solve <word>` – one-shot solve of a target from `words.txt`. Flags:
  `--debug` (verbose output + lookup diagnostics), `--dump-json`.
- `start` – exhaustively analyze all words to report the best opening word.
  Primarily used when experimenting with new heuristics or data sets.
- `generate` – create auxiliary assets. Flags: `--lookup-start`,
  `--lookup-depth` (default 6), `--lookup-output`, `--feedback-table`
  (rebuilds `feedback_table.bin` first), and `--word-list FILE` (temporary
  override of the vocabulary for experiments).
- `help` / `--help` – print usage.

All flags are mode-agnostic and may appear before or after the mode token.
`--feedback-table` is honored by every mode so you can refresh caches while
solving or benchmarking.

## Benchmarking workflow

`benchmark.py` times the solver across a subset of `official_answers.txt` to
keep historical comparisons consistent. Although the solver never consults
`official_answers.txt`, we retain the file to drive benchmarks and to
double-check regression results. The canonical runtime vocabulary is
`words.txt` (the union of official answers and guesses).

# Word Sources

- `words.txt` contains every valid guess (official answers ∪ guesses). The
  build embeds this list in `word_lists.h`, so solver logic never needs to
  read the historical answer set directly. Tools like `benchmark.py` may still
  open `official_answers.txt` to pick historical targets, but the solver only
  sees the unified vocabulary.

# Lookup Table Format

Binary files `lookup_<start>.bin` encode a sparse feedback tree for a fixed
start word. All integers are little-endian.

### Tree structure at a glance

```
                  ┌──────────┐
                  │  root    │ guess = roate
                  └────┬─────┘
                       │ fb = 110
                       ▼
                  ┌──────────┐
                  │ node A   │ guess = acres
         ┌────────┴─────┬────┴────────┐
      fb 0           fb 96          fb 242
         │              │               │
         ▼              ▼               ▼
   ┌──────────┐   ┌──────────┐    ┌──────────┐
   │ leaf ... │   │ node ... │    │ leaf ... │
   └──────────┘   └──────────┘    └──────────┘
```

Only reachable feedback IDs become children, so deep levels stay sparse even
though 243 theoretical outcomes exist per node.

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

Entries are sorted by `feedback`. Offsets point to other nodes within the same
file. Leaves set `child_offset` to 0. The root node immediately follows the
32-byte header.

## Generator semantics

Generation runs inside the solver binary
(`./build/solver generate --lookup-start roate --lookup-depth N --lookup-output lookup_roate.bin`)
and follows this pipeline:

```
┌─────────────┐    ┌──────────────────┐    ┌────────────────┐    ┌────────────────┐
│ Load words  │ -> │ Precompute caches│ -> │ DFS w/ memo    │ -  │ Serialize nodes│
└─────────────┘    └──────────────────┘    └────────────────┘    └────────────────┘
                           ▲   ▲                    │
                           │   │                    │
                  letter weights   feedback table   └── produces sparse edges
```

1. **State bitsets + memoization** – Every node corresponds to a candidate
   subset tracked as a bitset over the current word list. These bitsets are
   hashed and memoized so equivalent states reuse cached results.
1. **Scoring with tunable lookahead** – For a given state the generator
   evaluates legal guesses in heuristic order (worst-case candidate count,
   total candidate count, then letter-frequency weight). Lookahead depth is
   configurable so we can peek multiple plies ahead before committing.
1. **Depth enforcement + backtracking** – The generator builds an explicit
   in-memory tree. For each guess it recursively solves every feedback branch.
   If any branch exceeds the remaining depth, that guess is banned and the
   algorithm rewinds to try the next candidate. This repeats until the entire
   state resolves within the Wordle limit (6 turns).
1. **Sparse emission** – After the full tree exists in memory, the serializer
   walks the nodes and emits the binary lookup file. Only reachable feedback
   IDs are stored, and singleton subsets terminate immediately with
   `child_offset = 0`.

Because memoization and lookahead share a cache across the entire run, deep
exploration stays tractable even when we regenerate the tree frequently.

### Pseudocode overview

```
function build_node(state_mask, depth_left):
    if memo.contains(state_mask, depth_left):
        return memo[state_mask, depth_left]

    size = popcount(state_mask)
    if size == 0:
        return failure
    if size == 1:
        return Leaf(only_word(state_mask))
    if depth_left == 0:
        return failure

    best = None
    for guess in all_words:
        partitions = partition_by_feedback(state_mask, guess)
        score = evaluate_partitions(partitions, lookahead_depth)
        score_tuple = (score.max_subset, score.total_candidates,
                       -letter_weight[guess])
        if best is None or score_tuple < best.score:
            best = Choice(guess, partitions, score_tuple)

    node = Node(guess=best.guess)
    for (fb, subset_mask) in best.partitions:
        child = build_node(subset_mask, depth_left - 1)
        if child is failure:
            return try_next_best_guess(...)
        node.add_edge(fb, child)

    memo[state_mask, depth_left] = node
    return node
```

`evaluate_partitions` recursively previews up to `lookahead_depth` additional
moves (reusing memoized states) so we pick guesses that keep the tree shallow.
`try_next_best_guess` means we re-enter the selection loop and examine the
next candidate guess if any branch overflows the depth cap.

### Tunable parameters

Several knobs control generator behavior; adjust them in `solver.cpp` when
experimenting:

- `lookup_depth` (CLI flag `--lookup-depth`, default 6) – maximum turns
  encoded in the tree. Must be ≥ 1 and should match the Wordle guess budget.
- `lookahead_depth` (internal constant, planned CLI flag) – how many plies the
  scorer peers into the future when ranking a guess. Increasing this reduces
  backtracking at the cost of slower generation. The memoization cache keeps
  the asymptotic blow-up manageable.
- `frequency_weights` – derived from `words.txt` once and cached for the
  entire run. Modify the weight formula here if you want to bias tie-breaks
  differently (e.g., favor prior answers, or penalize duplicate letters more
  aggressively).

Documenting these parameters here ensures future contributors know where to
look when tweaking heuristics or aligning the generator with new rulesets.

## Runtime behavior

Runtime solving is a simple traversal:

1. Load the root node (start word) and emit its guess.
1. Compute the feedback for the user-supplied target using
   `calculate_feedback_encoded`.
1. Binary-search the node’s entry list for that feedback ID. If no entry
   exists, the solver reports failure (this signals an incomplete lookup
   file).
1. Follow the child pointer and repeat until (a) the solver enters a leaf
   whose stored guess equals the target (success) or (b) depth 6 is exceeded
   (failure).

`--debug` mode logs each hop, including the depth, feedback ID, and the guess
stored in the lookup table so we can diagnose any unexpected paths.

## Regeneration

```
./build/solver generate --lookup-start roate --lookup-depth 6 --lookup-output lookup_roate.bin
```

The solver filters candidates and runs the search internally for each branch,
ensuring the lookup mirrors the exact runtime behavior. Adjust
`--lookup-start`, `--lookup-depth`, and `--lookup-output` as needed (though
the shipping binary assumes depth 6 and `roate`).

# Feedback Cache Format (`feedback_table.bin`)

`feedback_table.bin` caches the result of `calculate_feedback_encoded` for
every ordered pair of valid words drawn from `words.txt`. The layout stays
minimal so it can be memory-mapped efficiently (~167 MB for 12,947² entries):

- No header; the size implicitly equals `kWordsCount * kWordsCount` bytes.
- Rows and columns follow the exact order of `words.txt` (and therefore
  `kEncodedWords`).
- Each byte stores a single feedback code in `uint8_t` form (0–242), identical
  to the runtime base-3 encoding (`ggggg` = 242).

When present, the solver memory-maps the file and indexes it via the pre-built
lookup tables, so fetching `(guess_idx, answer_idx)` is an O(1) byte read. If
the file is missing or stale, rebuild it with:

```
./build/solver generate --feedback-table
```

or any other mode plus `--feedback-table`. The generator rewrites the file
atomically by iterating every `(guess, answer)` pair and writing the byte
matrix in row-major order.

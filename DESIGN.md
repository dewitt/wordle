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

## Regeneration
```
python3 tools/generate_lookup.py --depth 3 --start roate
```
The tool invokes the solver in `--dump-json` mode for every official answer, builds the sparse tree up to the requested depth, and writes the binary file. Adjust the `--start` and `--depth` flags to target other start words or deeper tables.

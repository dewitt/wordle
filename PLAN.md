# Plan: Lookup Table Generation & Integration

1. Add TODO entries and track progress.
2. Update DESIGN.md with exhaustive lookup requirements (header fields, node format, generator behavior).
3. Extend solver CLI parsing (new `--generate-lookup` flag, optional `--start`, `--depth`, `--output`).
4. Implement lookup generator inside solver:
   - Reuse existing filtering & entropy search.
   - Serialize tree in documented binary format.
5. Remove Python lookup generator (tools/generate_lookup.py).
6. Update README (new workflow for generating lookup tables).
7. Ensure code formatting, remove unused files, clean repo.
8. Run benchmark/tests to confirm behavior.
9. Commit with clear message.


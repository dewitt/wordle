# Plan: CLI Ergonomics & Lookup Improvements

1. Track work via TODO.md.
2. Update solver to expose explicit modes (`solve`, `start`, `generate`, `help`) and rename binary to `solver`.
3. Centralize flag parsing so flags are mode-independent; merge verbose/debug logging into a single `--debug`.
4. Update docs/scripts (README, benchmark.py) and DESIGN notes for new CLI workflow.
5. Keep lookup generation fully inside C++; remove old Python generator and provide helper `tools/dump_lookup.py`.
6. Ensure code formatting, remove unused files, clean repo.
7. Run benchmarks/tests (`python3 benchmark.py --limit 10`, sample solves).
8. Commit changes with clear message.

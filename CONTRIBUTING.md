# Contributing

Contributions are welcome through focused pull requests.

1. Build the project with a C++20 compiler.
2. Add or update tests for behavior changes.
3. Run the release test suite and, when available, ASan/UBSan.
4. Keep benchmark changes separate from functional changes when practical.
5. Document the exact machine, compiler, flags, and workload for new performance
   claims. Never replace matching-core results with end-to-end wording.

The matching book is single-writer by design. Changes that introduce shared mutation
or dynamic allocation on normal book-storage paths should explain the tradeoff.

Please keep commits small enough to review and avoid committing build directories,
generated binaries, journals, snapshots, or machine-specific configuration.

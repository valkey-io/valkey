# AGENTS.md

## Scope
- These instructions apply to the entire repository unless a deeper `AGENTS.md` overrides them.

## Repo overview
- This is the Valkey server codebase.
- The main implementation lives under `src/`.
- Unit tests live under `src/unit`
- Integration tests live under `tests/`.
- Top-level `Makefile` forwards most targets into `src/Makefile`.

## Working guidelines
- Keep changes minimal and easy to backport.
- Match the style of the surrounding code instead of introducing new patterns.
- Avoid unrelated refactors in the same change.

## Build
- Default build: `make`
- Clean rebuild when build settings or bundled deps change: `make distclean && make`

## Unit tests
- Unit tests live under `src/unit/` and use GoogleTest (gtest/gmock).
- Build and run all unit tests: `make -C src test-unit`
- Unit tests cover data-structure and low-level logic changes.
- A single test filter can be run with: `make -C src test-unit && ./src/unit/valkey-unit-gtests --gtest_filter='<TestSuite>.<TestName>'`
- **Write tests in minimal C++.** GTest provides the test runner, but test code should look like C. No STL containers (`std::vector`, `std::string`, `std::set`), no STL algorithms, no `auto`, no lambdas, no templates, no RAII. Use fixed-size C arrays, `sds`, `qsort`, and explicit types. See `src/unit/README.md` for full guidelines and examples.

## Integration tests
- Integration tests live under `tests/` and are written in Tcl.
- Run the full integration suite: `make test` (from the repo root).
- Run a single test file: `./runtest --single <path/to/test.tcl>`
- Additional specialized suites:
  - Cluster tests: `./runtest-cluster`
  - Sentinel tests: `./runtest-sentinel`
  - Module API tests: `./runtest-moduleapi`
- For targeted validation, run the smallest relevant test scope first before broader suites.

## Code style
- Follow the repository conventions described in `DEVELOPMENT_GUIDE.md`.
- Most formatting is enforced by `clang-format`.
- CI uses `clang-format-18` across `*.c`, `*.h`, `*.cpp`, and `*.hpp` files.
- When touching C/C++ sources or headers, run `clang-format-18 -i` on the modified files before finalizing when the tool is available.
- Use comments for non-obvious behavior and rationale, not for restating code.

## Tests
- Code changes should include relevant tests when the repo already has a matching test location.
- Data-structure and low-level logic changes usually belong in `src/unit/` (C++ gtest).
- End-to-end behavior changes usually belong in `tests/` (Tcl integration tests).
- If behavior or commands change, check whether related documentation also needs updating.

## Files to avoid touching unless required
- Do not commit local runtime artifacts such as `dump.rdb`, `nodes.conf`, `*.log`, or ad hoc cluster directories unless the task explicitly requires them.
- Treat vendored dependency code under `deps/` as special-case changes; modify it only when the task clearly requires it.

## Pull Requests
Always push to the user's fork. Never push to the upstream valkey-io/valkey repository. Never push directly to unstable. If a user fork does not exist, ask the contributor to create one.

---
applyTo:
  - "src/**/*.{c,h}"
  - "valkey.conf"
---

# Valkey Core Engine Review Standards

Apply these standards to core engine C code. Do NOT apply to `deps/` (vendored dependencies).

## 1. Code Style (from DEVELOPMENT_GUIDE.md)
- **Formatting:** Follow clang-format (4-space indent, no tabs, braces attached).
- **Comments:**
  - C-style `/* ... */` for single or multi-line.
  - C++ `//` only for single-line.
  - Multi-line: align leading `*`, final `*/` on last text line.
  - Document *why* code exists, not just *what*. Document all functions.
- **Line Length:** Keep below 90 characters when reasonable.
- **Types:** Use the boolean type for true/false values.
- **Static:** Use `static` for file-local functions.

## 2. Naming Conventions
- **Variables:** `snake_case` or lowercase (e.g. `cached_reply`, `keylen`).
- **Functions:** `camelCase` or `namespace_camelCase` (e.g. `createStringObject`, `IOJobQueue_isFull`).
- **Macros:** `UPPER_CASE` (e.g. `MAKE_CMD`).
- **Structures:** `camelCase` (e.g. `user`).

## 3. Safety & Correctness
- **Memory:** Strict check for buffer overflows and leaks.
- **Strings:** Validate `sds` string handling.
- **Concurrency:** Verify thread safety in threaded I/O paths.

## 4. Design Guidelines
- **Configuration:** Avoid new configs if heuristics suffice. Only add for explicit trade-offs (CPU vs memory).
- **Metrics:** No new metrics on hot paths without zero-overhead proof.
- **PR Scope:** Separate refactoring from functional changes for easier backporting.

## 5. Testing & Documentation
- **Unit Tests:** Required for data structures in `src/unit/`. Test files should follow `test_*.c` naming.
- **Integration Tests:** Required for commands in `tests/`.
- **Command Changes:** New/modified commands need corresponding updates in `src/commands/*.json`.
- **New C Files:** Remind to update `CMakeLists.txt` when adding new `.c` source files.
- **License:** New files need BSD-3-Clause header. Material changes (>100 lines) also require it.
- **Documentation:** User-facing changes need docs at [valkey-doc](https://github.com/valkey-io/valkey-doc).

## 6. Critical Escalation
- **Trigger:** Changes to `cluster*.c`, `replication.c`, `rdb.c`, `aof.c`.
- **Action:** Comment mentioning **@core-team** to request architectural review.

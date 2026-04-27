# Review Notes

<!-- metadata: topic=review; audience=ai-agents,developers -->

Self-assessment of the generated documentation in this directory.

## Consistency

- Terminology is aligned across files: `valkey-server`, `valkeyServer` struct, `robj`, `serverCommand`, `kvstore`, `hashtable`, `listpack`. No conflicting names between `architecture.md`, `components.md`, and `data_models.md`.
- `dict` vs `hashtable`: consistently described as legacy vs current in both `components.md` §4 and `data_models.md` §5.
- `tests/cluster/` is consistently marked deprecated in `components.md` §13, `interfaces.md` §9, and `workflows.md` §10, matching `.github/instructions/integration-tests.instructions.md` and `DEVELOPMENT_GUIDE.md`.
- Build-flag names (`BUILD_TLS`, `BUILD_RDMA`, `BUILD_LUA`, `USE_SYSTEMD`, `USE_LTTNG`) match `README.md` and the `src/Makefile` exactly.

## Completeness

The docs cover the breadth of the codebase without diving into per-function detail. Known gaps and why they are left out:

### Intentionally shallow

- **Precise line numbers and sizes.** Avoided by policy — they drift and become misleading. The index points readers to read the source directly.
- **Specific command semantics.** Out of scope; covered by `src/commands/*.json` and the upstream `valkey-doc` repo.
- **Detailed config directive listing.** `valkey.conf` and `src/config.c` are authoritative.

### Deserve manual follow-up if you care about these areas

- **Sentinel internals.** `components.md` §6 lists `sentinel.c` and notes it is monitor + failover; the state-machine detail is not enumerated. If working on Sentinel, read `tests/sentinel/` and `sentinel.c` directly.
- **Client tracking (RESP3 invalidations).** `tracking.c` is listed but the invalidation protocol is not described. See `tests/unit/tracking.tcl`.
- **Functions vs `EVAL` semantic differences.** Both are listed in `interfaces.md` §4; the effect-isolation rules, replication modes, and engine interaction surface are summarized at a high level only. For depth, read `src/functions.c` and `src/script.c`.
- **Cluster gossip packet details.** Noted as living in `cluster_legacy.h`; the packet-type enum and the v1/v2 handshake differences are not enumerated here.
- **Module event hooks (`RedisModule_SubscribeToKeyspaceEvents`, cluster message callbacks, etc.).** `interfaces.md` §3 names the concepts; the full hook taxonomy is not listed.
- **Vector sets (`vset.c`).** Mentioned as a data type but the algorithm family (HNSW-like) is only hinted at; see the source for authoritative detail.
- **Atomic slot migration** — referenced to `design-docs/atomic-slot-migration.md`; design-doc content is not inlined.
- **RDMA configuration and lifecycle.** Mentioned at a high level; `tests/rdma/` and `src/rdma.c` are the detailed sources.
- **LOG_REQ_RES / request-response logging mode.** Referenced as a compile-time switch in `src/Makefile` but the workflow is not elaborated.

### Not enumerated

- Contents of `utils/generate-*.py` beyond their purpose.
- Per-file descriptions of `deps/jemalloc/` — considered vendored and usually not touched. See `deps/README.md`.
- Specific encoding-conversion thresholds — covered at a high level; exact defaults change by version. Grep for `hash-max-listpack-entries`, `list-max-listpack-size`, etc., in `src/config.c` when needed.

## Language-Support Gaps

The generator is most confident about C and Tcl (the project's primary languages). Observations:

- C: well-covered via `src/` exploration.
- C++: covered at the test layer. No production C++ in this repo.
- Tcl: covered via `tests/` READMEs and tag table.
- Python / Ruby / Shell utilities under `utils/` are named but their internal structures are not analyzed.
- JSON command schemas under `src/commands/` are described at the pipeline level; no per-command summary.

## Recommendations

1. **Treat `AGENTS.md` at repo root as the minimum context file.** It is designed to be compact and to delegate to this knowledge base for depth.
2. **Refresh this knowledge base after large refactors.** Re-run the `codebase-summary` SOP when subsystems are renamed, merged, or split (e.g., cluster work, persistence changes).
3. **Do not rely on docs for line numbers or byte counts.** Always re-read the source for precise locations.
4. **When extending modules, scripting, or cluster code, open the corresponding design-doc (if any) in `design-docs/` alongside these files.**
5. **When adding a new top-level subsystem**, add an entry to `components.md` and an arrow in the `architecture.md` diagram, and cross-reference the new files from `interfaces.md` if there is an external surface.

## Cross-File Sanity Checks Performed

- `components.md` file counts (47 module tests, 425 command JSONs) corroborated against `ls` results.
- `OBJ_*` constants and `OBJ_ENCODING_*` names verified against `src/server.h`.
- `struct valkeyServer` and `struct serverCommand` presence verified in `src/server.h`.
- `__attribute__((weak)) int main` verified in `src/server.c`.
- Binary list matches `ENGINE_SERVER_OBJ` / `ENGINE_CLI_NAME` / `ENGINE_BENCHMARK_NAME` in `src/Makefile`.
- LTTng provider list matches `src/trace/README.md`.
- Test tag list matches `tests/README.md`.
- Build-flag names match `README.md` and `src/Makefile`.

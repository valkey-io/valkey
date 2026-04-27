# Rough Idea — Inline (Real-time) Data Compression for Valkey

## Source

Valkey GitHub issue [#3423 — Add support for inline in-memory value compression](https://github.com/valkey-io/valkey/issues/3423), opened by @ikolomi.

## User-provided context

The main aim is to **reduce RAM usage**. Our internal POC has shown that in order to achieve a high level of compression — also for small data — we need to use **ZSTD with a dictionary** (as opposed to LZ4). Using a dictionary introduces challenges such as:

- Dictionary creation
- Dictionary persistency
- Countering drift (when the dictionary becomes inefficient as the dataset changes)
- Other dictionary lifecycle aspects

Additional constraints and design guidance:

- Must **minimize latency on the hot path**, since low-latency data is a priority for Valkey.
  - Probably skip compression for hot items tracked by LRU/LFU.
- **Leverage worker threads** (possibly reuse `io-threads`) for processing keys in multi-key commands.
- **RDB and the in-memory dict must support both compressed and uncompressed items** side by side.
- **Minimize the blast radius** by encapsulating the feature deeply in the dict/hashtable layer so existing mechanics (e.g., replication) are affected as little as possible.
- Must be **opt-in**, with configurations and metric / statistic reporting.
- Should expose **admin commands** to enable / disable / configure compression remotely at runtime.
- **Initial target: `STRING` data type.** Design and implementation must allow future extension to other data types.

## Summary of issue #3423

- Memory cost is a major constraint for a meaningful subset of Valkey workloads; rising RAM costs increase the value of memory-efficiency work.
- AWS ElastiCache internal analysis: production datasets show roughly **~74% compression savings** on memory-constrained workloads.
- Application-level compression is today's workaround; it complicates app logic, reduces debuggability, and is hard to apply consistently across fleets.
- Native server-side inline compression fills this gap for memory-bound workloads.

### Key properties from the issue

- Transparent from the application's point of view
- Opt-in
- Bounded CPU overhead with clear tradeoffs
- Observability: compressed bytes, uncompressed bytes, compression ratio, compression/decompression activity
- Compatible with persistence and replication, or at minimum clearly documented interaction

### Design axes the issue calls out

- Which object types are eligible
- Minimum object-size thresholds
- Compression algorithm choice (ZSTD + dictionary per internal POC)
- Behavior under updates and partial modifications
- Interaction with eviction, persistence, replication, memory accounting
- User controls and observability

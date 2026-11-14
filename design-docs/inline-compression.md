# Design Document: Inline In-memory Compression

_Issue: [valkey-io/valkey #3423](https://github.com/valkey-io/valkey/issues/3423)_

## Overview

Inline in-memory compression reduces memory used by cold string values. The
feature is opt-in. It uses LZ4 workers to compress values outside the client
request path and keeps the compressed form only when it uses less memory than
the original value.

Clients continue to read and write the same logical bytes. Physical
introspection, such as `OBJECT ENCODING` and `MEMORY USAGE`, may expose the
compressed representation.

The release targets for representative workloads are at least 30% lower memory
use and less than a 20% throughput reduction. The maximum value size bounds the
cost of one compression or decompression operation.

### Scope

The first version supports:

- `OBJ_STRING` values with `OBJ_ENCODING_RAW`;
- the LZ4 block compression backend, without dictionaries;
- background compression of cold values;
- `copy` and `hot` read policies;
- prioritized compression of slot-import writes;
- transparent reads and writes;
- RDB, AOF, replication, slot migration, and module access; and
- resource and performance metrics.

Compression dictionaries, Zstandard, non-string values, and compressed
node-to-node protocols are outside this design.

## Architecture

The main thread owns candidate selection and all keyspace changes. Workers only
read job-owned snapshots and produce immutable frames.

```text
                         +--------------------+
 keyspace scan ----------> candidate checks   |
 slot import ------------> and admission      |  main thread
                         +---------+----------+
                                   |
                          owned byte snapshot
                                   |
                    bounded request queue
                                   |
                         +---------v----------------+
                         | Compression worker pool  |
                         +---------+----------------+
                                   |
                         immutable frame
                                   |
                    bounded completion queue
                                   |
                         +---------v----------+
                         | freshness check    |  main thread
                         | and installation   |
                         +--------------------+
```

Both the number of jobs and their total owned bytes are bounded. Candidate
selection also has per-cycle key and byte budgets, so one event-loop cycle
cannot copy an unbounded amount of data.

## Candidate Selection

A cursor scan visits each database and eventually examines every key. The scan
keeps its position between cron calls and pauses after a pass that finds little
eligible work. It does not perform normal key lookups or update access metadata.

A background candidate must be:

- a raw string with a single owner;
- inside the configured size range; and
- cold under the active LRU or LFU policy.

Candidate selection is bounded to limit memory use and event-loop work.

## Job Safety and Installation

A worker must never access a live keyspace object. At admission, the main thread
copies the value bytes into memory owned by the job. This prevents use-after-free
and torn reads while commands continue to modify the keyspace.

The job retains that snapshot until installation. The main thread installs a
completed frame only when:

- the key still contains a raw string that passes the type, size, and ownership
  checks;
- its length and bytes still match the snapshot; and
- the compressed form uses less memory than the original value.

Before storing a background compression result, Valkey checks that the key is
still cold because it may have been read while the worker was running. Values
received through slot migration do not need to be cold; Valkey queues them for
compression without waiting for them to become cold. In both cases, Valkey
throws away the result if the key was deleted or changed, no longer passes the
checks above, or compression does not save memory.

Compression and decompression do not emit keyspace notifications because they
do not change the logical value.

## Slot Import and Incremental Replication

Atomic slot migration and incremental replication carry uncompressed logical
commands, but their destinations handle compression differently.

The slot-import destination gives eligible values priority over the background
scan. After applying an eligible write, Valkey must admit its compression job
before reading more commands from that slot-import connection. If the job-count
or byte limit is full, Valkey pauses the connection until capacity becomes
available. Slot-import jobs skip the coldness check but retain all other
eligibility and safety checks.

Incremental replication installs the logical value as plain data, just like a
normal client write. It does not create a priority compression job or pause the
primary connection for compression. The normal sweeper may compress the value
after it becomes cold.

## Reads, Writes, and Modules

### Client reads

The `compression-read-policy` setting applies to normal client reads that need
the value bytes:

- **`copy`** - The read uses a temporary copy and leaves the stored value
  compressed, so memory use is the most predictable.
- **`hot`** - The read decompresses the stored value only when the key is hot.
  With LRU tracking, including noeviction mode, the key is hot when its previous
  access was within the configured time window. With LFU, the key is hot when
  its frequency after the read reaches the compression threshold plus a
  configurable margin.

Commands that only inspect key existence, type, expiry, or logical length can
read frame metadata without decompressing the value. The read policy does not
apply to them.

### Reads during a save

While a forked child is running, reads that need the value bytes use temporary
copies regardless of the read policy. This avoids copy-on-write memory growth.
During a forkless save, such a read uses a temporary copy while the save is
using that value.

### Writes

When a write runs, it always decompresses the stored value, regardless of the
read policy. The command then applies its normal change. The raw result becomes
eligible for later background compression.

### Modules

A module key opened for reading receives a handle-owned raw copy when the stored
value is compressed. This behavior does not depend on the read policy. All read
APIs, including `ValkeyModule_StringDMA`, operate on that copy, which remains
valid until `ValkeyModule_CloseKey`.

A module key opened for writing follows the normal write path and decompresses
the stored value. A module therefore observes the same logical string regardless
of its physical encoding.

## Stored Frame

Compressed strings use `OBJ_ENCODING_COMPRESSED`. The SDS payload is an
immutable frame containing:

- the uncompressed length;
- the compression algorithm identifier;
- the frame format version; and
- the compressed body.

Fields use fixed byte order and stable identifiers because RDB files persist the
frame. The format contains no process pointers or library-owned state.

Valkey keeps a compressed value only when it uses less memory than the original
value.

## Persistence and Data Movement

The stored representation is preserved only by RDB-based persistence and full
synchronization. Logical command streams and locally generated single-value
transfer payloads use plain bytes.

| Path | Representation |
|---|---|
| Client writes | Plain; background compression may replace it later. |
| RDB save and RDB-format AOF base | Preserve plain values and compressed frames as stored. |
| RDB load | Restore values as stored. Validate compressed frames by decompressing them into a temporary buffer. |
| Command-format AOF and incremental AOF | Plain logical commands. |
| Incremental replication | Install plain logical values; the normal sweeper may compress them later. |
| Atomic slot migration | Plain command stream; the target prioritizes resulting compression jobs. |
| `DUMP` and legacy `MIGRATE` | Emit ordinary uncompressed RDB string payloads. |
| `RESTORE` | Accept ordinary strings and validated v1 compressed payloads. |

The RDB format uses a dedicated type for compressed strings, and its version is
bumped for this change. A compressed entry contains the frame bytes. A server
that accepts this RDB format must keep support for decoding the v1 LZ4 frame
format, even when new compression is disabled.

Malformed or unsupported compressed data fails RDB loading before the keyspace
becomes available.

For full synchronization, a replica that does not support compressed RDB
strings receives them as ordinary plain strings. If the selected full-sync
transport cannot produce a compatible RDB format, the source rejects the sync
before starting the snapshot instead of sending an unreadable RDB file.
Commands received after the snapshot follow the incremental replication rule
and remain plain until the normal sweeper selects them.

An RDB-format AOF base follows the RDB rules. A command-format base and all
incremental AOF files remain independent of the compressed frame format.

## Resource and Threading Rules

Valkey limits compression work so it cannot use unlimited memory or keep the
event loop busy for too long. It limits the value size, the work started in one
cycle, and the total work waiting or running.

Workers compress private copies of values and never access the keyspace. The
main thread checks each result and decides whether to store it.

Normal client writes do not wait for compression. A slot import may pause when
the compression queue is full. During a forked save, Valkey stops starting new
compression jobs and waits until the child exits before storing completed
results.

The compression mode and worker count are selected at startup. The read policy
and other limits can be changed while Valkey is running. The configuration
documentation lists their names, ranges, and defaults.

## Observability

`INFO compression` reports:

- the active mode and worker count;
- current compressed-value count, original allocation bytes, compressed
  allocation bytes, and saved allocation bytes;
- queued, running, and completed job memory;
- slot-import backpressure;
- scan progress and rejection reasons;
- successful results and results discarded because they were stale or did not
  save memory;
- compression and decompression time; and
- coldness rejections, temporary read copies, and in-place decompressions.

## Failure Handling

Compression is an optimization. Failure to allocate or compress a candidate
leaves its raw value unchanged. A changed or deleted key causes the completed
job to be discarded.

Persisted input is treated differently. A malformed frame, unsupported
algorithm, or decompression-length mismatch fails RDB loading before the
keyspace becomes available. A decompression failure for a frame that was already
validated indicates an internal invariant violation and stops the server rather
than returning incorrect data.

## Testing

Unit tests will cover frame validation, memory-saving decisions, job limits,
and safe handling of results for values that changed while compression was
running.

Integration tests will cover both read policies with LRU and LFU, writes,
modules, RDB and AOF reload, full and incremental replication, slot migration,
legacy migration, forked and forkless saves, and reading stored compressed
values when new compression is disabled. Specific edge cases will be covered by
the tests rather than this document.

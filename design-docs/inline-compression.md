# Inline In-memory Compression for Valkey — Design Summary

_Issue: [valkey-io/valkey #3423](https://github.com/valkey-io/valkey/issues/3423)_

_Note on wording: "compression dictionary" always means the trained block of sample bytes used by the compressor. It is never a Valkey `dict` (hashtable)._

---

## 1. The problem

Valkey keeps every value in memory as a plain SDS string. Many `OBJ_STRING` values contain repetitive data, such as JSON, HTML, or protobuf, but Valkey stores them raw.

AWS ElastiCache fleet data shows that, in 92% of memory-bound snapshots, values between 256 B and 1 MiB compress by **50% or more** with a trained compression dictionary. Memory, not CPU, is the main limit on those hosts.

**Target:** ≥30% memory reduction, under 20% TPS cost, opt-in, invisible to clients.

---

## 2. What we intend to build

We intend to add opt-in, transparent compression for Valkey values, starting with string values. When enabled, Valkey will find cold values in the background and compress eligible values with the selected algorithm. It may also use a trained compression dictionary. Valkey will keep the compressed representation only when it provides a net memory saving. The first client read of a compressed value will transparently decompress it, restore the RAW representation, and mark the key hot. Later reads then avoid decompression, and the sweeper does not immediately recompress the value.

Candidate selection and frame installation remain coordinated with the main event loop. Expensive compression and dictionary training run outside the request path. The design bounds the work in each cycle, checks that a key has not changed before replacing its value, and exposes operational data for measuring savings and cost. Implementation details may evolve, but the goal remains predictable background work without changing Valkey's data semantics.


### 2.1 Scope: v1 and v2

The work comes in two versions.

- **v1** compresses cold string values with LZ4 and no dictionary. It covers the sweeper, the worker pool, the frame, the read path, persistence (§4.3), forkless save (§7.2), compression on slot-migration import and replica apply (§3.2, §3.4), the downgrade option, and the measurements in the TODO notes of this document.
- **v2** adds improvements that v1 works without: the skip table for data that is already compressed (§6.1), the compression dictionary registry (§5), the zstd backend and dictionary training (§8), and a lower read cost: decompress in the I/O threads, or keep the frame on a read of a cold key (§4.2).

The v1 measurements decide if dictionaries and zstd are worth the work.

---

## 3. Data entering Valkey

Data enters Valkey through five paths. Client commands, slot migration, incremental replication, and command-form AOF replay carry logical, uncompressed values. RDB load and an RDB-format AOF base file preserve the stored representation. Client writes rely on background compression. Slot-migration imports and replication-stream updates compress eligible values at once, without waiting until they are cold, which prevents a large transfer or catch-up operation from building an uncompressed copy of the dataset.

### 3.1 Client write commands

A value supplied by a client command enters as uncompressed bytes and is installed with the existing command semantics. The command path does not compress inline because that would make foreground latency depend on the value size and selected algorithm. After installation, the background sweeper may select the value and compress an owned snapshot. It replaces the value only if the key is unchanged and the result provides a net memory saving.

A command that mutates a compressed value first gets its logical, uncompressed representation. The command then applies the mutation and stores the result as plain data. Any later recompression happens in the background.

### 3.2 Slot migration import

Slot migration receives uncompressed logical values. When compression is enabled, the destination compresses each eligible value at once, without the coldness test of the sweeper. It uses the destination's algorithm and active dictionary when applicable. It also applies the same size, format, and net-savings checks as the background path. If the value is ineligible or compression provides no saving, the value stays plain.

The import client installs the plain value as usual. `signalModifiedKey()` then calls `compressorKeyModified()`, which adds only the key name to a set of incoming keys (no value copy, and one entry per key however often it is written). On each event loop (`compressorBeforeSleep()`) and each cron call, the main thread takes keys from the set, oldest first, while there is room in `compression-max-inflight-requests`, and queues normal compression jobs for them (§6.2). The jobs are marked, so the install skips the coldness test, but keeps the byte compare and the net-savings guard (§6.3, §6.4). Incoming keys go before the sweeper. A key that is deleted or changed while it waits is skipped, or its frame is dropped at install. A key that already has a job in flight (from the sweeper, or for an older value) stays in the set and is tried again after that job is done, because that job is dropped at install when the value changed.

Not waiting is deliberate. Installing every migrated value as plain data and waiting for the sweeper could cause a memory spike proportional to the imported slot: a new key is hot, and the snapshot touches a key with a TTL a second time (`PEXPIREAT`). With the queue, the extra plain memory is the values that wait for a worker. The import is not slowed down by compression on the main thread. It also keeps the destination independent of the source node's algorithm and dictionary set.

> **TODO — validate inline migration compression with measurements:** quantify destination peak memory, migration throughput, per-value latency, and event-loop occupancy across value sizes and compression ratios.

### 3.3 RDB load

RDB preserves values **as-is**. A value that was plain when the snapshot was taken is loaded as plain, and a value that was compressed is loaded as the same compressed frame. The loader does not decompress compressed entries or synchronously compress plain entries.

To make compressed entries self-contained, the RDB also stores the frame-format version, algorithm metadata, and every compression dictionary referenced by any frame in the snapshot. Before exposing compressed values, the loader rebuilds the decoder and dictionary registry. It then validates every frame's algorithm and dictionary reference and rebuilds dictionary user counts. An unknown format, unsupported algorithm, missing dictionary, or malformed frame causes the RDB load to fail before the entry reaches the keyspace.

Persisting frames creates a long-term compatibility obligation and requires an RDB format change. Future versions that accept this RDB must continue to decode its frame and dictionary formats. The implementation must define downgrade behavior and behavior for unsupported backends before releasing the format.

### 3.4 Replication stream application

Incremental replication carries normal Valkey commands and uncompressed logical values. When compression is enabled on the replica, each command from the primary that creates or replaces an eligible value adds the key to the incoming keys, as in §3.2, and the result is compressed at once by the workers. A mutation of an existing compressed value obtains its logical representation, applies the command, and the result is compressed again when eligible. Only the primary client and slot-import clients add incoming keys; a normal client write waits for the sweeper. The replica uses its local algorithm and active dictionary when applicable; it does not need the primary's dictionary for incremental updates.

Compressing at once prevents replication catch-up from building an unintended uncompressed copy of the changed dataset while waiting for a later sweep. The cost: each write from the primary to an eligible value is one compression job, and a read on the replica decompresses it again, so a write-heavy workload costs more CPU on a replica. Values that are ineligible or do not pass the net-savings guard remain plain. Primary and replica may therefore use different physical representations while retaining the same logical dataset.

During full synchronization, the incoming snapshot follows §3.3: compressed and plain values are loaded as-is together with all referenced dictionaries. Commands buffered after the snapshot offset then arrive through the uncompressed incremental stream and follow the rule above.

> **TODO — validate inline replication compression with measurements:** quantify replica peak memory, apply throughput, replication lag, event-loop occupancy, and catch-up time with and without inline compression.

### 3.5 AOF load

AOF loading follows the format of each multipart AOF component. When the base file was created with `aof-use-rdb-preamble yes` (the default), it is an RDB-format base and loads exactly as described in §3.3: plain and compressed values are restored as-is, all referenced dictionaries and decoder metadata are loaded before the keyspace becomes visible, and malformed or unsupported compressed state fails the load.

When `aof-use-rdb-preamble no`, the base file contains logical commands and uncompressed values. Incremental AOF files also contain logical commands and uncompressed values regardless of the preamble setting. The AOF loader replays those commands from their uncompressed representation; it does not need compression dictionaries to parse command-form files. Values produced by command replay remain plain until normal background compression selects them. A mixed multipart AOF can therefore restore compressed values from its RDB base and then apply plain command updates from its incremental files.

This distinction creates two recovery profiles. An RDB-format base preserves existing memory savings but carries the same durable frame, algorithm, and dictionary compatibility requirements as RDB. A command-format base is independent of compressed frame formats but can rebuild the dataset at the uncompressed memory baseline until the sweeper catches up.

> **TODO — validate AOF load with measurements:** compare startup time, peak memory, dictionary loading, and time to recover steady-state compression for RDB-preamble and command-format bases, each with incremental files.

---

## 4. Data leaving Valkey

Data leaving Valkey follows one of two rules. RDB and an RDB-format AOF base preserve the stored representation. Client-visible and node-to-node logical interfaces, along with command-form AOF files, emit uncompressed values. A client read decompresses a compressed value, keeps the plain value in the keyspace, and marks the key hot (§4.2). Other outgoing logical interfaces decompress into a temporary buffer without changing the stored value.

> **TODO — validate synchronous decompression with measurements:** benchmark latency, throughput, event-loop occupancy, and peak temporary memory for each outgoing path before finalizing the v1 execution model. If the numbers are not acceptable, move decompression off the main thread or introduce bounded incremental work.

### 4.1 Slot migration export

Slot migration sends uncompressed logical values. A forked child writes the snapshot as `SET` commands (`childSnapshotForSyncSlot()` → `rewriteAppendOnlyFileRio()`). `rioWriteBulkObject()` decompresses each compressed value into a temporary buffer, so the parent's memory and stored frame do not change. After the snapshot, the stream carries plain client writes. The destination therefore needs no knowledge of the source algorithm or dictionaries. It processes each value as described in §3.2. This works with any target version.

Sending frames as they are, to save bandwidth, needs two more things. First, the target must say that it can read frames before the source forks the snapshot. `CLUSTER SYNCSLOTS CAPA` is a one-way message and can arrive after the fork, so one option is a `+OK frames` reply to `ESTABLISH` (an old source accepts any reply that does not start with `-`). Second, the snapshot is a list of AOF commands, so a frame needs `RESTORE` with `RDB_TYPE_STRING_COMPRESSED` (§4.3).

A key with a TTL is sent as `SET` and then `PEXPIREAT`. On the target, the `PEXPIREAT` touches the key and raises its LFU counter, so the key arrives hot and waits until the counter decays (about one minute per step). Compression on import (section 3.2) removes this wait.

> **TODO — back the wire-format decision with numbers:** compare migration duration, network bytes, source CPU, main-thread stall time, and destination memory for plaintext transfer versus a compression-aware protocol.

### 4.2 Client reads

Reads return uncompressed logical values. An uncompressed value follows the existing path. On the first read of a compressed value, Valkey synchronously decompresses it and replaces the stored frame with the RAW value. It also adds the key to the internal hot-key LRU described in §7.3. Later reads use the RAW value directly. Every read refreshes the key's hot-key entry, and the background sweeper excludes keys that remain in that LRU. This approach pays the decompression cost once when a cold key becomes active, rather than on every read.

This tradeoff is deliberate: the first read gives back that key's memory saving. A lower read cost, with decompression in the I/O threads or with the frame kept on a read of a cold key, is part of v2 (§2.1). The value becomes eligible for compression again only after its hot-key entry expires or is evicted and the normal coldness gate in §6.1 passes.

> **TODO — back decompress-on-read with numbers:** measure p50, p99, and p99.9 first-read latency, steady-state hot-read latency, throughput, event-loop stall time, memory returned by decompress-on-read, and recompression attempts across value sizes and multi-value commands.

### 4.3 RDB save and full synchronization

RDB save writes each value **as-is**: plain values remain plain and compressed values remain compressed. It also serializes the frame-format and algorithm metadata plus every compression dictionary referenced by the snapshot. The snapshot must never contain a frame whose dictionary is absent, including when retraining or dictionary retirement overlaps a background save.

The format is RDB version 82. A compressed value has the type `RDB_TYPE_STRING_COMPRESSED` (24), followed by the frame bytes as they are, without LZF. The type is chosen by the target RDB version (`rdbGetObjectType()`): a target that supports only an older version, such as a full sync to a Valkey 9.2 replica, gets the plain bytes as `RDB_TYPE_STRING`. `DUMP` payloads carry RDB version 82, so `RESTORE` on an older server fails, as for any RDB version change.

A full synchronization uses this RDB representation as its baseline. The replica loads the snapshot as described in §3.3 and then applies the buffered, uncompressed replication commands produced after the snapshot offset. This preserves in-memory savings across full sync without making the incremental replication stream compression-aware.

### 4.4 Replication stream propagation

Incremental replication sends uncompressed logical commands and values. Commands that already carry client-provided bytes can propagate those original plain bytes. Any propagation path that must materialize a value from the keyspace will synchronously decompress a compressed value before encoding the replication command, without replacing the stored frame.

> **TODO — back the replication decision with numbers:** quantify replication bandwidth, primary CPU, main-thread stall time, replica catch-up time, and backlog pressure for uncompressed propagation versus a negotiated compressed stream.

### 4.5 AOF append and rewrite

Incremental AOF persistence always writes logical commands and uncompressed values. Commands that already carry client-provided bytes append those original bytes. If an AOF path must materialize a value from the keyspace, it decompresses a compressed value for command encoding without replacing the stored frame. Incremental files never reference a compression dictionary.

AOF rewrite depends on `aof-use-rdb-preamble`:

- With `aof-use-rdb-preamble yes` (the default), the new base file is RDB format and follows §4.3. It writes values as-is and includes the frame-format and algorithm metadata plus every dictionary referenced by a compressed frame.
- With `aof-use-rdb-preamble no`, the new base file is a command-form AOF. The rewrite child emits logical commands with uncompressed values. It temporarily decompresses compressed values while encoding those commands and does not change the primary's stored representation.

A multipart AOF may therefore contain a representation-preserving RDB base followed by uncompressed incremental command files. The RDB base carries the durable compatibility requirements of §3.3; the incremental files remain independent of compression algorithms and dictionaries.

> **TODO — validate AOF persistence with measurements:** compare rewrite duration, child peak memory, output size, copy-on-write impact, and load time for both preamble settings with plain, compressed, and mixed keyspaces.

---

## 5. The compression dictionary, and why we hold more than one

Dictionaries are part of v2 (§2.1). In v1, every frame has `dict_id` 0.

Normal compression finds repeated data **inside one value**. A 500-byte JSON value may contain few repeats, so it often compresses poorly on its own.

A compression dictionary is a block of sample bytes trained from real values in the keyspace. The compressor treats it as text that came just before the value, so it can point back into the dictionary. For example, `"user_id"` may appear only once in a value but also appear in the dictionary, allowing it to shrink to a few bytes.

**A compression dictionary is frozen once trained.** A compressed frame does not contain words; it contains offsets into the dictionary. Editing the dictionary would move those offsets and break every existing frame that uses it. The dictionary therefore remains read-only for its entire life.

That is why retraining creates a **new, separate** compression dictionary with a new ID rather than updating the old one:

| State | Live compression dictionaries |
|---|---|
| Enabled, before first training | 0 |
| Steady state | **1** — the active one |
| After a retraining | **2** — new active, old draining |
| Slow drain plus another retraining | up to 4 (`compression-dict-max-versions`) |

**Two is the normal case we design for.** New compressions use the active dictionary. Frames made earlier still need the old one, so a small registry keeps it alive until its user count reaches zero. Each registry entry records the algorithm and dictionary ID needed to decode its frames. RDB save persists every referenced entry, and RDB load rebuilds the registry and its user counts before compressed values become visible (§3.3 and §4.3). Each frame counts as one user; freeing a frame makes the registry drop that count. The backend's `release()` hook is optional (it may be NULL, as for LZ4) and only drops per-frame library state, if the backend keeps any; it never changes the user count. No background job hunts down old frames.

**Dictionary IDs are never reused.** `dict_id` is a 32-bit number that counts up for the life of the data, so a frame can never point at the wrong dictionary. `0` means "no dictionary".

**When to compress before the first dictionary.** With `zstd`, the sweeper does not compress until the first dictionary exists. Without a dictionary small values save little, those frames are never compressed again with the new dictionary (the sweeper only picks plain values), and training needs plain sample values. With `lz4`, values are compressed without a dictionary (`dict_id` 0) from the start.

Retraining is triggered by drift: when the live compression ratio gets clearly worse than the ratio right after the last training. Also on first reaching `compression-dict-min-training-keys` (default 1000), on an optional interval, and manually via `COMPRESSION TRAIN`.

---

## 6. Background compression path

### 6.1 Choosing a value

On each `compressorCron()` call, the sweeper first checks `compression-max-inflight-requests`. An in-flight request remains counted from enqueue until the main thread installs or discards its result, including queued, running, and completed-but-not-drained jobs. If the in-flight count has reached `compression-max-inflight-requests`, 1000 by default, the sweeper does not sample.

When capacity exists, the sweeper picks keys with one of two policies, set by `compression-sweeper-policy`: `cursor` (the default) or `random`. Both look at no more than `compression-sweeper-samples` keys per cron call, 500 by default (from the benchmark, §6.1.1). It is a separate setting from `compression-max-inflight-requests`, so a large in-flight cap does not add work to one cron call. `random` reads the samples in batches of `COMPRESSOR_SWEEPER_SAMPLE_BATCH` (128), so its array on the stack does not grow with the setting. The policy can change at runtime. The cursor keeps its place when the policy changes.

**`cursor`.** The sweeper scans all databases with `kvstoreScan()`, one bucket at a time. It keeps the database index and cursor between cron calls. When a database cursor returns to 0, the sweeper moves to the next database. After the last database, the pass is complete and `compression_total_sweeper_passes` increases. One pass checks each key once.

One cron call visits at most `compression-sweeper-samples` buckets, so a table with many empty buckets cannot make the call unbounded. A scan step returns every key in one bucket and its chain and cannot stop midway through that bucket. The sweeper stops checking keys when it reaches the key limit or the in-flight cap. It records the cursor and how many keys from the current bucket it has handled. The next call scans the same bucket, skips those keys, and continues. One call therefore checks at most `compression-sweeper-samples` keys. For every key beyond the limit in the final bucket, the callback compares only two counters. Hashtable resizing keeps this extra bucket-chain work short.

Keys remain in the same order if the bucket does not change. If it changes, a key may be checked twice or wait for the next pass. Each call handles at least one new key from the bucket, so a value that is queued again, such as one that does not compress, cannot block later keys. `kvstoreScan()` handles rehashing, `FLUSHALL`, and `SWAPDB`. At worst, a key is checked twice or waits for the next pass. It also skips importing hashtables.

**Backoff (`cursor` only).** When the data is stable, each pass checks every key and finds nothing. At the end of a pass, the sweeper pauses for `compression-sweeper-backoff-seconds` if fewer than `COMPRESSOR_BACKOFF_MIN_ELIGIBLE_PCT` (1%) of the checked keys were eligible. The default pause is 60 seconds; 0 disables backoff.

A pass that checks no keys does not pause. For example, a scan skips importing slots during slot import, and those keys should be compressed soon after the import ends. While paused, `compressorCron()` returns immediately and `compression_sweeper_paused` is 1. The pause uses the monotonic clock. After it ends, a new pass starts.

Changing the setting wakes the sweeper immediately. `CONFIG SET` does not call the apply function when the value is unchanged. If this wake occurs in the middle of a pass, that pass cannot start a pause when it ends because part of the keyspace was scanned before the wake. If the setting changes during a pause, the next pass starts from the beginning and may start another pause when it finishes.

The threshold is 1%, rather than 0%, because a pass over 1 million keys that finds only a few new cold keys costs almost as much as a pass that finds none. Those few keys can wait for the next pass. The 60-second default matches the time that a new or recently read key stays hot. The `random` policy has no backoff.

**`random`.** The sweeper chooses a non-empty database with probability proportional to its key count, selects one of that database's hashtables with `kvstoreGetFairRandomHashtableIndex()`, and calls `kvstoreHashtableSampleEntries()` for up to the remaining budget. If the selected hashtable has fewer entries, it repeats the database and hashtable selection until the budget is exhausted or the in-flight request cap is reached. The kvstore helper excludes importing hashtables.

`kvstoreHashtableSampleEntries()` returns no more entries than requested and does not return the same entry twice within one call. With `random`, samples can repeat across cron calls, and no cursor or completed-pass state is kept. Selection is therefore probabilistic rather than an eventual-coverage guarantee (section 6.1.1). `INFO compression` reports sampled entries and eligibility rejection reasons so operators can measure repeated work and convergence as the share of compressed values rises.

The sweeper reads each sampled database entry directly. Sampling must not perform a normal key lookup, refresh LRU/LFU metadata, or add the key to the hot-key LRU. Candidate discovery is not a client access.

A sampled value is cold according to the access metadata selected by the maxmemory policy. In LFU mode, the sweeper applies normal LFU decay and accepts a frequency at or below `compression-lfu-threshold`, 4 by default. A new key starts at 5 (`LFU_INIT_VAL`), so it is hot after it is written. With the default `lfu-decay-time` of 1, the counter decays to 4 within one minute, like the 60-second idle test in LRU mode. With `lfu-decay-time 0`, the counter never decays, and a new key is never compressed, unless `compression-lfu-threshold` is 5 or more. In LRU and `noeviction` modes, it accepts an idle time at or above `COMPRESSOR_MIN_IDLE_SECONDS`, 60 seconds (a fixed value in v1, section 9). The object's 24-bit field stores one of these forms, so the sweeper never applies both tests.

After the coldness test, the sweeper takes a value only if a compression backend is active and the value is `OBJ_STRING`, `OBJ_ENCODING_RAW`, sole-owner (`refcount == 1`), within the size limits, and absent from the internal hot-key LRU (§7.3). The hot-key LRU is an independent first-line exclusion: it blocks recently read keys before object metadata is evaluated, while the policy-specific coldness gate remains a fallback if that bounded LRU evicts an entry. Compressing a hot key would waste worker CPU and make its next read pay synchronous decompression again.

Dictionary-capable modes use the active dictionary when one is available. A backend may also create a frame without a dictionary—for example, an LZ4 configuration that does not use one—in which case `dict_id` is zero. The frame still records the algorithm and format needed to decode it (§6.4).

**Skip data that is already compressed** (v2, §2.1). Before the snapshot copy, the sweeper compares the first bytes of the value against a small table. A match means the bytes are already compressed, so an attempt would spend a main-thread `memcpy` (§6.2), a worst-case-size allocation, and worker CPU to learn nothing.

| Format | First bytes |
|---|---|
| PNG | `89 50 4E 47 0D 0A 1A 0A` |
| JPEG | `FF D8 FF` |
| GIF | `47 49 46 38`, "GIF8" |
| WEBP | `52 49 46 46`, "RIFF", then `57 45 42 50`, "WEBP", at offset 8 |
| gzip | `1F 8B` |
| zip | `50 4B 03 04` |
| zstd | `28 B5 2F FD` |
| xz | `FD 37 7A 58 5A 00` |
| bzip2 | `42 5A 68`, "BZh" |
| 7z | `37 7A BC AF 27 1C` |
| MP4 | `66 74 79 70`, "ftyp", at offset 4 |
| base64 PNG | `iVBORw0KGgo` |
| base64 JPEG | `/9j/` |

Two rules keep this honest. The list does **not** need to be complete: the net-savings guard (§6.4) still catches every format nobody listed, and a custom container that wraps a JPEG. And the check is a **hint, not a verdict** - it only avoids work, and it must stay a `memcmp` on the first bytes with no allocation, next to the size window and the coldness gate.

`INFO compression` counts values skipped this way, so an operator can see that a keyspace is full of images instead of guessing why the ratio is flat.

**Where it runs.** `compressorCron()`, called from `serverCron()`, picks keys and queues jobs. With `random`, the database is picked with a 64-bit random number (`genrand64_int64()` with rejection sampling), so a key count above 2^31 is covered. A small in-flight set, keyed by database and key, keeps a key out of two jobs at once. `compressorBeforeSleep()`, called from `beforeSleep()`, installs the finished frames (§6.3). Each call site costs one check when compression is off.

**Pause during a child process.** While a child process runs (`BGSAVE`, `BGREWRITEAOF`), the sweeper neither queues new jobs nor installs finished frames. Installing a frame frees the plain value and writes a new buffer. This changes memory that the child still shares and forces the kernel to copy those pages. Finished jobs therefore remain queued until the child exits. The byte comparison during installation (§6.3) detects any value change made while they wait.

A client read during a child process also leaves the stored value compressed (§7.2). It gets a temporary plain copy with the same key, TTL, and LRU/LFU data. Without this rule, a client that reads many compressed keys during a save could force the kernel to copy a large part of the dataset. The copy is released when the command that created it ends, including a command inside a `MULTI`, script, or module call. A script that reads many keys therefore holds only one copy at a time. If the reply holds the copy, its own reference keeps the copy alive until it is sent.

A write during a child process still decompresses the stored value in place because the command changes the key anyway. `PFCOUNT` does the same because it writes its cached count into the stored value.

Modules follow a stricter rule, with or without a child process. A module key handle opened for reading always gets a private plain copy, so scanning the keyspace does not decompress every stored value. The handle owns and frees the copy. `VM_StringDMA()` returns NULL for such a handle and sets the length to 0. A write into the copy would be lost, and the pointer would be freed by `VM_CloseKey()` earlier than it would be for a plain value. A module that needs the bytes of a compressed value must open the key for writing or use `VM_Call("GET")`. This compromise may change later. A module key handle opened for writing decompresses the stored value in place.

The sweeper does not use a wall-clock or CPU-percentage budget. Candidate work per cron call is bounded by `compression-sweeper-samples`, 500 keys by default (and, with `cursor`, as many buckets). At the default and `server.hz = 10`, it checks at most 5,000 keys per second, and less when the request cap is full. With `cursor`, one pass over 1M keys takes about 200 seconds.

For each eligible value, the main thread reserves one in-flight request slot before copying and enqueueing it. The reservation is released only after the main thread installs or discards the result. This keeps queued, running, and completed-but-not-drained work within `compression-max-inflight-requests`, 1000 by default.

A separate in-flight byte cap is unnecessary because `compression-max-value-size` is a finite hard limit. It defaults to 128 KiB, cannot be disabled, and cannot be raised above 128 KiB. Therefore 1000 in-flight jobs own at most 125 MiB of input snapshots. Output allocation is also bounded: only running workers allocate a worst-case destination, and `compression-threads` is 1 by default and at most 16. A completed job keeps its input snapshot until the main thread installs or drops the result, because the freshness check in §6.3 compares against it. A worker drops a frame that is not smaller than its input, so a completed output is always smaller than the input. Input and output together are therefore at most 250 MiB.

### 6.1.1 Discussion: random sampling or a cursor scan

This point is open. v1 has both policies, so we can measure them.

**The problem with random sampling.** Each sample is a random key. When a fraction `p` of the keys is still plain, a sample finds a plain key with chance `p`. With `N` cold keys and `c` checks, about `N * exp(-c/N)` keys are still plain. To find almost all of them, the sweeper needs about `N * ln(N)` checks: about 14M checks for 1M keys. Near the end, almost every check finds a key that is already compressed. `kvstoreHashtableSampleEntries()` also returns keys from neighbouring buckets, so the samples are not fully independent. The tail can be a bit longer than the formula.

A benchmark with about 1M keys (`random`, 100 samples per call for most of the run) showed this:
- 999,534 keys compressed.
- 8,104,500 keys checked. 6,933,263 of them (86%) were already compressed. 171,703 were hot.
- The workers used only 4.6 s of CPU with 12 threads. The cost is on the main thread, in the checks.

**The cursor scan.** One pass checks each key once, so 1 million keys need about 1 million checks rather than 14 million. The tradeoffs are:
- A key that is hot during the pass waits for the next pass. With 1M keys this is about 200 s. `random` may find it sooner by chance.
- When all keys are compressed, each pass still checks every key and finds nothing. The waste is smaller than with `random`, but it does not go to 0.

**Next steps.**
- Backoff: done for `cursor` (section 6.1): pause after a pass in which less than 1% of the checked keys were eligible. This removes most of the waste when the data is stable. `random` has no backoff.
- Hot-key LRU expiry (section 7.3, Phase 4): when a key leaves the hot-key list after 60 s, it is a good candidate to compress again. The sweeper can check these keys first, in place of a full search.
- Defaults, from the benchmark below: `compression-sweeper-policy cursor`, `compression-sweeper-samples 500`, and `compression-max-inflight-requests 1000`. With 1000 jobs in flight, `cursor` compressed 99% of 1M values in 362 s; with 100, it compressed 487,583 values in 600 s. `random` needed 11,802,752 checks for 99% (2000 samples). The benchmark runs a baseline without compression, then `cursor` with changes to the backoff, the samples, `compression-threads`, `compression-max-inflight-requests` and `--io-threads`, and then `random`. Each scenario loads the data, watches the compression for 10 minutes, and runs GET over 10%, 30%, 50% and 100% of the keys. With only part of the values compressed, RSS stays high without active defrag, because freed values leave holes in pages; with active defrag, RSS follows `used_memory`.

### 6.2 The worker must not race the main thread

A worker compresses a value's bytes. The main thread may be mutating those same bytes.

**The tempting fix:** hand the worker a live pointer, and require every mutating command to copy-on-write first. That is a *convention*. It depends on every current and future code path obeying it. One missed call site is a use-after-free.

**What we do instead: an owned snapshot.** At enqueue, the main thread copies the bytes into a buffer the job owns. The worker never touches the keyspace.

Two bug classes become impossible rather than merely audited:
- **Use-after-free** — `APPEND` frees the old buffer; the worker isn't reading it.
- **Torn read** — `SETBIT` changes bytes mid-compression; the worker's copy is private.

Cost: one extra memory pass, ~0.1-2 us for typical values. That is **20-40x cheaper than the compression itself**, and paid once per attempt. If a compressed value is later read, the first read decompresses it and keeps the plain value (§7.2), so decompression is also paid only once per cold-to-hot transition.

### 6.3 Freshness: compare the bytes

A snapshot is safe, but it can go stale. The key may change while the job is queued. Installing the result would then bring old data back.

**What we do: compare the bytes at install.** The job keeps its snapshot until the main thread installs the result. At install, the main thread looks up the key again and checks:

- the key still exists,
- the value can still be compressed: a plain string (`OBJ_ENCODING_RAW`) with refcount 1, in the configured size range, and still cold (the same test as when it was picked),
- the value has the same length as the snapshot, and the same bytes (`memcmp`).

If all checks pass, the frame is correct for the value that is stored now, whatever happened while the job was queued. If one check fails, the result is dropped and counted.

This requires no command hook, version counter, or epoch. It covers command writes, expiry, eviction, `FLUSHALL`, `FLUSHDB`, `SWAPDB`, RDB load, replication, and modules because it checks the data itself rather than how the data changed. The cost is one `memcmp` on the main thread for each installation. This costs about the same as the copy at enqueue (§6.2) and never runs inside a client command.

**Snapshot and compare solve different problems.** The copy gives memory safety. The compare gives freshness. We need both.

#### 6.3.1 Why not lock the key

Valkey can already lock keys. Forkless save uses `blockClientInUseOnKeys()` in `blocked.c`: when a write command touches a key that a background thread holds, the client waits until the key is released. We could lock each key while its compression job runs. We do not, because of what it does to clients:

- **Writes get slower.** A client that writes a key with a pending job must wait for that job. The wait is not the cost of its own command. It depends on how many jobs are in the queue before it and how busy the workers are. Compression is optional background work, and it must never make a user's command slower.
- **Latency becomes hard to predict.** The sweeper picks cold keys, but a cold key can become hot at any time. The first write after it is picked would see a spike. This shows up as p99 and p99.9 latency that the user cannot explain or control.
- **One waiting key stops more than one command.** A client that sends a pipeline, a `MULTI`, or a script waits on the first locked key, and every command after it waits too. A script that does not declare its keys cannot be blocked in advance. The main thread would have to wait for the worker, and then every client waits.
- **The user pays for a feature they should not notice.** The goal is to be invisible to clients (§1). A lock turns a background memory saving into foreground latency.

With the byte compare, a write always runs at once. If it changes a key with a pending job, the only cost is that the job's result is dropped, and the key may be compressed again later.

### 6.4 What gets stored: the compression frame

The worker's output is one SDS string: a 10-byte, self-describing header followed by the compressed bytes (the body).

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 bytes | `uncompressed_len` | Size to allocate when decompressing. Little endian. |
| 4 | 4 bytes | `dict_id` | Dictionary version, `0` = no dictionary. Little endian. Never reused (§5). |
| 8 | 1 byte | `algorithm_id` | Backend needed to decode the body. |
| 9 | high 4 bits | `format_version` | Frame format, `1` today. |
| 9 | low 4 bits | `original_encoding` | Object encoding to rebuild after decompression, `OBJ_ENCODING_RAW` for strings. |

The header is small on purpose, because every compressed value pays for it. It makes a persisted frame decodable on its own. `algorithm_id` selects the backend, `format_version` selects how to read the frame, and `dict_id` selects a dictionary. `original_encoding` lets other types use the same frame later (§7.1). The multi-byte fields are little endian, because frames are saved to RDB as they are. Algorithm and format numbers can never change once written to RDB.

Fields deliberately left out:
- **No compressed length.** `sdslen(frame) - 10` is exact and free. A stored copy could disagree with sds.
- **No read counter.** Recent reads are tracked in the bounded hot-key LRU (§7.3), so the immutable frame needs no lifetime counter.
- **No dictionary payload.** Dictionaries are shared objects in the registry and are serialized once per RDB snapshot, not duplicated in each frame (§4.3).

**One allocation.** The worker allocates enough space for the header and worst-case body. The backend then compresses directly into the space after the header, with no intermediate buffer. Each header field is read and written at a named offset with `memcpy`. The code must never cast the buffer to a struct. An SDS `buf` follows a 3- or 5-byte SDS header, so these fields may not be aligned. A `static_assert` confirms that the offsets have no gaps.

`max_output_size()` returns a generous bound, so the code shrinks the frame to its real length after compression. Without this step, the allocator would still charge for unused capacity and the reported savings would exceed the actual savings.

Finally, a **net-savings guard** compares **allocation sizes**, not string lengths. It compares the memory used by the frame with the memory used by the plain value (`zmalloc_size`). The allocator rounds each request up to a size class, so a shorter string may still use the same amount of memory (Appendix A). If the frame does not save at least `COMPRESSOR_MIN_SAVINGS_PCT` percent (10; a fixed value in v1, §9), it is dropped. The worker also drops a frame that is not shorter than its input. Incompressible data therefore costs one failed attempt but adds no permanent memory overhead.

Errors are codes, not logs. A function of the block layer sets `blockCompressor.last_error` (for example `BLOCK_COMPRESSOR_ERR_DECOMPRESS`). The frame layer, which comes in a later PR, adds its own codes to the same list, for example for an unknown format version or a dictionary mismatch. The caller, which knows the key, decides what to log and count.

Frames created locally are trusted internal objects. Frames loaded from RDB are persisted input and must be validated before installation: supported algorithm, known format version, valid lengths, and a resolvable dictionary reference when `dict_id` is nonzero (§3.3). `RESTORE` payloads come from clients, and a read stops the server on a frame that does not decompress, so the load also decompresses the body once into a temporary buffer and checks its length (`compressorCreateStringObjectFromFrame()`).

---

## 7. Decompression details

### 7.1 How we know a value is compressed

Valkey uses a new object encoding, `OBJ_ENCODING_COMPRESSED` (14). The `encoding` field has 4 bits, and only values 14 and 15 were free. The same encoding applies to every type and algorithm. `robj->type` identifies the object type, while the frame header identifies the algorithm and the encoding to rebuild (§6.4). String code checks both the type and encoding through `compressorIsCompressedString()`. This single `robj` field shows whether the bytes need decompression, with no side table or separate per-value flag. `OBJECT ENCODING` reports `compressed`.

This approach requires an audit of every site that reads `obj->encoding` directly. These sites fall into four groups:

- **Client read** → synchronously decompress in v1, keep the plain value in the keyspace, release the frame, and mark the key hot (§7.2 and §7.3). During a child process, and for module read handles, use a temporary plain copy instead (§7.2).
- **Need plaintext for another egress path** → synchronously decompress into a temporary value without changing the keyspace: slot migration, replication propagation that materializes a stored value, a full sync to a replica that supports only an older RDB version, and `DEBUG DIGEST`.
- **Preserve the stored representation** → do not decompress: RDB save and `DUMP` copy the frame and persist its referenced decoder metadata and dictionary (§4.3).
- **Only inspect or release the object** → do not decompress: object free (the registry drops the dictionary's user count when applicable, and the backend's optional `release()` hook drops any per-frame library state), `MEMORY USAGE`, and active defragmentation. Today the free path (`compressorStringObjectFreed()`) reads the frame header once, for the gauges and for `releaseFrameDictionary()`. That function only checks that `dict_id` is 0, because no frame has a dictionary before Phase 10. Phase 10 cannot drop the count or call `release()` there as it is: a free can run on a background thread (`FLUSHALL ASYNC`), and each `blockCompressor` instance belongs to one thread.

`DEBUG COMPRESS-KEY <key>` compresses one value now, on the main thread, for tests. It makes the same checks as the sweeper, except the coldness test, and keeps the net-savings guard. It does not change the sweeper counters, so their sums stay right; the `compressed_*` gauges change as usual. It replies `OK`, or an error with the reason. Compressing a value (by the sweeper or by this command) and decompressing it in place send no keyspace events, because the logical value does not change.

### 7.2 The read helper

Client reads go through `lookupKey()`, which all commands use to find a key. When it finds a compressed string, it calls `compressorLookupValue()`. Commands do not change, so no command can get a frame by mistake, and new commands are covered too. An inline check of the encoding comes first, so a lookup of a plain value costs one branch.

`compressorLookupValue()` returns:

- **The value as it is**, when the caller passes `LOOKUP_NODECOMPRESS`. Commands that do not read the bytes do this, so they do not change the key: `OBJECT`, `DEBUG OBJECT`, `TYPE`, `EXISTS`, `TTL`/`PTTL`/`EXPIRETIME`/`PEXPIRETIME`, `TOUCH`, `EXPIRE`/`PEXPIRE`/`EXPIREAT`/`PEXPIREAT`, `PERSIST`, `RENAME`/`RENAMENX`, `MOVE`, `COPY` (source and destination), and `STRLEN`, which reads the plain length from the frame header (`uncompressed_len`) without decompressing. `DUMP` and `MIGRATE` do it too: the RDB code writes the frame into the payload as it is (§4.3), and `RESTORE` keeps it compressed. `COPY` copies the frame (`compressorDupFrame()`), so the new key is compressed too, and both count in `compressed_values`. Lookups that only check whether a key exists, or its type, do it too: `SET` (it replaces the value; `IFEQ`/`IFNE` compare through a temporary plain copy), `SET NX/XX`, `MSETNX`, `MSETEX NX/XX`, `setKey()`, `RESTORE` without `REPLACE`, `VM_KeyExists()`, `getNodeByQuery()`, and the blocked-client type check in `blocked.c`. So a failed `SET NX` leaves the value compressed. `MEMORY USAGE` uses `dbFind()` and does not decompress either. `LOOKUP_NOEFFECTS` does not include `LOOKUP_NODECOMPRESS`, because a module can open a key with NOEFFECTS and still read its bytes.
- **The stored value, decompressed in place**, for a write (`LOOKUP_WRITE`), or for a read when no child process runs. Only the value buffer and the encoding change. The object, its key, its TTL, and its LRU/LFU data stay the same. The key is then hot (§7.3).
- **A temporary plain copy**, for a read while a child process runs. The copy has the same key, TTL, and LRU/LFU data, because a caller may read them from the object. Callers are commands that read the bytes, for example `GET` and `GETRANGE`. The stored frame does not change, so the read causes no copy-on-write (§6.1). The copy goes on a cleanup list. `call()` notes the length of the list when a command starts, and `compressorReleasePlainCopiesSince()` releases the copies added after that when the command ends, also for a command inside a `MULTI`, a script, or a module call. `compressorAfterCall()` in `afterCommand()` releases all copies when the outermost command ends, and `compressorBeforeSleep()` also releases them, as a safety net. `PFCOUNT` looks up with `LOOKUP_NODECOMPRESS` and decompresses the stored value in place, because it writes its cached count into it.

**Modules.** All module key handles are made in `moduleInitKey()`: `RM_OpenKey`, scan callbacks, and key events. A read handle always gets its own plain copy, with or without a child, and the stored value stays compressed. So a module that scans the keyspace does not decompress all of it. The handle owns the copy (`owns_value`), and `moduleCloseKey()` frees it. A write handle decompresses the stored value in place.

**Failure.** Frames are checked when they are made and when RDB loads, so a decompression failure here means a bug or broken memory. `lookupKey()` cannot return an error (NULL means "no key"), so it stops the server with `serverPanic()`.

No side-map is needed. Commands run on the main thread, so a change in place cannot race another command. Forkless save is different: a save thread in the same process reads the objects that the main thread gives it, and each of them has a reference from the save (refcount 2 or more). So a read decompresses in place only when the refcount is 1; otherwise it gets a temporary plain copy, as during a child process. The check is per key, so other keys still decompress in place during the save. A write handle of a module (`VM_OpenKey()`) does not decompress an object that the save holds (`bgIteration_isEntryInuse()`), because `VM_OpenKey()` refuses that key after the lookup. A write to a small string that is not saved yet sends a clone to the save thread first (`tryCloneDbEntry()`); a compressed value is cloned as its plain bytes (`compressorCreatePlainClone()`). The sweeper compresses only values with refcount 1, so it never changes an object that the save holds. The save thread itself never decompresses: forkless save writes only RDB version 82 files, which keep frames as they are (§4.3), and a full sync, which can ask for an older RDB version, always uses a child process.

#### Reply ownership

The plain value belongs to the keyspace, or to the cleanup list for a temporary copy. Reply code may keep a large value by reference (`_addBulkStrRefToBufferOrList()` adds 1 to its refcount), exactly as for any other plain string. When the cleanup list releases its copy, it only drops its own reference, and the reply frees the copy after the socket write. If the same command later writes or deletes the key, the reply's reference keeps the old object alive until the write completes.

Multi-key reads decompress each compressed key on its own.

### 7.3 Hot-key LRU

Valkey keeps a bounded internal LRU set of recently read keys. Each entry identifies a database and key, records the last touch time, and participates in an MRU-to-LRU order. A client read inserts or refreshes the entry whether the current value is plain or compressed. The sweeper checks this set before copying a candidate and skips every match.

In v1 the set holds at most 100,000 entries. An entry expires after `compression-min-idle-seconds` without a read, which is 60 seconds by default, and the least recently used entry is removed when the capacity is reached. The bound prevents read tracking from growing with the keyspace. Expiry lets a key become compressible again after it cools. The existing idle/LFU test remains required, so early LRU eviction cannot by itself make an active key eligible.

The set tracks key identity, not a `robj *` or value pointer. It must not keep a deleted value alive. `DEL`, expiry, rename, database flush, and database replacement remove or update matching entries. False stale entries are safe because they only delay compression, but they must still age out normally.

`INFO compression` reports the current entry count, insertions, refreshes, capacity evictions, expirations, and sweeper skips. These counters show whether the 100,000-entry bound covers the active working set.

> **TODO — validate the hot-key LRU with measurements:** test working sets below and above 100,000 keys; measure tracking memory, lookup cost, decompressions per key that became hot, capacity churn, skipped compression work, and time for a cooled key to become compressed again.

### 7.4 A read keeps the plain value

Once a client reads a compressed value (with no child process running), the plain value replaces the frame and stays in the keyspace. Later reads avoid decompression. The key stays out of the sweeper while it is in the hot-key LRU, and it must also pass the normal coldness test after it leaves that set.

Other paths that need plain bytes do not prove that the key is hot. Slot migration, replication propagation, `DEBUG DIGEST`, and command-form AOF rewrite use a temporary plain copy, and leave the stored frame unchanged. RDB save and `DUMP` write the frame as it is (§4.3).

This policy trades memory for CPU only when a compressed key becomes active. A one-time scan by a client can decompress many values and move the keyspace back toward its uncompressed size, but the bounded LRU prevents repeated compression/decompression churn. Values that are not read again cool down and are compressed in later sweeps.

The compressed frame stays immutable and self-describing until a read or a write replaces it (§6.4).

---

## 8. The algorithm is replaceable

The compression library is never called directly. It uses the block API of the library (one whole value in, one buffer out, no stream state and no container header), next to the stream compression of `compression.h`, which uses the LZ4 frame format for the RDB and replication byte stream. The algorithm ids are `compressionAlgo` from `compression.h` (`ALGO_LZ4`, `ALGO_ZSTD`), shared with stream compression. `src/compression_block_interface.h` holds three objects:

| Object | What it is |
|---|---|
| `blockCompressorApi` | The compression function table. Every entry calls straight into a backend library. Shared, constant, never allocated. |
| `blockCompressorConfig` | The value size range the operator asked for. |
| `blockCompressor` | What callers hold. Built by `blockCompressorCreate()`, released by `blockCompressorFree()`. Carries the function table, the plain data, and the backend's private scratch. |

```c
typedef struct blockCompressorApi {
    int    (*state_new)(blockCompressor *instance);
    void   (*state_free)(blockCompressor *instance);
    int    (*train)(blockCompressor *instance, const void *samples, const size_t *sizes,
                    unsigned n, void *dict_out, size_t dict_cap);
    void  *(*dict_load)(const void *dict_buf, size_t len);
    void   (*dict_free)(void *cdict);
    size_t (*lib_max_output_size)(size_t input_len);
    size_t (*compress)(blockCompressor *instance, void *cdict, const void *src,
                       size_t srclen, void *dst, size_t dstcap);
    size_t (*decompress)(blockCompressor *instance, void *cdict, const void *body,
                         size_t body_len, void *dst, size_t dstcap);
    void   (*release)(blockCompressor *instance, void *cdict);
} blockCompressorApi;
```

Three shapes here are deliberate.

**The caller allocates the destination**, which is what makes the one-allocation build in §6.4 possible. It asks `instance->max_output_size()` how much room it needs. That function applies the operator's range first, then asks the backend, so the range check exists in one place.

**`cdict` is an argument, not state inside the instance**, because several compression dictionaries are live at once (§5) — a worker may compress with the active one while the main thread decompresses a frame built with a retiring one. A dictionary is read only once `dict_load()` returns, so every thread shares one.

**An instance belongs to one thread and is never shared**, and it never changes after it is built. That is what removes every lock and atomic from this layer. Each worker thread holds one, and the main thread holds one for decompressing on reads. To change a setting, build a new instance and drop the old one.

A loaded dictionary is passed as one `blockCompressorDict` (`id`, `algorithm_id`, `cdict`), not as separate arguments, so a caller cannot write one dictionary's ID with another dictionary's data. `blockCompressorDictIsValid()` checks one.

A failure returns 0, and the code goes into `instance->last_error`. `blockCompressorStrerror()` maps it to text. The codes are shared by all backends, because none of them is specific to one library.

The backend used for **new compression** is selected at startup from `compression-mode` (§9). Persisted frames may outlive the process that created them, so each frame records its algorithm and format version, and each loaded dictionary is associated with that algorithm (§6.4). The process must retain decoder contexts for every algorithm present in the loaded RDB even when new compression is disabled or configured to use another backend.

v1 has only the LZ4 backend (`compression-mode lz4`), and the default of `compression-mode` is `off`. zstd and dictionary training are part of v2 (§2.1), and the v1 measurements decide if they are worth the work. The reason to look at zstd: dictionary support is what makes small values compress well, and LZ4's dictionary support is weaker. The honest summary for the config docs is that `lz4` trades ratio for speed, and the gap is widest exactly where this feature aims: values of a few hundred bytes. LZ4 is already vendored in `deps/lz4`. zstd is not vendored, but Valkey can already link it as an optional system library (`BUILD_ZSTD`, default `no`), used by `src/compression_zstd.c`; a zstd backend should use that. Open question: whether a future `zstd` mode requires `BUILD_ZSTD=yes`.

---

## 9. Configuration

All names are normal `CONFIG GET` / `CONFIG SET` settings except `compression-mode`, `compression-threads`, and `compression-dict-size`. These three settings are read once at startup, like Valkey's `*_cpulist` settings, and `CONFIG SET` rejects changes to them. Background compression remains off unless `compression-mode` is set at startup. Decoder support remains available for compressed frames restored from RDB even when the mode is `off`.

**Ten primary settings**

| Name | Values | Default | What it does | Change at runtime |
|---|---|---|---|---|
| `compression-mode` | `off`, `lz4` (`zstd` when its backend exists) | `off` | The main switch. Off, or the algorithm to compress with — the background sweeper runs automatically whenever it's not `off`. | **No — startup only.** |
| `compression-threads` | `1`–`16` | `1` | Worker pool size, fixed at startup. | **No — startup only.** |
| `compression-min-value-size` | `1`-`131072` bytes | `256` | Smallest value worth compressing. | Yes |
| `compression-max-value-size` | `1`-`131072` bytes; must be at least the minimum | `131072` (128 KiB) | Largest value. Hard cap for per-value decompression and compression-job memory. | Yes |
| `compression-dict-size` | `1024`-`1048576` bytes | `102400` (100 KiB) | Target size of a trained compression dictionary. | **No — startup only.** |
| `compression-max-inflight-requests` | `1`-`10000` requests | `1000` | Cap on queued, running, and completed-but-not-drained compression jobs. Each job holds a copy of the plain value. If you lower it, jobs in flight finish, and new jobs wait until the count is below the new cap. | Yes |
| `compression-lfu-threshold` | `0`-`255` | `4` | Coldness gate in LFU mode: a value is cold when its LFU counter is at most this number. A new key starts at 5, so with 4 it is hot until its counter decays. With 5 or more, a new key is cold at once. | Yes |
| `compression-sweeper-policy` | `cursor`, `random` | `cursor` | How the sweeper picks keys: a cursor scan over all databases, or random samples (sections 6.1 and 6.1.1). | Yes |
| `compression-sweeper-samples` | `1`-`10000` keys | `500` | The most keys the sweeper checks in one cron call; with `cursor`, also the most buckets it visits. The default is not final (section 6.1). | Yes |
| `compression-sweeper-backoff-seconds` | `0`-`3600` seconds | `60` | With `cursor`, the pause after a pass in which less than 1% of the checked keys were eligible. 0 turns the backoff off. A change ends a pause at once (section 6.1). | Yes |

The upper limit of the value sizes is `COMPRESSOR_FRAME_MAX_VALUE_LEN`, so 128 KiB is written in one place only. A bigger value is an error and is never changed without a message.

**The mode names the algorithm, so there is no separate `compression-alg`.** One switch says both *whether* to compress and *how*. An operator cannot express the broken state "compression on, no backend chosen."

- **`off`** — create no new compressed frames and run no compression background work; frames restored from RDB remain readable.
- **`lz4`** — compress with the LZ4 backend, for the life of this process.
- **`zstd`** — compress with the Zstandard backend, for the life of this process.

**The mode used to create new frames is fixed at startup—there are no runtime transitions.** Changing it means restarting with a different value. Because RDB preserves compressed values, startup must also initialize read-only decoder contexts and load every referenced dictionary from the RDB before exposing the keyspace (§3.3). Existing frames keep their recorded algorithm; the configured mode controls only future background compression. Disabling compression stops new work but does not make persisted compressed values unreadable.

**Advanced settings (v2)**

None of these are configurable in v1. The mechanisms behind them run with the fixed values shown here, defined as constants in the code. Exposing them via `CONFIG GET`/`CONFIG SET` is v2 work.

| Name | Values | Default | What it does | Change at runtime |
|---|---|---|---|---|
| `compression-min-savings-ratio` | percent | `10` | Net-savings guard. Below this, the frame is thrown away. | No |
| `compression-min-idle-seconds` | seconds | `60` | Coldness gate in LRU and noeviction modes, and hot-key LRU entry lifetime in every mode. | No |
| `compression-hot-key-lru-max-entries` | entries | `100000` | Maximum recently read keys excluded from background compression (§7.3). | No |
| `compression-sweeper-backoff-min-eligible-pct` | percent | `1` | With `cursor`, a pass below this share of eligible keys starts the backoff (`COMPRESSOR_BACKOFF_MIN_ELIGIBLE_PCT`, section 6.1). | No |
| `compression-dict-min-training-keys` | int | `1000` | First training fires at this key count, and needs this many samples. | No |
| `compression-dict-max-training-keys` | int | `10000` | Sample cap for one training scan. | No |
| `compression-training-buffer-size` | bytes | `16777216` (16 MiB) | Sample buffer, allocated in full per scan and freed after it. | No |
| `compression-dict-drift-ratio` | percent | `70` | Retrain when the live ratio degrades past this. | No |
| `compression-dict-refresh-interval` | seconds, `0` = off | `0` | Periodic retrain, on top of drift. | No |
| `compression-dict-max-versions` | `2` or more | `4` | How many compression dictionaries may be live at once (§5). | No |
| `compression_cpulist` | CPU list string | `""` | Pins the worker threads. | No |

**Three controls bound background compression:**
- `compression-max-value-size` is finite and at most 128 KiB, bounding one input snapshot and one compression operation.
- `compression-max-inflight-requests` is 1000 by default, bounding job count, input snapshots to at most 125 MiB, and input plus output to at most 250 MiB. At the maximum of 10000, the bounds are 1.25 GiB and 2.5 GiB.
- `compression-threads` defaults to 1 and is at most 16, bounding concurrent compression and worst-case output allocations.

### 9.1 `INFO compression`

`INFO compression` reports the feature's behavior so operators can measure its memory savings and cost. `INFO all` combines every section, so each field name must be clear without the section title.

Types:
- **Setting:** the current value of a setting.
- **Gauge:** a value right now. It can go up and down.
- **Counter:** a total since the server started. It only goes up. `CONFIG RESETSTAT` sets it back to 0.

Names: every field starts with `compression_` or `compressed_`. Every counter starts with `compression_total_`. A name without `total` is a value right now.

All counters change only on the main thread, so they need no atomics. A worker writes its compression time into the job, and the main thread adds it up when it installs the job.

| Name | Type | Unit | Description |
|---|---|---|---|
| `compression_mode` | Setting | text | `off` or the algorithm (`lz4`). |
| `compression_threads` | Setting | threads | Number of worker threads. |
| `compressed_values` | Gauge | values | Values stored compressed now. |
| `compressed_values_original_bytes` | Gauge | bytes | Size of those values before compression. |
| `compressed_values_bytes` | Gauge | bytes | Memory those values use now (allocation size). |
| `compression_saved_bytes` | Gauge | bytes | Memory saved now: `compressed_values_original_bytes - compressed_values_bytes`. |
| `compression_jobs_in_flight` | Gauge | jobs | Values waiting for compression, being compressed, or waiting to be installed. |
| `compression_incoming_pending_keys` | Gauge | keys | Incoming keys (replica apply, slot import, §3.2) that wait for a job. |
| `compression_sweeper_paused` | Gauge | 0 or 1 | 1 while the `cursor` sweeper is in its backoff pause (section 6.1). |
| `compression_temporary_copies` | Gauge | values | Temporary plain copies alive now (§7.2). |
| `compression_total_keys_checked` | Counter | keys | Keys the sweeper looked at. |
| `compression_total_keys_eligible` | Counter | keys | Keys that passed all checks. |
| `compression_total_keys_skipped_already_compressed` | Counter | keys | Skipped: the value is already compressed. Once most data is compressed, this is the most common reason. |
| `compression_total_keys_skipped_not_string` | Counter | keys | Skipped: not a plain (RAW) string, and not compressed. |
| `compression_total_keys_skipped_in_use` | Counter | keys | Skipped: another part of the server holds the value right now (refcount above 1). |
| `compression_total_keys_skipped_size` | Counter | keys | Skipped: size outside `compression-min-value-size` to `compression-max-value-size`. |
| `compression_total_keys_skipped_hot` | Counter | keys | Skipped: clients used the key recently (not cold). |
| `compression_total_keys_skipped_already_queued` | Counter | keys | Eligible, but the key is already in the queue. |
| `compression_total_jobs_queued` | Counter | jobs | Values sent to the workers by the sweeper. |
| `compression_total_incoming_jobs_queued` | Counter | jobs | Incoming values sent to the workers (§3.2, §3.4). |
| `compression_total_values_compressed` | Counter | values | Values compressed and stored. |
| `compression_total_values_dropped` | Counter | values | Values compressed, then not stored. The total of the three counters below. |
| `compression_total_values_dropped_low_saving` | Counter | values | Dropped: the saving was under the 10% net-savings guard (§6.4, §9). |
| `compression_total_values_dropped_changed` | Counter | values | Dropped: the key changed or was deleted while it waited. |
| `compression_total_values_dropped_not_eligible` | Counter | values | Dropped: the key became hot, in use, or out of the size range while it waited. |
| `compression_total_sweeps_paused_by_child` | Counter | sweeper runs | Sweeper runs skipped because a child process (`BGSAVE`, `BGREWRITEAOF`) was running. |
| `compression_total_sweeper_passes` | Counter | passes | Full passes of the `cursor` policy over all databases. Stays 0 with `random`. With stable data, `compression_total_keys_checked` grows by about the key count per pass. |
| `compression_total_sweeper_backoffs` | Counter | pauses | Backoff pauses of the `cursor` sweeper. |
| `compression_total_compression_time_us` | Counter | microseconds | Total time of all workers spent compressing. With more than one worker, it can grow faster than real time. |
| `compression_total_values_decompressed` | Counter | values | Values that a read or a write made plain again. |
| `compression_total_temporary_copies_made` | Counter | values | Temporary plain copies made, during a save or for module reads. |
| `compression_total_decompression_time_us` | Counter | microseconds | Total main-thread time spent decompressing. This is the latency cost on reads. |

How the counters fit together:
- `compression_total_keys_checked = compression_total_keys_eligible + compression_total_keys_skipped_already_compressed + compression_total_keys_skipped_not_string + compression_total_keys_skipped_in_use + compression_total_keys_skipped_size + compression_total_keys_skipped_hot`
- `compression_total_keys_eligible = compression_total_jobs_queued + compression_total_keys_skipped_already_queued`
- `compression_total_jobs_queued + compression_total_incoming_jobs_queued = compression_total_values_compressed + compression_total_values_dropped + compression_jobs_in_flight`

Implementation notes:
- `compression` is not a default INFO section. `INFO compression`, `INFO all`, and `INFO everything` show it. A plain `INFO` does not, so its output does not change.
- The three `compressed_values*` gauges change on install, on decompress in place, and on free (`freeStringObject()` calls `compressorStringObjectFreed()`). A value can be freed on a background thread (`FLUSHALL ASYNC`), so these three use relaxed atomics. All other counters change only on the main thread. Gauges and counters are two separate structs, so `CONFIG RESETSTAT` clears only the counters and never writes the gauges: a background free cannot be lost during a reset.
- The time counters use `getMonotonicUs()`, read twice per compression or decompression.
- Any code that creates a compressed value without `compressorSetCompressedValue()` (for example RDB load, in `compressorCreateStringObjectFromFrame()`) must add it to the gauges.

Later phases add counters for the hot-key LRU (§7.3), dictionary training (§5), and the skip list for already-compressed formats (§6.1).

---

## 10. Testing

### 10.1 Idea: run the whole test suite with compression on

This is an idea, not a plan yet. The feature changes how string values are stored, so the whole test suite, not only the compression tests, should run on compressed values.

What exists today: `./runtest --config compression-mode lz4` adds the setting to every test server (`::global_overrides` in `tests/support/server.tcl`). No new code is needed for this step.

The problem: with only this setting, almost no value gets compressed.
- Most tests use `noeviction`, so a value must be idle for `COMPRESSOR_MIN_IDLE_SECONDS` (60 seconds). Most tests are shorter.
- Even a cold value waits for the next sweeper run. Tests usually read a key right after they write it.

What would make it useful: a test-only mode (for example a hidden debug setting) that compresses each eligible value right after the write. Then every read and write path in the suite sees compressed values. It can use the same code as `DEBUG COMPRESS-KEY`.

Expected cost:
- Some tests check things that compression changes: `OBJECT ENCODING`, `MEMORY USAGE`, `DEBUG OBJECT`, and memory or defrag numbers. They need a tag, so this mode can skip them, like the tags for TLS or cluster mode.
- A separate CI job for this mode.

---

## Appendix A: measured cost of compressing plain values during RDB load

The selected RDB design does not recompress plain values during load: it restores both plain and compressed values as-is (§3.3). The measurements below quantify the alternative and support that choice.

Source: a proof-of-concept load test. 5,000,000 keys, 512-byte JSON-like values, LZ4 with **no dictionary**, three runs of each load per host. The same generated data on every host, so the byte figures are identical and only the environment differs.

| | macOS arm64, libc, page cache | Linux, jemalloc, EBS gp3 | Linux, jemalloc, local NVMe |
|---|---|---|---|
| Baseline load | 7.20 s | 16.65 s | 9.42 s |
| Compress on load | 10.38 s | 20.25 s | 14.47 s |
| **Load time change** | **+44.2%** | **+21.6%** | **+53.7%** |
| `used_memory` | 3.27 → 2.48 GB, -24.2% | 3.24 → 2.45 GB, -24.4% | 3.24 → 2.45 GB, -24.4% |
| `used_memory_rss` | -25.1% | -23.8% | -23.8% |
| Value bytes | -14.3% | -14.3% | -14.3% |
| Time inside LZ4 | 3.21 s, 101% of delta | 6.07 s, 169% of delta | 4.75 s, 94% of delta |

**The cost is a range, not a single number: +22% to +54%.** The two Linux columns use the same machine, allocator, and data; only the storage differs. On EBS gp3, the baseline reads 2.1 GB at about 136 MB/s. Loading waits for disk I/O, so some compression work overlaps that wait. As a result, the 6.07 seconds spent in LZ4 exceeds the 3.60-second increase in total load time. With local NVMe, storage is no longer the limit. The 9.42-second baseline then shows the cost of parsing and building 5 million objects, and 94% of the LZ4 time appears in the total. Plan for the upper end because a well-provisioned node has fast storage and pays the full cost.

**Why the memory savings exceed the byte savings, and why this matters for the net-savings guard (§6.4).** The payload fell 16%, from 511 to 429 bytes, but the allocation fell 30%. jemalloc rounds each request up to a size class; nearby classes are 384, 448, 512, 640, and 768 bytes. An SDS string needs the payload, a 5-byte header, and a null terminator. The 511-byte payload therefore needs 517 bytes and lands in the 640-byte class, wasting 123 bytes. The 429-byte payload needs 435 bytes and lands in the 448-byte class, wasting 13 bytes. Compression recovered 82 bytes of data **and** 110 bytes of allocator rounding.

That was luck, and it does not generalise. A 300-byte payload rounds to the 320 bin; compress it by the same 16% and it still rounds to 320, so the CPU is spent and nothing is freed. **So the net-savings guard compares the allocation the value will occupy, not its byte length** (§6.4).

**Two caveats on reading the table.** These are no-dictionary numbers, so -14.3% is the pessimistic ratio; a trained dictionary reached 13.2% of original on similar data at roughly the same CPU cost per byte. And LZ4 throughput is not constant even on one machine, 422 MB/s on the EBS run against 539 MB/s on NVMe, because blocking on I/O between compressions leaves caches colder.

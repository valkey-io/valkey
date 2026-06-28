# Per-Slot Memory Tracking — High-Level Design

## Goal

Track the allocated memory consumed by each hash slot in a Valkey cluster.
The metric is a single per-slot total, summed across all keys in the slot
regardless of object type (string, list, set, zset, hash, stream, module).
The metric is exposed via `CLUSTER SLOT-STATS` as `memory-bytes` and is
always-on when cluster mode is enabled.

As part of this work we plan to provide an O(1) size function for every data
type, so the per-key size lookup — and therefore the accounting on every write
path — stays O(1) regardless of value size. Streams are the first type wired
this way (see [Stream accounting](#stream-accounting)); the remaining types
follow the same pattern.

## Definition of "memory size"

The metric reflects the *allocated* footprint of an entry (matching
`objectComputeSize()`, which backs `MEMORY USAGE`), not a logical byte count.
For each key in a slot, the contribution to the slot's `memory_bytes` counter
is computed by `slotStatsObjectSize()` as:

```
zmalloc_size(val) + <key, if not embedded> + <type-specific external allocations>
```

- `zmalloc_size(val)` is the actual allocation of the value object, including
  allocator rounding. Because keyspace objects embed the key inside the value
  object (`hasembkey`), this already accounts for the key sds and the expire
  field. The key is added explicitly only for the (rare) non-embedded case.
- The type-specific term adds allocations that live *outside* the value robj:

| Type | External allocation | Notes |
|------|-------------------|-------|
| String (EMBSTR/INT) | none | value bytes are inside the robj, already in `zmalloc_size(val)` |
| String (RAW) | `sdsAllocSize(val->ptr)` | separate sds allocation |
| Stream | `streamMemUsage(s)` | stream struct + data rax (`raxAllocSize`) + listpacks (`tracked_memory_bytes`) |
| List / Hash / Set / ZSet / Module | (not yet wired) | future: add a `case` to `slotStatsObjectSize()` |

Adding support for a new type requires:
1. A single `case` in `slotStatsObjectSize()` returning its allocated size
   (base `zmalloc_size(val)` plus the type's external allocations).
2. If the type mutates in-place (bypassing `dbSetValue`), ensuring
   `signalModifiedKey` is called after the mutation so the delta is captured.

## Scope: a slot is cluster-wide, not per-DB

The counter is stored in a single global array, `server.cluster->slot_stats[CLUSTER_SLOTS]`,
indexed only by slot number (`crc16(key) % 16384`) — there is no DB dimension.
When `cluster-databases > 1`, keys for the same slot in different DBs all
contribute to the *same* `memory_bytes` counter, so the reported value is the
sum across all DBs for that slot.

This is intentional and matches the other `CLUSTER SLOT-STATS` fields.

---

## Architecture

The accounting system has two complementary mechanisms:

### 1. DB-primitive hooks (create / overwrite / delete)

All key lifecycle events funnel through a small set of functions in `db.c`:

| Primitive | Event | Action |
|-----------|-------|--------|
| `dbAddInternal` | New key created | `+= size(key, val)`, refresh key's snapshot |
| `dbSetValue` | Key overwritten | `-= size(key, old)`, `+= size(key, new)`, refresh key's snapshot |
| `dbGenericDeleteWithDictIndex` | Key deleted | `-= size(key, val)`, drop key's snapshot entry |
| `dbAddRDBLoad` | Key loaded from RDB | `+= size(key, val)` |
| `emptyDbStructure` | FLUSHDB / FLUSHALL | Reset `memory_bytes` to 0 |

These hooks cover: SET, DEL, UNLINK, RENAME, COPY, RESTORE, MIGRATE,
eviction, expiration, replication apply, RDB load, AOF replay, and any
module API that creates/overwrites/deletes keys (`RM_StringSet`,
`RM_DeleteKey`, etc.).

### 2. Snapshot + signalModifiedKey (in-place mutations)

For commands that mutate a value without going through `dbSetValue`
(e.g. APPEND, INCR, XADD on existing stream, LPUSH, SADD, HSET):

```
lookupKeyWrite(db, key)
  → records this key's size_before in the client's per-key snapshot set

... in-place mutation (value changes without dbSetValue) ...

signalModifiedKey(c, db, key)
  → finds this key's snapshot entry, looks up current val, computes size_after
  → applies delta: slot_stats[slot].memory_bytes += (after - before)
  → removes that key's entry
```

#### Per-key snapshot set (why it isn't a single scalar)

A single command may mutate **several** keys in place — e.g. `SMOVE`,
`LMOVE`/`RPOPLPUSH` shrink one key and grow another. A single `(slot, before)`
scalar on the client cannot represent this: the second `lookupKeyWrite` would
clobber the first key's `before`, and the first `signalModifiedKey` would clear
the snapshot so the second key's delta is lost. The accounting would be
corrupted by an arbitrary amount.

So the snapshot is a **small per-key set** on the client
(`client.slot_data_bytes`, type `slotMemoryBytesSnapshot`):

```c
typedef struct slotMemoryBytesSnap {
    robj *key;        /* the modified key (incref'd while snapshotted) */
    uint64_t before;  /* size at lookup / last db-hook refresh */
} slotMemoryBytesSnap;

typedef struct slotMemoryBytesSnapshot {
    slotMemoryBytesSnap inlined[2]; /* inline fast path, no allocation */
    int count;                    /* valid inline entries; 0 once spilled */
    int slot;                     /* shared slot for the command, -1 = none */
    list *overflow;               /* NULL until >2 keys; then holds ALL entries */
} slotMemoryBytesSnapshot;
```

This is a small-vector optimization. Two entries are stored **inline**, which
covers single-key commands and the common two-key movers (`SMOVE`, `LMOVE`)
with zero allocation. A third distinct key migrates the inline entries into the
`overflow` list and everything is kept there afterwards. The list stays `NULL`
in the common case, so there is no allocation on the hot path. The set is keyed
by the key, so `signalModifiedKey` searches for the entry matching the key it
was handed (a 1–2 element scan in practice).

#### Single shared slot, no db-id

- **One slot field.** Cluster rejects cross-slot commands, so every key a
  command touches hashes to the same slot. The slot is stored once on the
  snapshot, not per entry. (`slot == -1` doubles as the "nothing captured"
  fast-path guard.)
- **No db-id.** Cluster now allows multiple databases (`cluster-databases`
  config), so the same key name can exist in several DBs. The snapshot stays
  db-id-agnostic because a built-in command never switches `c->db` mid-command
  **and** the snapshot set is cleared at every command boundary (see below), so
  no entry can ever outlive the command that created it. The only path that
  could break this is a module command that calls `RM_SelectDb` mid-execution
  and mutates same-named keys in two DBs — out of scope for the currently
  wired types.

#### Per-command clear

`afterCommand()` clears the snapshot set at the end of every `call()`. Because
`call()` wraps every command — including each `MULTI`/`EXEC` sub-command and
every scripted call — this guarantees:

- A snapshot can never leak into the next command (e.g. a command that does
  `lookupKeyWrite` but returns early without `signalModifiedKey`).
- A snapshot can never span a `SELECT`, which is what makes dropping the db-id
  safe.

`resetClient()` also clears it (belt-and-suspenders for the top-level command),
and `freeClient()` releases the overflow list.

**Interaction between the two mechanisms:**

- If the mutation goes through `dbSetValue` (e.g. `dbReplaceValue` called by
  `dbUnshareStringValue`), the db-hook does its own Sub/Add and **refreshes**
  that key's snapshot entry to the new value's size. When `signalModifiedKey`
  later fires, it sees `before == after` and applies zero delta. The refresh
  only updates an *existing* entry — it never creates one — so bulk write paths
  like `MSET` that never snapshotted do not allocate.
- If the key is deleted between lookup and signal (rare — module callbacks),
  `dbGenericDelete` **removes** that key's entry. `signalModifiedKey` finds no
  entry and skips; the delete path already did the explicit Sub.
- If the command exits early without calling `signalModifiedKey`, the leftover
  entry is dropped by the per-command clear. Nothing is updated.

### Cost when cluster mode is disabled

Zero. The `lookupKey` snapshot is gated on `server.cluster_enabled`.
The db-primitive hooks call `dataBytesAccountingEnabled(slot)` which
returns false immediately. No function calls, no branches taken.

---

## Data paths and correctness

### Normal write commands (SET, HSET, LPUSH, SADD, ZADD, XADD, etc.)

1. Command calls `lookupKeyWrite` → key's snapshot entry captured.
2. Command either:
   - Calls `setKey` / `dbAdd` / `dbSetValue` → db-hook accounts it, refreshes the key's entry.
   - Mutates value in-place → `signalModifiedKey` accounts the delta for that key.
3. `afterCommand` clears any leftover snapshot entries at the command boundary.

### Key deletion (DEL, UNLINK, expire, evict)

- `dbGenericDelete` subtracts the key's size and drops the key's snapshot entry.
- `signalModifiedKey` is not called for deletions (only `signalDeletedKeyAsReady`).
- Lazy-free / async delete: the size is subtracted **before** the object is
  queued for background freeing — the accounting uses the live object.

### RDB load

- `dbAddRDBLoad` calls `clusterSlotStatsAddMemorySdsKey` after inserting each
  key (the `SdsKey` variant, since at that point the value is already embedded
  with its key and the caller holds only an `sds` key).
- For streams, `tracked_memory_bytes` is maintained during RDB load by adding
  `zmalloc_size(lp)` at each `raxTryInsert` of a listpack.

### Replication apply (replica receiving writes from primary)

- Replicated commands execute through the normal command path.
- `lookupKeyWrite` + `signalModifiedKey` and db-hooks fire as usual.
- `server.current_client` is set to the replication client.

### FLUSHDB / FLUSHALL

- `emptyDbStructure` calls `clusterSlotStatsResetMemoryBytesAll()` which
  zeroes all `memory_bytes` counters across all slots.
- This runs regardless of sync/async flush — the counters are cleared
  immediately even if the actual memory is freed in the background.

### CONFIG RESETSTAT

- `memory_bytes` is a **state metric** (reflects current data), not a
  cumulative counter. `CONFIG RESETSTAT` preserves it.
- Only the cumulative metrics (cpu_usec, network_bytes_in/out) are reset.

### Slot ownership changes (CLUSTER ADDSLOTS / DELSLOTS)

- `clusterSlotStatReset(slot)` zeroes the entire `slotStat` struct including
  `memory_bytes`. This is correct because keys migrate with the slot — the new
  owner will rebuild the counters as keys arrive via RESTORE/MIGRATE.

### Module API (RM_StreamAppendItem, RM_StringSet, RM_HashSet, etc.)

- Module calls that create/delete keys go through `dbAdd` / `dbGenericDelete`
  → covered by db-hooks.
- Module calls that mutate existing values (RM_StreamAppendItem,
  RM_ListPush, RM_HashSet on existing key) call `signalModifiedKey` at the
  end → covered by snapshot+signal mechanism.
- No per-module-function instrumentation is needed.

### RENAME / COPY

- Source key is deleted from old slot → db-hook subtracts.
- Destination key is created in new slot → db-hook adds.
- Cross-slot RENAME is rejected in cluster mode.
- COPY within same slot: both source and destination are accounted.

### Multi-key in-place commands (SMOVE, LMOVE / RPOPLPUSH, ...)

- These shrink one key and grow another **in place** (no `dbSetValue`), within
  a single command and a single slot (cross-slot is rejected).
- Each key gets its own snapshot entry at `lookupKeyWrite`, and each
  `signalModifiedKey` consumes only that key's entry. The per-key snapshot set
  is what makes this correct — a single-scalar snapshot would miss-attribute one
  key's `before` to the other and drop the second key's delta entirely.
- Not exercised by the currently wired types (string, stream have no two-key
  in-place command); this is a prerequisite for wiring set/list/zset/hash. The
  `SMOVE`/`LMOVE` regression tests should land with those types.

### SWAPDB

- Not allowed in cluster mode (`server.cluster_enabled` check rejects it).
- No accounting concerns.

---

## Storage

```c
/* In cluster_legacy.h */
typedef struct slotStat {
    uint64_t cpu_usec;
    uint64_t network_bytes_in;
    uint64_t network_bytes_out;
    uint64_t memory_bytes;  /* per-slot allocated-byte accounting */
} slotStat;
```

Total memory-bytes overhead: `8 bytes × 16384 slots` ≈ 128 KB.

For streams, the per-slot counter draws on `stream->tracked_memory_bytes`, a
counter maintained incrementally in `t_stream.c` that holds the summed
`zmalloc_size` of all listpacks in the stream's data rax. See
[Stream accounting](#stream-accounting) below.

The in-place mutation snapshot lives on the client
(`client.slot_data_bytes`, type `slotMemoryBytesSnapshot`; see
[Snapshot + signalModifiedKey](#2-snapshot--signalmodifiedkey-in-place-mutations)).
It is two inline entries plus a lazily-allocated overflow list, so the common
case adds a fixed handful of bytes per client and **zero** per-command
allocations. The overflow list is allocated only when a single command
in-place-mutates more than two distinct keys (essentially modules only) and is
released in `freeClient`.

---

## Exposure

### CLUSTER SLOT-STATS reply

The `memory-bytes` field is a single integer: the total allocated footprint of
all keys in the slot.

```
CLUSTER SLOT-STATS SLOTSRANGE 0 0
1) 1) (integer) 0
   2) 1) "key-count"
      2) (integer) 5
      3) "memory-bytes"
      4) (integer) 3840
      5) "cpu-usec"
      ...
```

### ORDERBY memory-bytes

`CLUSTER SLOT-STATS ORDERBY memory-bytes [LIMIT n] [ASC|DESC]`

Sorts by each slot's `memory_bytes` total. It is available even when
`cluster-slot-stats-enabled` is off (and gated only on `cluster_enabled`)
because, unlike the cumulative metrics that can start counting from zero on
enable, this state metric cannot be reconstructed after the fact without an
O(N) rescan of the slot's keys, so it must be maintained continuously.

---

## Adding a new type — checklist

1. Implement a size function that returns the type's allocated footprint (or
   maintain a `tracked_memory_bytes`-style counter on the type's struct if it
   uses in-place mutations that change container sizes; see the stream example).
2. Add a `case OBJ_<TYPE>:` to `slotStatsObjectSize()` in
   `cluster_slot_stats.c`, adding the type's external allocations on top of the
   base `zmalloc_size(val)`.
3. Ensure the type's mutation commands call `signalModifiedKey` after
   modifying the value (most already do).
4. If the type is loaded from RDB via a non-standard path (not
   `dbAddRDBLoad`), ensure any incremental counter is maintained during load.
5. Add integration tests in `tests/unit/cluster/slot-stats-memory-bytes-<type>.tcl`.
   Prefer exact assertions against `MEMORY USAGE <key> SAMPLES 0` (see the
   `assert_slot_bytes_match_memory_usage` helper) over hardcoded byte counts,
   which are allocator-dependent.

---

## Stream accounting

Streams need incremental tracking because their bytes live in listpacks that
are mutated in place (XADD appends, XDEL/XTRIM mark-delete or free nodes).

- `stream->tracked_memory_bytes` holds the summed **allocated** size
  (`zmalloc_size`, not `lpBytes`) of all listpacks in the stream's data rax.
  Allocated size matters because stream nodes are over-allocated on creation
  and only shrunk to fit when sealed; using logical `lpBytes` would undercount
  the active tail node by up to its preallocation slack.
- It is maintained by three helpers in `t_stream.c`: `streamTrackLpAdd`,
  `streamTrackLpRemove` (call before `lpFree`), and `streamShrinkLpToFit`
  (shrink + account + return the reallocated listpack). All XADD/XTRIM/XDEL/dup/
  RDB-load sites go through these or bracket an in-place mutation with a
  `zmalloc_size` read before and after.
- `streamMemUsage(s)` returns the stream's footprint as
  `sizeof(*s) + raxAllocSize(s->rax) + s->tracked_memory_bytes`, all O(1).
  `slotStatsObjectSize()` adds this to `zmalloc_size(val)` for OBJ_STREAM.

**Not yet counted:** consumer group memory (`s->cgroups`: groups, consumers,
PELs, NACKs). For a stream with consumer groups, `memory-bytes` is therefore
lower than `MEMORY USAGE`, which does count it. Consumer-group tracking is
deferred to a follow-up (it needs a dedicated incremental counter, because the
per-slot metric is consulted on every in-place mutation and an O(groups +
consumers) walk on that hot path would be too costly).

No changes to `db.c`, `networking.c`, or command dispatch are needed.

---

## Module types

Modules expose only an approximate memory-usage callback today (used by
`MEMORY USAGE`), which cannot be trivially reused for exact,
incrementally-maintained per-slot accounting. Our approach does **not** rely on
that callback. Instead, modules participate through a **delta API**: the module
reports `+n`/`-n` against the per-slot counter on its own allocations and
frees — the same incremental mechanism the built-in types use, so a
participating module is accounted with the same fidelity as a native type.

Support policy, by module category:

- **Old (legacy) modules** — not supported; their keys do not contribute to
  `memory-bytes`.
- **Official modules** (Valkey Search, Valkey Bloom, Valkey JSON, ...) — we
  provide the delta API and do the wiring ourselves. These will be exact.
- **New modules** — opt-in via the delta API; using it correctly is the
  module's responsibility.

Shared/common (non-per-key) module memory (e.g. JSON's shared structures) is
excluded, since it cannot be attributed to any single slot.

A slot containing keys from a non-participating module will be under-counted.
This is made *visible* (tracked scope is observable) rather than silently
presented as authoritative for data the metric cannot see.

---

## Design decisions

Resolved during design review (2026-06-23).

- **Expiry metadata.** Included. Since the metric reflects actual allocated
  memory, expiry information should be counted as part of the key's footprint
  for as long as the key exists. The embedded expire field is already covered
  by `zmalloc_size(val)` (it lives inside the value robj); if additional
  per-key expiry structures are added in the future they should be included
  too. The memory is only released (and subtracted from the counter) when the
  key is actually evicted or expired.
- **Always-on vs. configurable.** Whether accounting is unconditional or gated
  behind a config will be decided based on measured performance. We are
  comfortable always paying the small extra memory cost for the large
  data-representation counters.

---

## Benchmark results

Measured on the same EC2 instance, 1M operations, 50 clients. Baseline is
upstream `unstable` at commit `aaa859d5` (2026-06-26); "With tracking" is the
`per-slot-memory` branch with stream `tracked_memory_bytes` accounting active
(cluster mode enabled).

### Stream operations

| Command | Baseline (ops/s) | With tracking (ops/s) | Δ% | p50 base (ms) | p50 track (ms) | p99 base (ms) | p99 track (ms) |
|---|---|---|---|---|---|---|---|
| XADD (small) | 97,456 | 96,217 | −1.3% | 0.479 | 0.487 | 0.735 | 0.756 |
| XADD (512B) | 91,583 | 93,519 | +2.1% | 0.511 | 0.500 | 0.783 | 0.791 |
| XTRIM MAXLEN | 96,805 | 98,954 | +2.2% | 0.479 | 0.471 | 0.743 | 0.743 |

Each result is the average of 3 runs (1M ops, 50 clients per run).

**Summary:** all stream operations are within noise of the baseline (±2%,
well within run-to-run variance). The per-slot accounting overhead — a single
`zmalloc_size` read before/after each listpack mutation — has no measurable
throughput or latency impact.

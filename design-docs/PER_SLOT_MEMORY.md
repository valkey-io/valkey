# Per-Slot Memory Tracking — High-Level Design

## Overview

Valkey Cluster needs a fast way to report how much allocated memory belongs to
each hash slot. This design adds a `memory-bytes` state metric to
`CLUSTER SLOT-STATS`.

The metric:

- Reports one allocated-memory total per slot.
- Includes keys from every configured database.
- Uses the same memory-size definition as `MEMORY USAGE` for supported types.
- Is maintained incrementally. Reading a slot's value costs O(number of
  databases), the same as `key-count`.
- Is available when cluster mode is enabled, even when cumulative slot metrics
  are disabled.

The current design maintains `memory-bytes` continuously when cluster mode is
enabled. The final always-on versus configurable policy is discussed in
[Design decisions](#design-decisions).

### Implementation status

| Type | Status | Size source | Known limitation |
|---|---|---|---|
| String | Implemented | Value robj allocation, plus the external sds for RAW strings | None |
| Stream | Implemented | `streamMemUsage()` and `stream->tracked_memory_bytes` | Consumer-group memory is not counted yet |
| List | Planned | O(1) allocated-size function or incremental counter | Not wired |
| Hash | Planned | O(1) allocated-size function or incremental counter | Not wired |
| Set | Planned | O(1) allocated-size function or incremental counter | Not wired |
| ZSet | Planned | O(1) allocated-size function or incremental counter | Not wired |
| Module | API planned | Module-reported allocation deltas | Legacy and non-participating modules are not counted |

Streams are the first container type with O(1) size tracking. The remaining
built-in types will follow the same pattern.

## Contents

- [Metric semantics](#metric-semantics)
- [Architecture](#architecture)
- [Lifecycle and correctness](#lifecycle-and-correctness)
- [Storage and command interface](#storage-and-command-interface)
- [Type-specific accounting](#type-specific-accounting)
- [Adding support for another type](#adding-support-for-another-type)
- [Design decisions](#design-decisions)
- [Known limitations](#known-limitations)
- [Benchmark results](#benchmark-results)

---

## Metric semantics

### What `memory-bytes` measures

`memory-bytes` represents the allocated footprint of every key/value entry in a
slot. It follows `objectComputeSize()`, which backs `MEMORY USAGE`, rather than
reporting logical content length.

For each key, `slotStatsObjectSize()` calculates:

```text
zmalloc_size(val) + <key, if not embedded> + <type-specific external allocations>
```

The terms are:

- `zmalloc_size(val)` is the actual allocation of the value object, including
  allocator rounding.
- Keyspace objects normally embed the key in the value object (`hasembkey`), so
  the value allocation already includes the key sds and expire field.
- The key is added separately only in the rare non-embedded case.
- Type-specific size functions add allocations outside the value robj, such as
  an external string sds, container nodes, listpacks, or radix trees.

This definition makes the intended relationship straightforward:

```text
slot memory-bytes = sum of MEMORY USAGE for the slot's keys
```

The equality applies to types and allocations supported by the per-slot
accounting. Current exceptions are listed in [Known limitations](#known-limitations).
Tests should compare against `MEMORY USAGE <key> SAMPLES 0` rather than use
allocator-dependent constants.

### Slot scope across databases

The reported value is cluster-wide for the slot: when `cluster-databases > 1`,
same-slot keys in all databases add to the same reported `memory-bytes`. This
matches `key-count`.

The counter itself is kept per (database, slot), next to the data. Each
database has one hashtable per slot in `db->keys`, and the counter lives in the
metadata of that hashtable. `CLUSTER SLOT-STATS` sums the counters over
`server.db`, the same way `countKeysInSlot()` sums key counts. See
[Per-slot storage](#per-slot-storage).

Because the counter lives with the data, it follows the data:

- `FLUSHDB` on one database removes only that database's bytes.
- An async flush swaps the database's kvstore out, so its counters leave with it.
- A temp database used by diskless load (`repl-diskless-load swapdb`) has its
  own kvstore. Its counters become visible only if it is swapped in, and are
  dropped with it otherwise.
- A slot ownership change (failover, `CLUSTER SETSLOT ... NODE`, slot takeover)
  does not touch the counter, because the keys stay in place.

### State metric versus cumulative metrics

`memory-bytes` describes current state. It differs from cumulative fields such
as `cpu-usec` and `network-bytes-in/out`:

- `CONFIG RESETSTAT` preserves `memory-bytes`. The counter is not stored in
  `slot_stats`, so resetting the cumulative fields cannot touch it.
- `FLUSHDB` and `FLUSHALL` reduce or clear it as data is removed.
- It must be maintained continuously. If enabled only after data already
  exists, rebuilding it would require an O(N) scan of keys.

---

## Architecture

The accounting uses two complementary paths:

1. **Database primitive hooks** handle key creation, replacement, deletion,
   loading, and expire changes. Database flushes need no hook, because the
   counter is dropped together with the slot hashtable.
2. **Per-key snapshots plus `signalModifiedKey`** handle values that mutate in
   place without passing through `dbSetValue`.

```text
Create / replace / delete                    In-place mutation
-------------------------                    -----------------
dbAdd / dbSetValue / dbDelete               lookupKeyWrite(key)
        |                                             |
        v                                             v
add/subtract full entry size                 capture size_before
                                                      |
                                             mutate existing value
                                                      |
                                                      v
                                             signalModifiedKey(key)
                                                      |
                                             apply size_after - size_before
```

### Database primitive hooks

Key lifecycle events pass through a small set of functions in `db.c`:

| Primitive | Event | Accounting action |
|---|---|---|
| `dbAddInternal` | New key | Add the entry size; refresh that key's snapshot if present |
| `dbSetValue` | Replace value | Subtract the old size, add the new size, then refresh the snapshot |
| `dbGenericDeleteWithDictIndex` | Delete key | Subtract the entry size and remove that key's snapshot |
| `dbAddRDBLoad` | Load key from RDB | Add the entry size |
| `setExpire` | Add or change an expire | Apply the size change, because adding the embedded expire field can reallocate the value; refresh the snapshot |
| `kvstoreEmpty` | `FLUSHDB` / `FLUSHALL` | No accounting hook; the counter in each slot hashtable is cleared with the hashtable |

All hooks take the `serverDb *` they work on, and add or subtract against that
database's slot hashtable.

These hooks cover normal commands and internal paths including `SET`, `DEL`,
`UNLINK`, `RENAME`, `COPY`, `RESTORE`, `MIGRATE`, eviction, expiration,
replication apply, RDB load, AOF replay, and module APIs that create, replace,
or delete keys (`RM_StringSet`, `RM_DeleteKey`, and similar APIs).

### In-place mutation snapshots

Commands such as `APPEND`, `INCR`, `XADD`, `LPUSH`, `SADD`, and `HSET` may
change an existing value without calling `dbSetValue`. Their accounting flow is:

```text
lookupKeyWrite(db, key)
  -> record this key's size_before in the snapshot of server.executing_client

mutate the existing value

signalModifiedKey(c, db, key)
  -> find this key's snapshot on server.executing_client
  -> read the current value (LOOKUP_NOEFFECTS) and compute size_after
  -> apply the counter of (db, slot) += size_after - size_before
  -> remove this key's snapshot
```

The snapshot always lives on `server.executing_client`, the client that
`call()` is running. This matters for scripts and `VM_Call`: `EVAL`, `FCALL`
and module calls run each command on a temporary client, while
`server.current_client` stays the client that sent `EVAL`. Using
`server.executing_client` for the snapshot, the refresh, the forget, and the
commit makes all four use the same client.

The re-read at commit time uses `LOOKUP_NOEFFECTS`, so it has no side effects.
In particular it is not counted again by hot-key detection.

#### Why snapshots are per key

Some commands mutate several keys in place. For example, `SMOVE` and
`LMOVE`/`RPOPLPUSH` shrink one value and grow another.

A single `(slot, before)` value on the client is not enough:

1. The second `lookupKeyWrite` would replace the first key's `before` value.
2. The first `signalModifiedKey` would consume the shared snapshot.
3. The second key's delta would be lost.

The client therefore owns a small per-key snapshot set in
`client.slot_memory_bytes`:

```c
typedef struct slotMemoryBytesSnap {
    robj *key;        /* incref'd while snapshotted */
    uint64_t before;  /* size at lookup or last DB-hook refresh */
    int slot;         /* slot of key */
    int dbid;         /* database of key */
} slotMemoryBytesSnap;

typedef struct slotMemoryBytesSnapshot {
    slotMemoryBytesSnap inlined[2]; /* allocation-free common path */
    int inlined_count;              /* valid inline entries; 0 after spill */
    int slot;                       /* -1 means empty (fast-path guard) */
    list *overflow;                 /* all entries after more than two keys */
} slotMemoryBytesSnapshot;
```

The first two entries stay inline. This covers single-key commands and common
two-key movers without allocation. On the third distinct key, the two inline
entries move to `overflow`, and the list stores all entries from that point.

`signalModifiedKey` searches for the entry matching its key. In normal command
paths this is a scan of one or two entries.

#### Why each entry stores its slot and database

Cluster rejects cross-slot commands, so all keys used by a built-in command
share one slot. Modules do not have this check: `RM_OpenKey` can open local
keys in different slots in one command, and a keyspace-notification callback
can open keys while another command runs. So each entry stores the slot of its
key, and the commit uses that slot. The `slot` field of the set is only the
empty-snapshot fast-path check (`slot == -1`).

Each entry also stores the database ID of its key, and entries are matched on
(database, key name). One command can use the same key name in two databases:
`MOVE` and `COPY ... DB` look up the key in the source and in the destination
database, and a module can call `RM_SelectDb` while its command runs. Without
the database ID, `MOVE` would share one entry between the two databases: the
destination lookup would overwrite the source `before`, and the settle step in
the source delete would count the key again in the source slot.

#### Snapshot lifecycle

- `afterCommand()` clears snapshots at the end of every `call()`, including
  each `MULTI`/`EXEC` sub-command and each scripted call.
- `resetClient()` also clears snapshots for top-level client cleanup.
- `freeClient()` releases the overflow list.

The per-command clear handles early returns after `lookupKeyWrite` and prevents
stale snapshots from leaking into later commands or across `SELECT`.

#### Interaction with DB hooks

The snapshot and DB-hook paths may both run for one command. The following
rules prevent double counting:

- Before `dbSetValue`, `setExpire`, or the delete path subtract the key's
  current size, they **settle** the key's snapshot
  (`clusterSlotStatsSettleKey`). Settle applies any in-place change that is
  still pending (current size minus `before`) and moves `before` to the
  current size. Without this step, an in-place change made earlier in the same
  command would be lost. Example: a module calls `RM_StringTruncate` and then
  `RM_SetExpire` or `RM_StringSet` on the same open key.
- After `dbSetValue` or `setExpire` performs its subtract/add, it refreshes the
  existing snapshot to the new value's size. A later `signalModifiedKey` sees
  equal before/after values and applies no delta.
- Refresh updates only an existing snapshot. It does not create one, so bulk
  write paths such as `MSET` do not allocate snapshot entries unnecessarily.
- If the key is deleted between lookup and signal, `dbGenericDelete` settles,
  performs the subtraction, and removes that key's snapshot. A later signal
  finds no entry and does nothing.
- If a command exits without signaling, `afterCommand()` drops the leftover
  entry without changing the metric.

### Cost outside cluster mode

Snapshot capture is gated on `server.cluster_enabled`. The DB primitive hooks
are still called, but each one returns after a single `server.cluster_enabled`
check. Non-cluster deployments do not maintain the per-slot metric.

---

## Lifecycle and correctness

### Operation matrix

| Path | How the metric stays correct |
|---|---|
| Normal write | `lookupKeyWrite` captures the old size; a DB hook accounts replacement or `signalModifiedKey` applies an in-place delta |
| Multi-key in-place write | Every key has its own snapshot; each signal consumes only the matching entry |
| Delete / unlink | Size is subtracted before the object is freed; the matching snapshot is removed |
| Lazy or asynchronous delete | Accounting uses the live object before it is queued for background freeing |
| Expiration / eviction | Both use the normal delete primitive and subtract the entry size |
| RDB load | `dbAddRDBLoad` adds every loaded key; type-specific counters are maintained while decoding |
| AOF replay | Commands execute through the normal command paths |
| Replication apply | Replicated commands use normal command paths with the replication client as `server.executing_client` |
| Replica full sync | The replica loads the RDB through `dbAddRDBLoad` and `setExpire`, so it reports the same value as the primary |
| Diskless load with `swapdb` | Keys load into a temp database with its own kvstore. The old dataset's counters are dropped with the old kvstore; a failed load drops the temp counters |
| Scripts and `VM_Call` | Snapshot and commit both use `server.executing_client`, the temporary client that runs the command |
| Setting an expire | `setExpire` applies the size change when the expire field reallocates the value (`SET EX`, `SETEX`, `GETEX`, `EXPIRE`, RDB load) |
| `FLUSHDB` / `FLUSHALL` | Only the flushed databases lose their counters, including for asynchronous flush |
| `CONFIG RESETSTAT` | `memory-bytes` is preserved because it represents current state |
| Slot ownership change | `clusterSlotStatReset(slot)` clears only the cumulative fields. Failover, `CLUSTER SETSLOT ... NODE`, and slot takeover keep the keys, so the counter stays. Keys dropped later (`delKeysInSlot`) are subtracted by the delete path |
| `RENAME` | Source deletion subtracts and destination creation adds; cross-slot rename is rejected |
| `COPY` | Destination creation/replacement is accounted; same-slot source and destination remain independent entries |
| `SWAPDB` | Rejected in cluster mode, so it needs no accounting path |

### RDB load details

`dbAddRDBLoad` calls `clusterSlotStatsAddMemorySdsKey` after inserting each key.
It uses the `SdsKey` variant because the value already embeds its key and the
caller holds an `sds` key.

For streams, RDB loading updates `tracked_memory_bytes` by adding
`zmalloc_size(lp)` whenever a listpack is inserted into the stream radix tree.

### Module command paths

Module APIs that create or delete keys pass through `dbAdd` and
`dbGenericDelete`, so DB hooks account them. APIs that mutate existing values,
such as `RM_StreamAppendItem`, `RM_ListPush`, and `RM_HashSet`, call
`signalModifiedKey` and use the snapshot path.

Module-owned allocations require the separate participation policy described
in [Module types](#module-types).

---

## Storage and command interface

### Per-slot storage

The counter is a field in the kvstore hashtable metadata (`src/kvstore.c`):

```c
typedef struct {
    listNode *rehashing_node;
    kvstore *kvs;
    uint64_t tracked_bytes; /* Caller-maintained byte count for the entries in this hashtable. */
} kvstoreHashtableMetadata;
```

kvstore exposes it through `kvstoreHashtableAddTrackedBytes()`,
`kvstoreHashtableSubTrackedBytes()`, and `kvstoreHashtableTrackedBytes()`. The
kvstore does not know what the bytes mean; `cluster_slot_stats.c` is the only
user, and only for `db->keys`. `kvstoreEmpty()` sets the field to 0, and the
field goes away when an empty hashtable is freed.

`cluster_slot_stats.c` reads the slot value with `slotMemoryBytes(slot)`, which
sums `kvstoreHashtableTrackedBytes(server.db[i]->keys, slot)` over all
databases.

The cost is:

- 8 bytes per allocated hashtable. The metadata type is shared by all
  kvstores, so this applies to `keys`, `expires`, `keys_with_volatile_items`,
  and the pubsub kvstores, not only to `keys`.
- O(number of databases) per slot when the stat is read. `ORDERBY` reads every
  slot owned by the shard before sorting.

The client snapshot adds two inline entries and a list pointer to each client.
It performs no per-command allocation for up to two keys. The overflow list is
allocated only if one command snapshots more than two distinct keys, which is
expected mainly from module commands, and its header is retained for reuse
until `freeClient()`.

### `CLUSTER SLOT-STATS` reply

`memory-bytes` is one integer containing the allocated footprint of all
supported keys in the slot:

```text
CLUSTER SLOT-STATS SLOTSRANGE 0 0
1) 1) (integer) 0
   2) 1) "key-count"
      2) (integer) 5
      3) "memory-bytes"
      4) (integer) 3840
      5) "cpu-usec"
      ...
```

The reply schema in `src/commands/cluster-slot-stats.json` lists
`memory-bytes`, so `--log-req-res` reply validation accepts it.

### Ordering slots by memory

```text
CLUSTER SLOT-STATS ORDERBY memory-bytes [LIMIT n] [ASC|DESC]
```

The command sorts by each slot's `memory-bytes` total (summed over databases).
It remains available
when `cluster-slot-stats-enabled` is off because the metric is state that cannot
be reconstructed later without scanning all keys.

---

## Type-specific accounting

### Strings

`slotStatsObjectSize()` starts with `zmalloc_size(val)`. String encodings add:

| Encoding | Additional allocation |
|---|---|
| EMBSTR | None; content is embedded in the value allocation |
| INT | None; the integer is stored in the value object |
| RAW | `sdsAllocSize(val->ptr)` for the external sds |

### Streams

Streams require incremental accounting because their listpacks are changed in
place by `XADD`, `XDEL`, and `XTRIM`.

#### Tracked state

`stream->tracked_memory_bytes` stores the total **allocated** size of every
listpack in the stream's data radix tree. It uses `zmalloc_size`, not
`lpBytes`, because active stream nodes are over-allocated and shrink only when
sealed. Logical listpack length would miss that unused capacity.

The counter is maintained by three helpers in `t_stream.c`:

- `streamTrackLpAdd`
- `streamTrackLpRemove` (called before `lpFree`)
- `streamShrinkLpToFit` (shrinks, updates accounting, and returns the possibly
  reallocated listpack)

XADD, XTRIM, XDEL, duplication, and RDB-load paths use these helpers or bracket
an in-place listpack mutation with `zmalloc_size` reads before and after.

#### O(1) stream size

```text
streamMemUsage(s) = sizeof(*s)
                  + raxAllocSize(s->rax)
                  + s->tracked_memory_bytes
```

`slotStatsObjectSize()` adds this result to `zmalloc_size(val)` for
`OBJ_STREAM`. The operation remains O(1), regardless of stream length.

#### Stream limitation

Consumer-group memory is not yet counted. This includes groups, consumers,
PELs, and NACKs under `s->cgroups`. `MEMORY USAGE` includes these allocations,
so streams with consumer groups currently report a lower per-slot value.

Adding this memory requires another incremental counter. Walking all groups and
consumers on every in-place mutation would make the hot path too expensive.

### Module types

The current module memory callback, used by `MEMORY USAGE`, is approximate and
cannot provide exact incremental per-slot accounting. The design therefore
uses a **delta API** instead: a module reports `+n` and `-n` as it allocates and
frees memory attributable to a key.

Support policy:

| Module category | Policy |
|---|---|
| Legacy modules | Unsupported; their keys do not contribute to `memory-bytes` |
| Official modules (Valkey Search, Bloom, JSON, and others) | Valkey provides the delta API and wires these modules to it |
| New modules | Opt in to the delta API; correct reporting is the module's responsibility |

Shared module memory that cannot be assigned to one key, such as common JSON
structures, is excluded. Slots containing non-participating module keys are
therefore under-counted. This limitation must remain visible rather than
presenting the value as complete for data the server cannot inspect.

---

## Adding support for another type

Use this checklist for lists, hashes, sets, sorted sets, and future types:

1. Implement an O(1) function that returns the type's allocated footprint.
   If container allocations change in place, maintain an incremental counter
   on the type structure, following the stream design.
2. Add `case OBJ_<TYPE>:` to `slotStatsObjectSize()` in
   `cluster_slot_stats.c`. Add external allocations to the base
   `zmalloc_size(val)`.
3. Confirm that every in-place mutation calls `signalModifiedKey` after the
   value changes. Most existing commands already do this.
4. Confirm that RDB loading and duplication maintain any new incremental
   counter. Add explicit handling for non-standard load paths that bypass
   `dbAddRDBLoad`.
5. Test single-key creation, replacement, in-place growth/shrink, deletion,
   expiration, RDB load, AOF replay, replication, and flush behavior.
6. For types with multi-key movers, add regression coverage for both keys. Use
   `SMOVE` for sets and `LMOVE`/`RPOPLPUSH` for lists.
7. Add the integration tests under:

   ```text
   tests/unit/cluster/slot-stats-memory-bytes-<type>.tcl
   ```

   Type-independent cases (replica sync, swapdb load, failover, several
   databases) live in `slot-stats-memory-bytes-lifecycle.tcl`. Add a key of the
   new type to `write_memory_bytes_dataset` there. Module key API cases (an
   in-place change followed by a second change on the same open key, and keys
   in two slots in one command) live in `tests/unit/moduleapi/cluster.tcl`,
   with commands in `tests/modules/cluster.c`.

8. Prefer exact comparisons with `MEMORY USAGE <key> SAMPLES 0` through
   `assert_slot_bytes_match_memory_usage`. Do not hardcode allocator-dependent
   byte values.

The common accounting design should not require further changes to `db.c`,
`networking.c`, or command dispatch when adding a built-in type.

---

## Design decisions

### Resolved: include expiry metadata

Resolved during design review on 2026-06-23.

The metric includes expiry information while the key exists because it reports
allocated memory. The embedded expire field is already part of
`zmalloc_size(val)`. Any future per-key expiry allocations should also be
included. The bytes are removed only when the key is deleted, evicted, or
expired.

### Open: always-on versus configurable

The current implementation and command semantics assume continuous accounting
whenever cluster mode is enabled. This avoids an O(N) rebuild when a state
metric is enabled after keys already exist.

The final policy depends on measured write-path cost. The current stream
benchmark found no measurable impact beyond normal run-to-run variation and
therefore supports keeping the metric always on. A configuration gate would
need defined initialization semantics, such as scanning existing data before
reporting a complete value.

---

## Known limitations

- Only strings and streams are currently wired. Other built-in types do not yet
  contribute to `memory-bytes`.
- Stream consumer-group allocations are not counted, so streams with groups do
  not yet match `MEMORY USAGE`.
- Legacy and non-participating module types are not counted.
- Shared module memory that cannot be assigned to a key is excluded.
- Nested clients do not share a snapshot. If a module opens a key for write,
  changes it in place, and then calls `VM_Call` on the same key, the inner
  command runs on a temporary client and does not see the outer client's
  snapshot. The counter for that key can drift.

---

## Benchmark results

### Test setup

- Same EC2 instance for baseline and tracking runs.
- 1 million operations per test.
- 50 clients.
- Each result averaged over three runs.
- Baseline: the same `per-slot-memory` branch without stream
  `tracked_memory_bytes` changes.
- With tracking: `zmalloc_size`-based listpack tracking plus
  `streamMemUsage()`.

### Result summary

Stream operations remained within approximately 1–2% of baseline, which is
within observed run-to-run variation. The accounting adds one
`zmalloc_size` read before and after each listpack mutation and showed no
measurable throughput or latency cost.

### Stream operation results

| Command | Baseline (ops/s) | With tracking (ops/s) | Delta | p50 base (ms) | p50 track (ms) | p99 base (ms) | p99 track (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|
| XADD (3B) | 95,782 | 94,838 | -1.0% | 0.487 | 0.492 | 0.753 | 0.769 |
| XADD (512B) | 94,146 | 92,000 | -2.3% | 0.497 | 0.508 | 0.769 | 0.791 |
| XTRIM MAXLEN | 98,408 | 97,245 | -1.2% | 0.471 | 0.476 | 0.724 | 0.748 |
| XDEL | 95,314 | 95,419 | +0.1% | 0.492 | 0.492 | 0.772 | 0.767 |

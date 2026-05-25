# Per-Slot Memory Tracking — High-Level Design

## Goal

Track the logical data size consumed by each hash slot in a Valkey cluster,
broken down by object type (string, list, set, zset, hash, stream, module).
The metric is exposed via `CLUSTER SLOT-STATS` and is always-on when cluster
mode is enabled.

## Definition of "logical size"

For each key in a slot, the contribution to the slot's `data_bytes[type]`
counter is:

```
sdslen(key) + type_specific_value_size(val)
```

Where `type_specific_value_size` depends on the object type:

| Type | Value size function | Notes |
|------|-------------------|-------|
| String | `stringObjectLen(val)` | Digit count for INT-encoded, sdslen for RAW/EMBSTR |
| Stream | `stream->tracked_data_bytes` | Σ lpBytes of all listpacks in the stream's rax |
| List | `quicklist->tracked_data_bytes` | Σ node entry bytes (raw or compressed) |
| Hash (listpack) | `lpBytes(val->ptr)` | Single listpack encoding |
| Hash (hashtable) | Σ (field_len + value_len) per entry | Requires incremental tracking |
| Set (listpack) | `lpBytes(val->ptr)` | Single listpack encoding |
| Set (hashtable) | Σ member_len per entry | Requires incremental tracking |
| ZSet (listpack) | `lpBytes(val->ptr)` | Single listpack encoding |
| ZSet (skiplist) | Σ (member_len + 8) per entry | 8 bytes for the score double |
| Module | Module-reported size | Via module type API callback |

Adding support for a new type requires:
1. A single `case` in `slotStatsObjectSize()` returning the value size.
2. If the type mutates in-place (bypassing `dbSetValue`), ensuring
   `signalModifiedKey` is called after the mutation so the delta is captured.

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
| `emptyDbStructure` | FLUSHDB / FLUSHALL | Reset all `data_bytes` to 0 |

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
  → applies delta: slot_stats[slot].data_bytes[type] += (after - before)
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
(`client.slot_data_bytes`, type `slotDataBytesSnapshot`):

```c
typedef struct slotDataBytesSnap {
    robj *key;        /* the modified key (incref'd while snapshotted) */
    uint64_t before;  /* size at lookup / last db-hook refresh */
} slotDataBytesSnap;

typedef struct slotDataBytesSnapshot {
    slotDataBytesSnap inlined[2]; /* inline fast path, no allocation */
    int count;                    /* valid inline entries; 0 once spilled */
    int slot;                     /* shared slot for the command, -1 = none */
    list *overflow;               /* NULL until >2 keys; then holds ALL entries */
} slotDataBytesSnapshot;
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

- `dbAddRDBLoad` calls `clusterSlotStatsAddObject` after inserting each key.
- For streams, `tracked_data_bytes` is maintained during RDB load by
  incrementing at each `raxTryInsert` of a listpack.
- For quicklists (lists), `tracked_data_bytes` is maintained by the
  quicklist's own `quicklistAppendListpack` / `quicklistAppendPlainNode`
  functions called during RDB load.

### Replication apply (replica receiving writes from primary)

- Replicated commands execute through the normal command path.
- `lookupKeyWrite` + `signalModifiedKey` and db-hooks fire as usual.
- `server.current_client` is set to the replication client.

### FLUSHDB / FLUSHALL

- `emptyDbStructure` calls `clusterSlotStatsResetDataBytesAll()` which
  zeroes all `data_bytes` counters across all slots.
- This runs regardless of sync/async flush — the counters are cleared
  immediately even if the actual memory is freed in the background.

### CONFIG RESETSTAT

- `data_bytes` is a **state metric** (reflects current data), not a
  cumulative counter. `CONFIG RESETSTAT` preserves it.
- Only the cumulative metrics (cpu_usec, network_bytes_in/out) are reset.

### Slot ownership changes (CLUSTER ADDSLOTS / DELSLOTS)

- `clusterSlotStatReset(slot)` zeroes the entire `slotStat` struct including
  `data_bytes`. This is correct because keys migrate with the slot — the new
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
    uint64_t data_bytes[OBJ_TYPE_MAX];  /* per-type byte accounting */
} slotStat;
```

Total slot-stats memory overhead: `7 types × 8 bytes × 16384 slots` ≈ 900 KB.

The in-place mutation snapshot lives on the client
(`client.slot_data_bytes`, type `slotDataBytesSnapshot`; see
[Snapshot + signalModifiedKey](#2-snapshot--signalmodifiedkey-in-place-mutations)).
It is two inline entries plus a lazily-allocated overflow list, so the common
case adds a fixed handful of bytes per client and **zero** per-command
allocations. The overflow list is allocated only when a single command
in-place-mutates more than two distinct keys (essentially modules only) and is
released in `freeClient`.

---

## Exposure

### CLUSTER SLOT-STATS reply

The `data-bytes` field is a nested map of type-name → bytes:

```
CLUSTER SLOT-STATS SLOTSRANGE 0 0
1) 1) (integer) 0
   2) 1) "key-count"
      2) (integer) 5
      3) "data-bytes"
      4) 1) "string"  2) (integer) 1024
         3) "list"    4) (integer) 512
         5) "set"     6) (integer) 0
         7) "zset"    8) (integer) 0
         9) "hash"    10) (integer) 256
         11) "module" 12) (integer) 0
         13) "stream" 14) (integer) 2048
      5) "cpu-usec"
      ...
```

### ORDERBY data-bytes

`CLUSTER SLOT-STATS ORDERBY data-bytes [LIMIT n] [ASC|DESC]`

Sorts by the sum of all `data_bytes[type]` for each slot. Available without
`cluster-slot-stats-enabled` (since data-bytes is a state metric, not a
cumulative counter that requires additional overhead).

---

## Adding a new type — checklist

1. Implement a value-size function (or maintain a `tracked_data_bytes` field
   on the type's struct if it uses in-place mutations that change container
   sizes).
2. Add a `case OBJ_<TYPE>:` to `slotStatsObjectSize()` in
   `cluster_slot_stats.c`.
3. Ensure the type's mutation commands call `signalModifiedKey` after
   modifying the value (most already do).
4. If the type is loaded from RDB via a non-standard path (not
   `dbAddRDBLoad`), ensure `tracked_data_bytes` is maintained during load.
5. Add integration tests in `tests/unit/cluster/slot-stats-data-bytes-<type>.tcl`.

No changes to `db.c`, `networking.c`, or command dispatch are needed.

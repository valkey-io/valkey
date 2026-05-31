# Per-Slot Memory Tracking — High-Level Design

## Goal

Track the logical data size consumed by each hash slot in a Valkey cluster.

**Phase 1** tracks a single aggregate `data_bytes` counter per slot (the total
logical size of all keys in the slot). A **per-type breakdown** (string, list,
set, zset, hash, stream, module) is an **optional** layer that can be added
later without changing the sizing machinery — see
[Per-type breakdown (optional)](#per-type-breakdown-optional). The comment that
prompted this: a single total is sufficient for the initial use cases, so the
per-type split is deferred to keep phase 1 small.

The metric is exposed via `CLUSTER SLOT-STATS` and is always-on when cluster
mode is enabled.

## Definition of "logical size"

For each key in a slot, its contribution to the slot's `data_bytes` counter is:

```
sdslen(key) + type_specific_value_size(val)
```

The object type only selects which value-size function to use (below); the
result is added to a single per-slot total. (If the optional per-type breakdown
is enabled, the same value is added to `data_bytes_by_type[type]` as well.)

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
| `dbAddInternal` | New key created | `+= size(key, val)`, refresh snapshot |
| `dbSetValue` | Key overwritten | `-= size(key, old)`, `+= size(key, new)`, refresh snapshot |
| `dbGenericDeleteWithDictIndex` | Key deleted | `-= size(key, val)`, invalidate snapshot |
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
  → captures (slot, size_before) on client struct

... in-place mutation (value changes without dbSetValue) ...

signalModifiedKey(c, db, key)
  → looks up current val, computes size_after
  → applies delta: slot_stats[slot].data_bytes += (after - before)
  → clears snapshot
```

**Interaction between the two mechanisms:**

- If the mutation goes through `dbSetValue` (e.g. `dbReplaceValue` called by
  `dbUnshareStringValue`), the db-hook does its own Sub/Add and **refreshes**
  the snapshot to the new value's size. When `signalModifiedKey` later fires,
  it sees `before == after` and applies zero delta.
- If the key is deleted between lookup and signal (rare — module callbacks),
  `dbGenericDelete` **invalidates** the snapshot. `signalModifiedKey` sees
  `slot == -1` and skips.
- If the command exits early without calling `signalModifiedKey`,
  `resetClient()` clears the snapshot. Nothing is updated.

### Cost when cluster mode is disabled

Zero. The `lookupKey` snapshot is gated on `server.cluster_enabled`.
The db-primitive hooks call `dataBytesAccountingEnabled(slot)` which
returns false immediately. No function calls, no branches taken.

---

## Data paths and correctness

### Normal write commands (SET, HSET, LPUSH, SADD, ZADD, XADD, etc.)

1. Command calls `lookupKeyWrite` → snapshot captured.
2. Command either:
   - Calls `setKey` / `dbAdd` / `dbSetValue` → db-hook accounts it, refreshes snapshot.
   - Mutates value in-place → `signalModifiedKey` accounts the delta.
3. `resetClient` clears any stale snapshot.

### Key deletion (DEL, UNLINK, expire, evict)

- `dbGenericDelete` subtracts the key's size and invalidates the snapshot.
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
    uint64_t data_bytes; /* aggregate logical size of all keys in the slot */
    /* Optional per-type breakdown — not built in phase 1:
     *   uint64_t data_bytes_by_type[OBJ_TYPE_MAX];
     */
} slotStat;
```

Total memory overhead (phase 1): `8 bytes × 16384 slots` = 128 KB.
The optional per-type breakdown would add `7 types × 8 bytes × 16384 slots`
≈ 900 KB on top.

---

## Exposure

### CLUSTER SLOT-STATS reply (phase 1)

`data-bytes` is a single integer: the aggregate logical size of the slot.

```
CLUSTER SLOT-STATS SLOTSRANGE 0 0
1) 1) (integer) 0
   2) 1) "key-count"
      2) (integer) 5
      3) "data-bytes"
      4) (integer) 3840
      5) "cpu-usec"
      ...
```

### ORDERBY data-bytes

`CLUSTER SLOT-STATS ORDERBY data-bytes [LIMIT n] [ASC|DESC]`

Sorts by each slot's `data_bytes` total. Available without
`cluster-slot-stats-enabled` (since data-bytes is a state metric, not a
cumulative counter that requires additional overhead).

### Per-type breakdown (optional)

A per-type split is **not part of phase 1**. When/if it is added, `data-bytes`
becomes a nested map of type-name → bytes, and the aggregate above is the sum
of its entries:

```
      3) "data-bytes"
      4) 1) "string"  2) (integer) 1024
         3) "list"    4) (integer) 512
         5) "set"     6) (integer) 0
         7) "zset"    8) (integer) 0
         9) "hash"    10) (integer) 256
         11) "module" 12) (integer) 0
         13) "stream" 14) (integer) 2048
```

Enabling it requires only: adding `data_bytes_by_type[OBJ_TYPE_MAX]` to
`slotStat`, having the accounting paths also bucket by `val->type`, and
emitting the map instead of the scalar. The per-type O(1) sizing work
(below) is unchanged — it already computes the value used by both shapes.

---

## Why this ships as multiple PRs

A single PR for this feature would touch `db.c`, `networking.c`, command
dispatch, the slot-stats exposure, and every data structure's `.c` file at
once. Splitting it into a scaffolding PR, one PR per data type, and a final
wiring PR is a deliberate choice:

- **Reviewability.** Each per-type PR is confined to one structure's `.c` plus
  one `case` in `slotStatsObjectSize()` and a unit test. A reviewer can verify
  the counter math for that one type without holding the whole feature in their
  head. The all-at-once alternative is effectively unreviewable.
- **Risk isolation.** Until the final wiring PR, every counter is **dormant** —
  maintained but consumed by nothing. A bug in, say, the quicklist counter has
  zero runtime effect until wiring, so each PR before the last is provably
  behavior-neutral and safe to merge.
- **Correctness is proven before integration.** Each per-type PR ships a gtest
  that compares the O(1) counter against a brute-force recomputation over
  random mutation sequences plus an RDB round-trip. The hard part (per-type
  accounting) is validated in isolation, with no cluster setup, before any
  plumbing exists. The wiring PR is then mostly mechanical.
- **Easy to bisect and revert.** If a regression appears, the per-type
  granularity makes it obvious which counter is at fault, and any single PR can
  be reverted without unravelling the others.
- **Backport-friendly.** Per the repo guidelines, changes should stay minimal
  and easy to backport. Small, self-contained PRs backport and cherry-pick far
  more cleanly than one large cross-cutting change.
- **Parallelizable.** The per-type counters are independent, so different
  contributors can own different types concurrently once the scaffolding lands.
- **No long-lived feature branch.** Each PR merges straight to `unstable`
  without drifting, avoiding a giant branch that rots and conflicts.

---

## Implementation plan (phased rollout)

The feature is built in two halves: first land the per-type **O(1) size**
machinery, dormant; then wire up reporting last. Each per-type PR is small,
independently reviewable, unit-testable, and introduces **no behavior change**
— its counter is maintained but nothing consumes it until the final wiring PR.

> **"Per-type" here means per-type *sizing*, not per-type *reporting*.** Every
> PR below adds an O(1) size function for one object type; all of them feed the
> single aggregate `data_bytes` counter (phase 1). The optional per-type
> *breakdown* in the slot stat reuses these exact same size values — so this
> plan is unchanged whether or not the breakdown is ever built.

### Why O(1) is required

The snapshot mechanism computes `size_before` at `lookupKeyWrite` and
`size_after` at `signalModifiedKey` on every write in cluster mode. If sizing
walked the container, every write to a large list/stream/hashtable would be
O(N). A running counter is what keeps the gated path O(1) — this is the core
reason for doing size accounting per-type rather than recomputing on demand.

### What actually needs new code

Only the **large encodings** need a counter. The compact encodings are already
O(1) and need nothing beyond a `case` in `slotStatsObjectSize()`:

| Type | Already O(1) | Needs a running counter |
|------|-------------|-------------------------|
| String | `stringObjectLen` | — |
| Hash | listpack: `lpBytes` | hashtable: `Σ(field_len+value_len)` |
| Set | listpack/intset | hashtable: `Σ member_len` |
| ZSet | listpack: `lpBytes` | skiplist: `Σ(member_len+8)` |
| List | listpack: `lpBytes` | quicklist: `Σ node entry bytes` |
| Stream | — | rax: `Σ lpBytes` of all listpacks |
| Module | — | via module type-API size callback |

So "a PR per data type" holds, but several PRs are near-trivial (string), and
the real correctness work is the five counter-bearing encodings plus module.

### Phase 0 — Scaffolding (no behavior change)

- Add `data_bytes` to `slotStat` (single aggregate counter; the optional
  per-type array is deferred).
- Add `slotStatsObjectSize()` dispatch returning `0` for every type (TODO cases).
- Add the startup-fixed tracking flag the per-type counters check (see
  "Cost decision" below).
- No `db.c` hooks, no `signalModifiedKey` delta, no exposure yet.

### Phase 1..N — One PR per data type (O(1) size)

Recommended order (simplest / lowest-risk first):

1. **String** — fold into scaffolding; `stringObjectLen`.
2. **Hash** — listpack via `lpBytes`; hashtable gets a `tracked_data_bytes`
   field maintained on field add / overwrite / delete.
3. **Set** — listpack/intset O(1); hashtable gets a running counter.
4. **ZSet** — listpack O(1); skiplist gets a running counter.
5. **List** — listpack O(1); quicklist `tracked_data_bytes` maintained on node
   insert/delete (raw + compressed) and during RDB load.
6. **Stream** — `tracked_data_bytes` maintained at each rax listpack
   insert/delete and during RDB load.
7. **Module** — via the module type-API size callback (may be a follow-up).

Each per-type PR contains:
- The counter field + maintenance inside the type's own `.c` (gated on the
  tracking flag).
- The `case OBJ_<TYPE>:` in `slotStatsObjectSize()`.
- A gtest in `src/unit/` that drives random mutation sequences and asserts the
  counter equals a brute-force recomputation, plus an RDB serialize/deserialize
  round-trip. No cluster setup needed — correctness is proven here.

### Phase N+1 — Wiring ("turn it on")

- `db.c` hooks: `dbAddInternal`, `dbSetValue`, `dbGenericDeleteWithDictIndex`,
  `dbAddRDBLoad`, `emptyDbStructure`.
- `lookupKey` snapshot capture + `signalModifiedKey` delta application.
- `CLUSTER SLOT-STATS` `data-bytes` field + `ORDERBY data-bytes`.
- Integration tests per type under `tests/unit/cluster/`.

Because every per-type computation is already proven by the Phase 1..N unit
tests, this PR is mostly plumbing and is where behavior first becomes visible.

> Alternative considered: wire reporting first (all types return 0) and light
> up each type as it lands, giving per-PR integration tests. Rejected as the
> default because it exposes an incomplete/zeroed metric while types are still
> being filled in. Wire-last keeps the metric correct-or-absent; unit tests
> carry correctness in the interim.

### Cost decision — "zero when cluster disabled"

The counters live on the data structures, so the 8-byte field is
**unconditional** (a struct field cannot be added at runtime). To keep *CPU*
cost at zero for non-cluster deployments, gate counter **maintenance** on a
single startup-fixed global derived from `server.cluster_enabled`:

- `cluster_enabled` is immutable after startup and known *before* any RDB/AOF
  load, so gated counters are always consistent with the data — no backfill or
  recompute is ever needed.
- Unit tests flip this flag explicitly so they can validate the counters.

This refines the "Zero cost when cluster mode is disabled" statement above: the
field memory (8 bytes per large object) is unconditional; the per-mutation
maintenance work and the slot-stats/lookup-snapshot paths remain fully gated.

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

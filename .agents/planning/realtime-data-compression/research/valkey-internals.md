# Research — Valkey internals relevant to inline compression

_Scope: where in the current source the feature must plug in, and which invariants it must preserve._

## 1. Where a value lives: `robj` and its encodings

`robj` is the single value wrapper used by every type in the server. Defined in `src/server.h`:

```c
struct serverObject {
    unsigned type : 4;            // OBJ_STRING, OBJ_LIST, …
    unsigned encoding : 4;        // OBJ_ENCODING_RAW / _INT / _EMBSTR / …
    unsigned lru : LRULFU_BITS;   // 24-bit LRU or LFU field
    unsigned hasexpire : 1;
    unsigned hasembkey : 1;
    unsigned hasembval : 1;
    unsigned refcount : OBJ_REFCOUNT_BITS;
    void *val_ptr;                // sometimes absent (embedded value)
};
static_assert(sizeof(struct serverObject) <= 8 + sizeof(void *), …);
```

Strings carry one of three encodings today:

- `OBJ_ENCODING_INT` — value fits in `long`, stored in place of `val_ptr`.
- `OBJ_ENCODING_EMBSTR` — small string embedded directly after the header (`hasembval == 1`).
- `OBJ_ENCODING_RAW` — arbitrary-length `sds` referenced by `val_ptr`.

**Compression design implication.** The natural seam is a **new encoding**, e.g. `OBJ_ENCODING_COMPRESSED`, whose `val_ptr` points to a small compressed-value header + ZSTD frame. The 4-bit `encoding` field has `0..11` assigned today with `3/4/5` retired — **room to add one** without widening the struct. Keeping the feature inside the encoding axis (and not as a new `robj.type`) preserves every caller that tests `obj->type == OBJ_STRING`; only paths that touch bytes need to call a "decompress or view" helper.

## 2. How the keyspace reaches a value: the single seam

`src/db.c` owns the lookup API that every read/write command funnels through:

```c
robj *lookupKey(serverDb *db, robj *key, int flags);            // core
robj *lookupKeyRead(serverDb *db, robj *key);
robj *lookupKeyReadWithFlags(…);
robj *lookupKeyWrite(…);
robj *lookupKeyWriteWithFlags(…);
```

Inside `lookupKey` the hot-path work is:

1. `kvstoreHashtable…Find…` through the per-DB `kvstore`.
2. Expire check (`expireIfNeededWithDictIndex`).
3. **LRU/LFU touch** — `val->lru = lrulfu_touch(val->lru);` — gated by `LOOKUP_NOTOUCH` and suppressed during fork (`hasActiveChildProcess()`).
4. Hit/miss statistics, keyspace-miss notification.

Because every command path (including scripting and `EXEC`) goes through `lookupKey*`, it is the **single best place to install the decompression hook**. That keeps the blast radius of the feature small and uniform across subsystems.

Writes arrive via `dbAddInternal` → `kvstoreHashtableAdd` and `dbSetValue`/`dbOverwrite` (also in `db.c`). These are the mirror seam for **compression on insert/overwrite** — though the most common design will compress asynchronously *after* the value is installed, not inline on insert (see "Hot-path latency" below).

## 3. Keyspace layout (kvstore → hashtable)

```mermaid
graph LR
  S[struct valkeyServer] --> DB[serverDb[]]
  DB --> K[kvstore *keys]
  DB --> E[kvstore *expires]
  K --> H0[hashtable shard 0]
  K --> Hn[hashtable shard N-1]
  H0 --> V1[robj * value]
  H0 --> V2[robj * value]
```

- `kvstore` (`src/kvstore.c`) shards the keyspace across many `hashtable` instances; shards are iterated lazily for background work (active expiry, defrag, etc.). This is the right granularity for a **background "sweep" compressor** if we choose that path: process one shard at a time to bound work per iteration.
- `hashtable` (`src/hashtable.c`) is the current primary hash table. `dict` is legacy; new code targets `hashtable` + `kvstore`.
- There is no "value replacement" API below the db level that compression would need that isn't already present: we already install new `robj*` via `dbAdd`/`dbSetValue` and free the old one.

## 4. LRU/LFU: the "skip hot items" signal

`src/lrulfu.h`:

```c
#define LRULFU_BITS 24
uint32_t lrulfu_touch(uint32_t lrulfu);
uint32_t lrulfu_getIdleness(uint32_t lrulfu, uint32_t *idleness);
bool     lrulfu_isUsingLFU(void);
```

- LRU mode: 24-bit seconds-resolution clock. Idleness monotonically increases until `touch`.
- LFU mode: 16 bits "last access minute" + 8 bits logarithmic counter.

**Design implication.** The existing `lrulfu_getIdleness()` (LRU) and decayed counter (LFU) provide exactly the signal needed for "compress cold keys, skip hot ones." Policy:

- In LFU mode: compress values whose frequency counter is below a threshold (`compression-lfu-threshold`, configurable).
- In LRU mode: compress values whose idleness exceeds a threshold (`compression-lru-idle-seconds`).
- In `noeviction` / maxmemory policies that do not maintain useful LRU/LFU, fall back to a time-based "settle" window.

The signal is **already maintained on every `lookupKey`**, so there is no extra cost to query it from a background compressor.

## 5. Embedded values, short strings and shared objects

Two cases the feature must handle carefully:

1. **`hasembval == 1` (embedded value).** The string lives inside the `robj` allocation. Replacing it with a compressed blob means reallocating the robj (to `hasembval == 0, val_ptr → compressedHeader`) and updating the hashtable bucket. `dbSetValue`/`objectSetKeyAndExpire` already know how to deal with robj reallocation; the compressor must use these rather than mutate bytes in place.
2. **`OBJ_SHARED_REFCOUNT` shared objects** (pre-allocated integer strings, RESP constants). `lookupKey` asserts `val->refcount != OBJ_SHARED_REFCOUNT` before touching LRU — shared objects are never added to a db. The compressor inherits this invariant: it must assert and skip shared refcount.

## 6. Hot-path latency budget

`lookupKey` is on the critical path of every command. Its current overhead is essentially a hashtable walk + a few branches. Our design must ensure:

- **Decompress on first read** — yes, but only when the encoding flag is set. Uncompressed keys pay one extra predictable branch.
- **Never compress on the write path.** Inserts must remain synchronous. Compression happens in the background, either (a) worker thread triggered by a "candidate" queue or (b) a cron sweep.
- **Skip compression for hot items.** As described in §4.
- **Cache the decompressed view** (the POC calls this "coexistence"): after decompression, retain the uncompressed `robj` briefly (in a small cyclic array or by flipping the encoding back to RAW/EMBSTR) so repeated access does not decompress repeatedly. Eviction from that cache returns the key to compressed-only.

## 7. Subsystem interaction seams (what must *not* change)

| Subsystem | Interaction | Needed change |
|---|---|---|
| **Replication** (`src/replication.c`) | `feedReplicationBufferWithObject(robj*)` reads `sdslen` + `objectGetVal`. Steady-state is decompressed RESP on the wire. | Wrap with a "view as uncompressed" helper; wire format unchanged. |
| **AOF** (`src/aof.c`) | AOF is RESP; replays commands. | Use same "view as uncompressed" helper when serializing. No format change. |
| **RDB** (`src/rdb.c`) | Already has `RDB_ENC_LZF` for compressed strings (§ persistence-and-replication.md). | Add a new opcode for ZSTD-with-dict, behind `RDB_VERSION` bump. |
| **Eviction** (`src/evict.c`) | Uses `zmalloc_size(obj)` to price memory. | Memory accounting must reflect **compressed** size for compressed items. |
| **Defrag** (`src/defrag.c`) | Walks `kvstore`, relocates allocations using jemalloc hints. | New allocations (compressed header + frame) are ordinary zmalloc and are defragged as usual. |
| **Lazyfree** (`src/lazyfree.c`) | Free large objects off-thread via `bio`. | Compressed values still free through `decrRefCount`; no change. |
| **Modules** (`src/module.c`) | Modules call `RedisModule_OpenKey` / `StringDMA` and may expect raw `sds` semantics. | Decompression must be transparent at the module API boundary — the module gets a decompressed view. |
| **Tracking / CSC** (`src/tracking.c`) | Invalidations driven by `signalModifiedKey` keyed on the **logical key**. | Background re-compression **must not** call `signalModifiedKey` — it rewrites storage, not logical value. |

## 8. Initial scope: `OBJ_STRING` only, extensible later

- The encoding-level seam naturally extends to non-string types, because every `robj` has an encoding. A future `OBJ_HASH` listpack, for instance, can live behind the same compressed-encoding wrapper.
- For the first milestone only `type == OBJ_STRING && encoding in {RAW, EMBSTR}` is eligible. `OBJ_ENCODING_INT` strings stay as-is (they are already memory-optimal).

## 9. Summary of seams the design depends on

```mermaid
graph TB
  subgraph Hooks[Places the feature plugs in]
    LK[lookupKey* — decompress-on-read]
    DBA[dbAddInternal / dbSetValue — track "candidate for compression"]
    BG[Background sweeper — per kvstore shard, uses LRU/LFU]
    OBJ[Object encoding tag — new OBJ_ENCODING_COMPRESSED]
    MEM[zmalloc_size / memory accounting — report compressed size]
  end
  subgraph Unchanged[Wire and boundary formats]
    REPL[Replication feed buffer — decompressed RESP]
    AOFw[AOF rewrite / append — decompressed RESP]
    MOD[Module API — decompressed view]
    RDBw[RDB — new opcode behind version bump]
  end
  Hooks -. preserves .-> Unchanged
```

## References

- `src/server.h` — `robj`, encoding constants, type constants.
- `src/db.c` — `lookupKey`, `dbAddInternal`, `dbSetValue`.
- `src/object.c` — `createObject`, `createStringObject`, `tryObjectEncoding`.
- `src/kvstore.c`, `src/hashtable.c` — keyspace storage.
- `src/lrulfu.h` — LRU/LFU bit layout and API.

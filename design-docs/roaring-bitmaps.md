# Design Document: Compressed Bitmaps

Tracking issue: [#1480](https://github.com/valkey-io/valkey/issues/1480).

## 1. Overview

Valkey bitmaps are strings: bit `i` is a bit of byte `i / 8`, so a bitmap costs
one byte per eight bits up to its highest bit, however few bits are set. The
example of #1480 is a single user id near 2^31, which takes 256 MB.

This design adds a **bitmap data type** stored as a
[Roaring bitmap](https://roaringbitmap.org), the compressed bitmap format used
by Lucene, Spark, Druid, ClickHouse and many other systems. It uses
[CRoaring](https://github.com/RoaringBitmap/CRoaring), the C library for
Roaring bitmaps, which Redis also adopted for its compressed bitmaps
([redis/redis#15331](https://github.com/redis/redis/pull/15331)). It is
vendored under `deps/croaring`.

- `TYPE` reports `bitmap` and `OBJECT ENCODING` reports `roaring`.
- The bit commands (`SETBIT`, `GETBIT`, `BITCOUNT`, `BITPOS`, `BITOP`,
  `BITFIELD`, `BITFIELD_RO`) work on strings and on bitmaps, and return the
  same results for a bitmap as for the string holding the same bytes.
- Other string commands (`GET`, `STRLEN`, `APPEND`, `SETRANGE`, `INCR`, `PF*`,
  ...) reply `WRONGTYPE` for a bitmap.
- Bitmaps are persisted, replicated and migrated in their compressed form.

Nothing changes for strings unless a user creates a bitmap, with `BITCONVERT`
or `R.IMPORT`. There is no configuration: a bit write keeps the type of its
key, and no node's settings change the type a command produces. The one change
to existing behavior is a fix for `BITFIELD`: when it only extends its key, it
now counts as a write (Section 3.1).

## 2. Data Model

```
robj { type = OBJ_BITMAP, encoding = OBJ_ENCODING_ROARING }
  └── bitmapRoaring { roaring_bitmap_t *rb; size_t len; }
```

- **Bits.** Bit `i` of the bitmap is the value `i` of a 32-bit CRoaring
  bitmap. A bitmap holds up to 2^32 bits (512 MB as a string, the default
  `proto-max-bulk-len`; Section 2.4).
- **Length.** We keep the length in bytes of the equivalent string. It can
  extend past the last set bit, and the bit commands depend on it: `BITCOUNT`
  and `BITPOS` ranges (including negative ones), `BITPOS 0` without an end, and
  `BITOP` result lengths. Keeping it is what makes the bit commands return the
  same results for both types, and lets `BITCONVERT` convert in both directions
  without loss.
- **Containers.** See Section 2.1.

All CRoaring allocations go through `zmalloc`, so `INFO` and `maxmemory`
account for them exactly. `MEMORY USAGE` is an estimate (Section 2.3).

### 2.1 Roaring architecture

CRoaring splits the 32-bit values into 65,536 chunks of 2^16 values, keyed by
their high 16 bits. Each non-empty chunk has one container, which holds the
low 16 bits of its values in one of three forms:

| Container | Holds | Size | Used for |
|---|---|---|---|
| Array | sorted 16-bit values | 2 bytes per value, up to 4,096 values (8 KB) | sparse chunks |
| Bitset | 2^16 bits | 8 KB, always | dense chunks (more than 4,096 values) |
| Run | sorted runs, each a start and a length | 4 bytes per run | consecutive values |

An array never exceeds 8 KB, the size of a bitset, which bounds the cost of a
chunk. `run_optimize` (Section 2.2) picks a run container only where it is
smaller than the other two; some operations, such as range writes and `NOT`,
also produce run containers.
The containers sit in one sorted top-level array: three parallel arrays of
keys (2 bytes), container pointers (8 bytes) and type codes (1 byte), so 11
bytes per container, found by binary search. A bitmap whose values are spread
over many chunks therefore has many containers: 100,000 random 32-bit values
fall into about 51,000 chunks, two values each.

Sparse, clustered and dense bitmaps all compress well; random bitmaps of
middling density (around 1 to 50%) are about as large as strings.

### 2.2 When the containers are rebuilt

CRoaring offers two passes that rewrite a bitmap: `run_optimize`, which turns
each container into a run container where that is smaller (and back), and
`shrink_to_fit`, which frees the spare capacity of every array (top-level and
containers). They cost a pass over the bitmap, so we call them only where a
bitmap is built at once:

| Path | `run_optimize` | `shrink_to_fit` |
|---|---|---|
| String to bitmap: `BITCONVERT key BITMAP`, string sources of `BITOP`, `R.EXPORT` of a string | yes | yes |
| `R.IMPORT`, RDB load, `RESTORE` (also full sync) | no: the containers are those of the payload | yes, though there is little to free: the deserializer allocates exact sizes |
| `SETBIT`, `BITFIELD` | no | no |
| `BITOP` result | no: CRoaring picks the containers (for example, `NOT` of a sparse bitmap gives run containers) | no |
| `COPY` (`roaring_bitmap_copy`) | no | no: the copy has the same containers |

So a bitmap built by bit writes keeps whatever containers the writes left,
until it is converted to a string and back. `R.IMPORT` keeps the containers of
its payload, like `RESTORE`, so AOF rewrite and atomic slot migration, which
send `R.IMPORT`, rebuild the source's containers exactly. A client that wants
run containers calls `runOptimize()` in its Roaring library before sending. In a test, `BITOP NOT` of a sparse
bitmap of 1,527 chunks took 37.9 KB, and 32.1 KB after `BITCONVERT` to a
string and back: the difference is modest.

Payloads from outside (`R.IMPORT`, RDB, `RESTORE`) are validated before use
(Section 4.3).

### 2.3 What a write does to the containers

`SETBIT` adds or removes one value; `BITFIELD` clears the bytes it writes
(`roaring_bitmap_remove_range`) and adds the new bits
(`roaring_bitmap_add_many`). Inside a container:

- **Array.** A value is inserted in sorted order. When the array is full, its
  capacity grows: doubled below 64 values, times 1.5 below 1,024, times 1.25
  above, and never past 4,096. Adding a 4,097th value converts the array into
  a bitset (8 KB).
- **Bitset.** Adding a value sets a bit. Removing a value that leaves 4,096 or
  fewer converts the bitset back to an array. There is no hysteresis:
  toggling one value around 4,096 converts the container back and forth.
- **Run.** Adding or removing a value extends, merges or splits runs; the run
  list grows like an array. A run container stays a run container, even when
  an array or a bitset would be smaller, until the next `run_optimize`.
- **Empty container.** Removing the last value of a chunk frees its container
  and removes it from the top-level array.

The top-level array grows when a write needs a new container: to twice the
needed size below 1,024 containers, and to 1.25 times above, up to 65,536.
Inserting or removing a container in the middle shifts the following entries
(11 bytes each). With tens of thousands of containers, that shift dominates:
on 100,000 random 32-bit ids (about 51,000 containers), a `SETBIT` that creates
or empties a container moves about 280 KB, and random `SETBIT`s cost 1.7 µs on
average, against 0.2 µs on bitmaps with 1,500 containers (measured on an
Intel Xeon Gold 6548N). A
`SETBIT` within an existing container never shifts the top-level array.
`roaring64`, which keeps its containers in a radix tree, avoids this cost
(Section 10).

No capacity is returned on its own: arrays keep their spare capacity, and the
top-level array keeps its size after containers are removed, until a rebuild of
Section 2.2 (for example `DEBUG RELOAD`, a restart, or `BITCONVERT` to a string
and back). `INFO` and `maxmemory` see the spare capacity, since it is
allocated through `zmalloc`. `MEMORY USAGE` counts the spare capacity of the
top-level array, but counts containers by their contents, so it can
underestimate a bitmap whose containers have spare capacity.

Code: [`src/bitmap_roaring.c`](../src/bitmap_roaring.c) (data structure),
[`src/bitops.c`](../src/bitops.c) (commands).

### 2.4 Size limits

A bitmap is limited by its length: the `len` field, the size in bytes of the
equivalent string. The limit is `BITMAP_ROARING_MAX_BYTES` = 2^29 bytes (512
MB), the most that 2^32 bits take. It is a constant, never derived from
`proto-max-bulk-len`, so every node agrees on it whatever its configuration.

Every path that sets or increases the length checks it:

| Path | Check |
|---|---|
| `SETBIT`, `BITFIELD` | First the check that strings have: the offset must be below `proto-max-bulk-len` × 8. Then, for a bitmap, a write that reaches byte 2^29 or beyond fails with `bit offset is out of range for a bitmap`, and the key is unchanged. Only then does the length grow (`bitmapRoaringGrow()`). |
| `BITOP` | The result is as long as its longest source. It is a bitmap only if that length is at most 2^29, otherwise a string (Section 3.1). |
| `BITCONVERT key BITMAP` | A string longer than 2^29 bytes is refused. |
| `BITCONVERT key STRING` | A bitmap longer than `proto-max-bulk-len` is refused, so no client gets a string that `SETRANGE` could not make. |
| `R.IMPORT` | Values are 32-bit, so the length is at most 2^29. A length above `proto-max-bulk-len` is refused. |
| RDB load, `RESTORE` | A length above 2^29, or a bit past the length, is reported as corrupt data. |

Nothing else changes the length, and `serverAssert`s in `bitmap_roaring.c`
check the limit wherever a length is set. As for strings, the
`proto-max-bulk-len` checks are skipped for commands from the primary, AOF
loading and slot import (`mustObeyClient()`), which replay writes already
accepted.

The two limits differ when `proto-max-bulk-len` is changed:

- **Raised** past 512 MB, strings can grow past 2^32 bits; bitmaps cannot. A
  write past bit 2^32 - 1 of a bitmap fails, and a `BITOP` with a longer
  string source stores a string.
- **Lowered**, the offset check limits bitmaps as it limits strings.
  `BITCONVERT key STRING` and `R.IMPORT` refuse a bitmap longer than the new
  limit; a bitmap made before the change stays usable with the bit commands.

One difference from strings remains at the default limit. For a string,
`BITFIELD` checks only the offset where a field starts, so a field that starts
below bit 2^32 and ends past it grows the string a few bytes past 512 MB. A
bitmap checks the end of the field, and that write fails.

**Memory.** The length bounds the memory: at most 65,536 containers, each at
most 8 KB, plus 11 bytes each in the top-level array and a few small
structures, so at most about 512 MB plus a few MB, slightly more than the
equivalent string. Typical bitmaps use far less (Section 9). `maxmemory`
applies as usual: the commands that can grow a bitmap are `DENYOOM`. Payloads
near the limit can also be too long to send back as a client argument (Section
4.1).

## 3. Commands

### 3.1 Which type a command produces

Bit writes keep the type of their key:

| Command | Key | Result |
|---|---|---|
| `SETBIT`, `BITFIELD` (writes) | string | string |
| | bitmap | bitmap |
| | missing | string, as today |

A bit write past bit 2^32 - 1 of a bitmap fails with an error rather than
changing the type of the key (Section 2.4). A `BITFIELD` that only extends its key (every
operation fails under `OVERFLOW FAIL`) counts as a write: it is replicated and
written to the AOF, emits the `setbit` event, and invalidates `WATCH`, for
strings and bitmaps alike. Its reply is unchanged. This fixes a divergence that
exists for strings today: the replica kept a shorter value.

`BITOP op dest src...` reads all its sources before writing `dest`, which may
be one of them. With `maxlen` the length of the longest source in bytes
(missing keys count as zero), for every operation including `NOT`:

| Condition, in order | `dest` | Reply |
|---|---|---|
| `maxlen` = 0 | deleted, if it exists | 0 |
| a source is a bitmap (even an empty one) and `maxlen` <= 2^29 | a bitmap of `maxlen` bytes | `maxlen` |
| otherwise | a string of `maxlen` bytes | `maxlen` |

The 2^29 cutoff is a constant, not `proto-max-bulk-len`, so every node chooses
the same type. A string result would expand bitmaps (a `BITOP OR` of two
bitmaps with a bit near 2^31 would write 256 MB), so a bitmap source gives a
bitmap. An empty bitmap counts: `BITOP OR dest empty_bitmap some_string`
stores a bitmap. The cost: `GET dest` after such a `BITOP` replies
`WRONGTYPE`.

### 3.2 New commands

- `BITCONVERT key BITMAP|STRING` converts a key between the two types, keeping
  its bits, its length and its TTL. It is the explicit opt-in per key, and the
  way back to a string (for `GET`). For a missing key, `BITCONVERT key BITMAP`
  creates an empty bitmap, which `SETBIT` and `BITFIELD` then grow. It replies
  1 when it creates or converts the key, and 0 when nothing changes.
- `R.EXPORT key` returns the bits of a bitmap or a string in the
  [portable Roaring format](https://roaringbitmap.github.io/RoaringFormatSpec/),
  which the Roaring libraries of many languages read and write (table below).
  Applications can fetch a large membership set compressed, and use it
  natively.
- `R.IMPORT key payload` stores a bitmap from that format. Like `SET`, it
  replaces any value and TTL. Payloads come from clients, so they are read with
  CRoaring's safe deserialization and then checked with
  `roaring_bitmap_internal_validate()`.

Libraries that read and write the portable format, for 32-bit values:

| Language | Library | Read, write |
|---|---|---|
| C | [CRoaring](https://github.com/RoaringBitmap/CRoaring) | `roaring_bitmap_portable_deserialize_safe`, `roaring_bitmap_portable_serialize` |
| C++ | [CRoaring](https://github.com/RoaringBitmap/CRoaring) (`roaring::Roaring`) | `Roaring::readSafe`, `Roaring::write` |
| Java | [RoaringBitmap](https://github.com/RoaringBitmap/RoaringBitmap) | `RoaringBitmap.deserialize`, `RoaringBitmap.serialize` |
| Go | [roaring](https://github.com/RoaringBitmap/roaring) | `Bitmap.ReadFrom`, `Bitmap.WriteTo` |
| Rust | [roaring-rs](https://github.com/RoaringBitmap/roaring-rs) ([crate](https://crates.io/crates/roaring)) | `RoaringBitmap::deserialize_from`, `RoaringBitmap::serialize_into` |
| Python | [pyroaring](https://github.com/Ezibenroc/PyRoaringBitMap) | `BitMap.deserialize`, `BitMap.serialize` |

Other implementations are listed at
[roaringbitmap.org](https://roaringbitmap.org/).

The names are open (see Section 10).

## 4. Persistence

### 4.1 RDB and DUMP

A new object type, `RDB_TYPE_BITMAP = 24`, in RDB version 82:

```
RDB_TYPE_BITMAP | length in bytes (rdbSaveLen) | portable Roaring bitmap (string)
```

- A sparse bitmap stays small in snapshots, full syncs and `DUMP` payloads, and
  loads directly as a bitmap.
- Loading validates the payload like `R.IMPORT` does, and checks that no bit
  lies past the length. An invalid payload is reported as a corrupt RDB, and
  `RESTORE` replies `Bad data format`.
- Like path hashes (RDB 81), a bitmap cannot be written for a target that only
  reads older RDB versions: `rdbSaveObjectType()` returns -1 for it, so an
  older replica fails its full sync while newer replicas sync normally.
- **Size limit.** A client cannot send an argument longer than
  `proto-max-bulk-len`, so a `DUMP` payload (for `RESTORE` and `MIGRATE`) or an
  `R.EXPORT` payload (for `R.IMPORT`) longer than that cannot be sent back.
  Strings already have this problem: at the default limit of 512 MB, a 512 MB
  string gives a `DUMP` payload 16 bytes too long. A bitmap close to 2^32 bits
  (512 MB as a string) and dense, with most chunks stored as 8 KB bitsets, has
  the same problem across a wider range: its payload adds 8 bytes per chunk,
  up to 512 KB. `MIGRATE` then fails and keeps the key on the source. RDB, AOF,
  replication and atomic slot migration are not affected: they do not go
  through a client's argument limit. Such a bitmap does not compress, and is
  better kept as a string.

### 4.2 AOF rewrite and atomic slot migration

`rewriteBitmapObject()` emits `R.IMPORT key <payload>`, which makes the bitmap
just long enough for its last set bit, followed by `SETBIT key <last bit> 0`
when the length extends past it. Atomic slot migration sends keys in this
format too.

### 4.3 Corrupted payloads

A bitmap payload can come from a client (`R.IMPORT`, `RESTORE`), a file (RDB,
AOF) or another node (full sync, slot migration). Any of them can be corrupted
or crafted, and an invalid Roaring bitmap could make CRoaring read or write out
of bounds later. So every payload goes through the same checks
(`deserializeRoaring()` in `src/bitmap_roaring.c`), always, whatever
`sanitize-dump-payload` says:

1. **Size.** The size that the payload's header declares, computed with
   `roaring_bitmap_portable_deserialize_size()`, must equal the payload's
   size: no truncated payload, no trailing bytes.
2. **Bounded deserialization.**
   `roaring_bitmap_portable_deserialize_safe()` builds the bitmap, and never
   reads past the end of the payload, whatever counts and offsets the header
   claims.
3. **Validation.** `roaring_bitmap_internal_validate()` checks the bitmap it
   built: chunk keys strictly increasing; no empty container; array values
   strictly increasing, at most 4,096 of them; bitset cardinalities that match
   their bits and exceed 4,096; runs sorted, not overlapping, not adjacent
   (they would have been merged) and within the 2^16 values of their chunk.
   Validation needs the deserialized structure, so it runs after step 2.
4. **Length.** For RDB and `RESTORE`, the saved length must be at most 2^29
   bytes, and no bit may lie past it. For `R.IMPORT`, the length must not
   exceed `proto-max-bulk-len` (Section 2.4).

These checks cost one pass over the payload, which deserialization makes
anyway. A payload that fails any of them is rejected, and nothing is stored:

| Source | What happens |
|---|---|
| `R.IMPORT` | `ERR invalid Roaring bitmap payload`; the key is unchanged. |
| `RESTORE` (and the target of `MIGRATE`) | `Bad data format`, as for other types; the key is unchanged. The `DUMP` checksum and RDB version are checked first, so a corrupted payload is usually caught before the bitmap checks. |
| RDB file at startup | `Invalid bitmap`, reported as a corrupt RDB: the server runs the RDB check and exits, as for any corrupt RDB. |
| Full sync, disk-based or diskless | Same as a corrupt RDB: the replica logs the error and exits. |
| AOF loading | The `R.IMPORT` fails like any command that fails while loading: the server logs `== CRITICAL == This server is sending an error to its AOF-loading-client`, the key is missing, and loading goes on. |
| `valkey-check-rdb` | Reports the bitmap as invalid. |

The checks catch payloads that are not valid Roaring bitmaps; they cannot
catch a corruption that leaves a valid bitmap with other bits. RDB files and
`DUMP` payloads carry a checksum for that; `R.IMPORT` payloads and the AOF do
not, as for every other command.

The tests cover truncated, extended, garbage and unsorted `R.IMPORT` payloads,
and `RESTORE` of payloads with a wrong length or a corrupted header. A fuzzer
also sends randomly mutated `R.IMPORT` and `RESTORE` payloads (bit flips,
truncations, insertions, overwritten counts) to servers built with ASAN and
UBSAN. The server must not crash, and every payload it accepts must behave like
the string holding its bits.

## 5. Replication and AOF

The type of every result follows from the command and the types of its keys,
never from a node's configuration. Every command therefore propagates
verbatim, and replicas and the AOF, replaying the same commands, build the
same types as the primary. A key changes type in place only through
`BITCONVERT`, which is propagated like any other write. AOF rewrite and atomic
slot migration do not replay commands: they rebuild each bitmap from its
value (Section 4.2).

## 6. Other Components

| Component | Behavior |
|---|---|
| `COPY`, `RENAME`, `MOVE` | Work like any type. |
| `SCAN ... TYPE bitmap` | Supported. |
| `DEBUG DIGEST` | Mixes the bits of each chunk, not the containers, so two bitmaps with the same bits have the same digest whatever their containers. |
| Keyspace events | Bit commands keep their events (`setbit`, `set`) in the string class `$`, so existing subscribers see no change. `BITCONVERT` emits `bitconvert`, after `new` when it creates the key. |
| Modules | `VALKEYMODULE_KEYTYPE_BITMAP`; `ValueLength()` returns the length in bytes. The string API (`StringDMA`, ...) rejects bitmaps like other types. |
| Lazy free | Effort is the number of containers. |
| Active defrag | Not implemented yet: bitmaps are skipped. |
| `valkey-check-rdb` | Reports bitmaps as their own type. |

## 7. A New Type or a String Encoding?

An earlier prototype
([branch](https://github.com/valkey-io/valkey/compare/unstable...lemire:valkey:roaring-bitmaps))
stored Roaring bitmaps as an *encoding* of strings: `TYPE` stayed `string` and
every string command worked, materializing bytes when needed. The data
structure and the bit command code are the same in both designs. They differ in
what users and the rest of the server see:

| | String encoding | New type (this design) |
|---|---|---|
| `TYPE` | `string` | `bitmap` |
| `GET`, `GETRANGE`, `STRLEN`, `APPEND`, `SETRANGE` | Work. `GET` builds the whole string: 256 MB for one bit near 2^31. | `WRONGTYPE`; `BITCONVERT` gives a string on request. |
| HyperLogLog, `LCS`, `INCR` | Work, converting or materializing the bytes. | `WRONGTYPE` |
| `SORT ... BY` and `GET` patterns | Read the bytes. | A bitmap counts as a missing key, like any key that is not a string. |
| Code outside bit commands | Every place that reads a string's bytes must handle the encoding (`object.c`, `networking.c`, `t_string.c`, `sort.c`, `hyperloglog.c`, ...). | The usual per-type plumbing, as for any type. |
| Persistence | A plain string, unless a new RDB type is also added for the encoding. | Its own RDB type. |
| Turning it on | A configuration can apply to every key. | Per key, with `BITCONVERT` or `R.IMPORT`. |
| Compatibility | Existing clients and scripts work unchanged. | Nothing changes until a key is converted, apart from the `BITFIELD` fix (Section 3.1). Clients that check `TYPE` or call `GET` on a bitmap, or on a `BITOP` result with a bitmap source, must adapt; the bit commands return the same results. |
| Redis 8.12 | Different. | Same type model, without the configuration (Section 8). |

The new type makes the cost of a bitmap explicit: no command silently turns a
few kilobytes into hundreds of megabytes. It also keeps the encoding out of the
string code. The string encoding is more transparent: an application can turn
it on without changing anything else.

The new type follows Valkey's conventions. Some commands read several types:
the sorted-set commands `ZUNION`, `ZINTER` and `ZDIFF` accept sets as inputs,
and their `STORE` forms always write a sorted set; `SORT` reads lists, sets
and sorted sets, and `SORT ... STORE` always writes a list. The bit commands
likewise read strings and bitmaps. And no command changes a key's type in place unless the
caller names the target type: a key otherwise changes type only when it is
overwritten (`SET`, `RESTORE`, `BITOP`, ...). `BITCONVERT` names the target
type. This is why bit writes do not convert strings, and why there is no
configuration to make them do so.

## 8. Comparison with Redis 8.12

Redis added compressed bitmaps in 8.12
([redis/redis#15331](https://github.com/redis/redis/pull/15331)) with the same
model: a new type, `TYPE bitmap`, bit commands on both types, and `WRONGTYPE`
from string commands. The differences:

| | Valkey (this design) | Redis 8.12 |
|---|---|---|
| `OBJECT ENCODING` | `roaring` | `bitmap-roaring` |
| Configuration | None: bitmaps are created explicitly | `bitmap-default-roaring` (off by default): bit writes create bitmaps and convert strings |
| Creating an empty bitmap | `BITCONVERT key BITMAP` on a missing key | None documented; bit writes create bitmaps when the configuration is on |
| Offsets | 32-bit (2^32 bits) | 64-bit (CRoaring's `roaring64`) |
| Getting the bytes | `BITCONVERT key STRING` | `DEBUG BITMAP-RAW` |
| Portable import/export | `R.IMPORT`, `R.EXPORT` | None |
| Replicating conversions | Commands propagate verbatim | Internal commands `BITCONVERT key` and `BITROAROP` |
| Keyspace events | String class `$` | New class `b`, plus `type_changed` |
| RDB type | 24, RDB 82 | 33, RDB 16 (both formats are already incompatible) |
| SIMD | Enabled, selected at runtime | Disabled |

Redis also has an internal `BITCONVERT key` command, with a different syntax
from ours. The Redis column is taken from the description of its pull request,
not checked against its code. An application that relies on Redis's
configuration needs one `BITCONVERT` per key with Valkey.

## 9. Performance

Measured on an Intel Xeon Gold 6548N, with this branch and with Redis at the
merge commit of its compressed bitmaps (redis/redis#15331). Times are
server-side (`INFO commandstats`), in µs, the best of three runs. Each dataset
holds two bitmaps generated from the same seed in every mode.

| Dataset | Operation | Valkey string | Valkey bitmap | Redis bitmap |
|---|---|---:|---:|---:|
| 0.01% of 100M ids, random | memory | 12.0 MB | 36.0 KB | 98.2 KB |
| | `BITCOUNT` | 789 | 1.4 | 24.4 |
| | `BITOP OR` | 11,696 | 137 | 699 |
| 1% of 100M ids, random | memory | 12.0 MB | 1.9 MB | 2.2 MB |
| | `BITOP AND` | 7,573 | 965 | 9,949 |
| 50% of 100M ids, random | memory | 12.0 MB | 11.9 MB | 12.0 MB |
| | `BITOP AND` | 11,304 | 1,985 | 4,597 |
| 10 runs of 1M ids in 100M | memory | 12.0 MB | 2.7 KB | 12.2 KB |
| | `BITOP AND` | 11,399 | 1.2 | 57.4 |
| 100k random 32-bit ids | memory | 512 MB | 746 KB | 3.1 MB |
| | `BITOP AND` | 722,635 | 4,541 | 9,517 |
| | `SETBIT` | 0.09 | 1.7 | 0.50 |

Point operations (`GETBIT`, `SETBIT`, `BITPOS`) cost 0.1 to 0.4 µs on a
bitmap, against about 0.06 µs on a string. The slow case is `SETBIT` that
creates or empties a container in a bitmap with tens of thousands of
containers (last row; Section 2.3). An earlier prototype with the same code
also found `BITOP` 2 to 3.5 times slower than on strings for random bitmaps
with about 99% of bits set.

## 10. Open Questions

1. **A global switch, as in Redis?** A configuration under which bit writes
   create bitmaps and convert strings is an alternative to the rule of
   Section 7, not an extension of it: bit writes would change a key's type
   without the caller naming it. It is also hard to replicate. Replicas must
   not depend on their own configuration, so the primary would have to
   propagate each conversion, and an earlier version of this design did so
   with `alsoPropagate()`. That splits a write from its conversion: in a
   script with `server.set_repl(server.REPL_NONE)`, the conversion is
   propagated but the write is not; from a module's `RM_Call()` without `!`,
   neither is, and the replica keeps a string. A safe switch would propagate
   the conversion exactly when the write itself is propagated, as one
   decision. What such a switch would bring is automatic creation, as with
   Redis's `bitmap-default-roaring` (the "Turning it on" row of Section 7),
   not the transparency of the string encoding: a converted key is a bitmap,
   so `GET`, `APPEND`, `INCR` and the other string commands reply
   `WRONGTYPE` for it.
2. **Names:** `R.EXPORT`/`R.IMPORT` look like a module's, and an `R` prefix
   usually means "right" in Valkey (`RPUSH`). `BITEXPORT`/`BITIMPORT`?
   Should `OBJECT ENCODING` match Redis's `bitmap-roaring`?
3. **Keyspace events:** keep the string class, or add a bitmap class as Redis
   does?
4. **64-bit offsets** with `roaring64`, as in Redis: it lifts the 2^32 limit
   and makes `SETBIT` on scattered chunks cheaper, at some cost elsewhere.
5. **RDB version:** 82 is mapped to Valkey 9.3 as a placeholder.

# Replication

This document is an overview of how replication works in Valkey: how a replica
attaches to a primary, how it synchronizes (partial resync vs full sync), how the
primary groups concurrent full syncs into cohorts, and how the replication stream
is optionally compressed on the wire.

Most of the code lives in `src/replication.c`, with the RDB snapshot format in
`src/rdb.c`, the streaming codec framing in `src/compression_stream.{c,h}`, and
the replica-side RDB rewrite in `src/rdb_transcoder.{c,h}`. Configuration
parameters are named inline throughout and collected in §7.

---

## 1. Overview

Replication is asynchronous and single-primary: a replica connects to one primary
and receives a continuous stream of writes. A replica can itself have
sub-replicas (chained replication).

A replication link has two phases:

1. **Synchronization** – the replica obtains a consistent starting point, either
   by replaying a small amount of buffered history (**partial resync**) or by
   loading a full dataset snapshot (**full sync**).
2. **Streaming** – from that point on, the primary forwards every write over the
   link (the *replication stream*).

Progress is tracked with two identifiers:

- **replication ID** (`replid`): identifies the primary's history. PSYNC2 keeps a
  secondary replid so a promoted replica's followers can often avoid a full sync.
- **replication offset**: a monotonically increasing byte count of the
  replication stream. Primary and replica compare offsets to detect divergence.

The primary keeps a bounded in-memory **replication backlog** of recent stream
bytes so a briefly disconnected replica can be caught up without a full resync.
Its size and lifetime are controlled by `repl-backlog-size` and
`repl-backlog-ttl` (how long the backlog is retained after the last replica
disconnects).

---

## 2. Handshake: REPLCONF and PSYNC

When a replica connects it performs a handshake: `PING`, then one or more
`REPLCONF` exchanges to describe itself, then `PSYNC` to start synchronization.

### 2.1 REPLCONF subcommands

`REPLCONF <option> <value> [<option> <value> ...]` is sent by the replica to
configure the primary side before `PSYNC`. All subcommands flow replica → primary
except `getack`, which the primary sends on the stream to poll the replica. Each
handshake subcommand replies `+OK`; `ack` and `getack` — the two exchanged on the
live replication stream — send no reply. The options (`replconfCommand`,
`src/replication.c`):

- **`listening-port <port>`** / **`ip-address <ip>`** — how the replica wants to
  be reached, so the primary can list it accurately in `INFO replication`, `ROLE`,
  and cluster topology. `ip-address` overrides the address the primary would
  otherwise infer from the connection.
- **`capa <cap>`** — advertise one capability (see §2.2). Sent once per
  capability, so a replica emits several `capa` pairs in the same `REPLCONF`.
  Capabilities the primary doesn't recognize are silently ignored.
- **`ack <offset> [fack <aofofs>]`** — the replica reports how far it has processed
  the replication stream, and optionally the offset it has fsynced to its AOF
  (`fack`). Streamed periodically (not just once), it drives offset tracking,
  `WAIT`/`WAITAOF`, and `min-replicas-*`. It deliberately sends no reply to avoid
  round-trip chatter on the hot path.
- **`getack <dummy>`** — sent primary → replica on the stream to request an
  immediate `ack` (e.g. to satisfy a `WAIT`). The replica responds by *sending its
  own* `REPLCONF ACK`, not by replying to this command.
- **`rdb-only <0|1>`** — the client wants only an RDB snapshot and no subsequent
  command stream, e.g. a backup tool that uses the sync machinery to fetch a dump
  and then disconnects.
- **`rdb-filter-only <filters>`** — *filtering:* restrict the RDB to an
  include-list. The only supported filter today is `functions` (ship functions but
  no key data); an empty string yields an empty RDB. This sets the
  `REPLICA_REQ_RDB_EXCLUDE_*` requirement flags.
- **`version <major.minor.patch>`** — the replica reports its Valkey version so the
  primary can pick the highest RDB version the replica understands (§4.5).
- **`rdb-channel <0|1>`** — marks this connection as the bulk RDB channel of a
  dual-channel sync, setting `REPLICA_REQ_RDB_CHANNEL`.
- **`set-rdb-client-id <id>`** — on the main channel of a dual-channel sync, links
  it to the already-open RDB-channel connection identified by `<id>`.
- **`set-cluster-node-id <id>`** — in cluster mode, tells the primary the replica's
  cluster node id.

`rdb-only`, `rdb-filter-only`, and `rdb-channel` translate into **replica
requirement** flags (`REPLICA_REQ_*`) that partly determine cohort grouping
(§4.5).

### 2.2 Capabilities (`REPLCONF capa`)

Capabilities are advertised **replica → primary**, one `capa <name>` pair each;
the primary adapts what it sends to what the replica claims. Unknown names are
ignored. The names, exactly as sent:

- **`eof`** (`REPLICA_CAPA_EOF`) — can parse the EOF-marked (diskless) RDB stream.
- **`psync2`** (`REPLICA_CAPA_PSYNC2`) — understands `+CONTINUE <new replid>`.
- **`dual-channel`** (`REPLICA_CAPA_DUAL_CHANNEL`) — supports dual-channel sync,
  honored only if the primary has `dual-channel-replication-enabled` and
  `repl-diskless-sync` (§4.3).
- **`skip-rdb-checksum`** (`REPLICA_CAPA_SKIP_RDB_CHECKSUM`) — accepts an RDB with
  no CRC64 trailer, used to skip the checksum when the transport already provides
  integrity (e.g. TLS).
- **`lz4`** (`REPLICA_CAPA_LZ4`) — accepts LZ4 streaming-compressed payloads.
- **`zstd`** (`REPLICA_CAPA_ZSTD`) — accepts Zstd streaming-compressed payloads.

### 2.3 PSYNC

`PSYNC <replid> <offset>` requests continuation from an offset. The primary
answers `+CONTINUE [replid]` (partial resync, §3) or `+FULLRESYNC <replid>
<offset>` followed by a full sync (§4).

---

## 3. Partial resynchronization (PSYNC)

If the replica presents a `replid` the primary recognizes and an offset still
covered by the backlog, the primary replies `+CONTINUE` and streams the backlog
from that offset onward — no RDB is transferred. This is the cheap, common path
after transient disconnects.

If the replid is unknown or the offset has fallen out of the backlog, the primary
falls back to a **full sync**.

---

## 4. Full synchronization

A full sync ships a complete RDB snapshot, then continues with the live stream.

### 4.1 Disk-based transport

With `repl-diskless-sync no`, the primary runs a `BGSAVE` to its RDB file
(`dump.rdb`) and streams that file prefixed by its length: `$<len>\r\n<bytes>`
(*size-framed*). Replicas that arrive while a save is in flight can piggyback on
the same child.

### 4.2 Diskless transport

With `repl-diskless-sync yes`, the primary forks and streams the RDB straight to
replica sockets without touching disk. Because the length is not known up front,
the payload is framed with a random 40-byte end-of-file marker
(`RDB_EOF_MARK_SIZE`): the stream starts `$EOF:<40-byte-mark>` and ends with the
same 40 bytes (*EOF-framed*). Only replicas that advertised `capa eof` can receive
this.

Two knobs shape diskless cohorts:

- `repl-diskless-sync-delay` – seconds to wait before forking, so more replicas
  can arrive and share one snapshot.
- `repl-diskless-sync-max-replicas` – cap on replicas per diskless round
  (`0` = unlimited).

### 4.3 Dual-channel transport

With `dual-channel-replication-enabled yes` (which also requires
`repl-diskless-sync`), the bulk RDB is transferred on a dedicated RDB channel
while the live command stream is buffered on a second channel, reducing the
primary's memory pressure during large transfers. The replica advertises `capa
dual-channel` and marks its RDB connection with `REPLCONF rdb-channel 1`; this
sets `REPLICA_REQ_RDB_CHANNEL` on that client.

Unlike the normal handshake, where an error reply to a `REPLCONF` is treated as
non-critical and ignored, the dual-channel handshake *does* branch on the reply:
if the primary errors on one of the dual-channel probes (the offset-sync
`REPLCONF`, `REPLCONF set-rdb-client-id`, or `REPLCONF ip-address`), the replica
concludes the primary does not support dual channel and falls back to a regular
single-channel sync.

### 4.4 Replica load paths

Controlled by `repl-diskless-load`:

- `disabled` (default) – **disk-receive**: the replica writes the incoming RDB to
  its own `dump.rdb`, then loads it. Runs in a background I/O (BIO) thread.
- `swapdb` / `on-empty-db` – **socket load**: the replica parses the RDB directly
  off the socket into memory (`swapdb` keeps the old dataset until the load
  succeeds; `on-empty-db` only when the keyspace is empty).

### 4.5 Cohort splitting and transport selection

When several replicas are waiting for a full sync (`REPLICA_STATE_WAIT_BGSAVE_START`)
at the same time — for example because a save was already in flight — the primary
does **not** force a single lowest-common-denominator snapshot on all of them.
It picks one waiting replica as the reference and admits into the round only the
replicas that match it on **all three** dimensions below; the rest stay parked
and are served in a later round. The same three-way filter guards both the
disk-based grouping (`src/replication.c`) and the diskless grouping (`src/rdb.c`).

1. **`req` — replica requirements** (`REPLICA_REQ_*`, a bitfield): the RDB content
   and channel a replica asked for. Set from the `REPLCONF` filters of §2.1:
   - `REPLICA_REQ_RDB_EXCLUDE_DATA` / `REPLICA_REQ_RDB_EXCLUDE_FUNCTIONS` — from
     `rdb-filter-only` (`REPLICA_REQ_RDB_MASK` covers these "filtered" RDBs).
   - `REPLICA_REQ_RDB_CHANNEL` — from `rdb-channel` (dual-channel).

   Replicas wanting different RDB contents or a different channel cannot share one
   snapshot.

2. **`rdbver` — RDB version** (`replicaRdbVersion()`): the highest RDB version the
   replica understands, from its reported `REPLCONF version` mapped through
   `RDB_VERSION_MAP`. A non-EOF (disk-based) replica is forced to the latest
   `RDB_VERSION`, because that file may be reused for other disk-based replicas.
   Replicas needing different versions get different snapshots.

3. **`compr` — full-sync codec** (`replSelectFullSyncCompression()`, §5.4): the
   codec a given replica will actually receive. Replicas that negotiate different
   wire codecs are split into separate rounds, so a capable replica keeps its
   compressed round even when an incompatible replica is waiting alongside it.

Because the reference replica changes each round, all waiting cohorts are
eventually served; there is no "one odd replica downgrades everyone" behavior.

Those same three dimensions also decide each cohort's **transport**. A disk-based
full sync reuses one on-disk `dump.rdb`, which has a single fixed shape: full
contents (no filter), the latest `RDB_VERSION`, and the configured
`rdbcompression` codec. The one cohort that matches that shape can be served
**disk-based**, streaming `dump.rdb` verbatim (size-framed). Every other cohort
**falls back to diskless**: the primary generates a fresh stream in the cohort's
own shape and leaves `dump.rdb` untouched so it stays reusable for the cohort that
does match. Concretely, given an EOF-capable cohort, the primary uses a diskless
(socket) target when any of these holds:

- `repl-diskless-sync` is enabled (diskless is the configured default), or
- the cohort wants a **filtered** RDB (`req & REPLICA_REQ_RDB_MASK`, e.g.
  functions-only), which the full on-disk file is not, or
- the cohort needs an **older RDB version** than the latest one written to disk, or
- the cohort's negotiated **wire codec** differs from the on-disk `rdbcompression`
  codec.

A cohort that is *not* EOF-capable cannot be diverted to diskless at all: it can
only take a disk-based sync in the on-disk codec and latest version, so it must
match, and a replica that cannot is rejected during the handshake.

---

## 5. Streaming compression

Full-sync RDB payloads and command-stream bytes can be compressed with a
**whole-stream** codec.

### 5.1 Two independent settings

Two configuration options control two *different* things, deliberately decoupled:

- **`rdbcompression`** – the format of the **on-disk** `dump.rdb` file:
  `no` (uncompressed), `yes`/`lzf` (legacy per-string LZF), `lz4`, `zstd`.
- **`repl-compression`** – the format used on the **replication wire**:
  `no`, `yes` (= `lz4`), `lz4`, `zstd`.

A primary can keep `rdbcompression zstd` on disk while speaking `repl-compression
lz4` (or uncompressed) to a given replica, and vice versa. Neither dictates the
other. (`zstd` requires a build with Zstandard support.)

### 5.2 Whole-stream framing: the VCS envelope

A whole-stream-compressed payload is prefixed with a 7-byte **VCS** envelope
(`src/compression_stream.h`):

```
[0..2] "VCS" magic
[3]    version
[4]    codec id      (VCS_CODEC_LZ4 = 0x01, VCS_CODEC_ZSTD = 0x02)
[5]    reserved (0)
[6]    stream kind   (VCS_STREAM_RDB = 0x01, VCS_STREAM_REPL = 0x02)
```

A stream is self-describing: a reader inspects the leading bytes and either
decodes a VCS frame or treats the input as uncompressed. `vcsCodecToAlgo()` maps a
codec id back to its `compressionAlgo`. A native RDB begins with the `VALKEY`
magic (legacy `REDIS`), so the `VCS` prefix unambiguously marks a compressed
stream.

### 5.3 Codec negotiation

Three codecs can appear on the wire — uncompressed, LZ4, and Zstd — with a strength
order (Zstd > LZ4 > uncompressed). (The legacy per-string LZF of `rdbcompression
yes` is not a whole-stream codec and counts as uncompressed on the wire.) The primary
chooses one per replica:

- The replica advertises what it can decode via `REPLCONF capa` (§2.2), and the
  advertisement is cumulative rather than one-hot: a Zstd-capable replica
  advertises **both** `zstd` and `lz4`, so a primary configured for LZ4 can still
  serve it with LZ4 instead of dropping to uncompressed.
- The primary picks the **strongest codec both sides support**, capped at its
  configured `repl-compression`: it starts from that ceiling and walks down the
  strength order until it finds one the replica accepts, using uncompressed only if
  none match.

Negotiated wire codec, by each side's `repl-compression` (rows: primary,
columns: replica; `yes` is an alias for `lz4`):

| primary ＼ replica | `no`         | `lz4`        | `zstd`       |
|--------------------|--------------|--------------|--------------|
| **`no`**           | uncompressed | uncompressed | uncompressed |
| **`lz4`**          | uncompressed | lz4          | lz4          |
| **`zstd`**         | uncompressed | lz4          | zstd         |

### 5.4 Full-sync codec selection

Which codec a replica actually receives for its full sync depends on its transport
capability:

- An **EOF-capable** replica can receive a freshly generated diskless stream, so
  it gets the negotiated wire codec from §5.3.
- A **non-EOF** replica can only receive a disk-based sync — the on-disk
  `dump.rdb` bytes — so it instead gets the on-disk `rdbcompression` codec if it
  can decode it; one that cannot is rejected during the handshake. (`rdbcompression
  yes`/`lzf` counts as uncompressed here, being per-string rather than whole-stream.)

This single per-replica choice (`replSelectFullSyncCompression()` in
`src/replication.c`) is what cohort grouping and the disk-vs-diskless decision in
§4.5 consult, so every path agrees on the codec a given replica gets.

### 5.5 Replica-side transcoding

The wire codec a replica *receives* need not match the codec it wants *on disk*.
On the disk-receive path (§4.4) the replica rewrites the incoming stream into its
own `rdbcompression` codec as it writes `dump.rdb`, using the streaming transcoder
(`src/rdb_transcoder.{c,h}`):

| Received (wire) | Wanted (on disk) | Action |
|-----------------|------------------|--------|
| X | X | **verbatim** – copy bytes unchanged |
| compressed | uncompressed | **decode** |
| uncompressed | compressed | **encode** |
| codec A | codec B | **recode** (decode then re-encode) |

The transcoder classifies the source from its leading bytes (VCS envelope vs
uncompressed); `verbatim` is taken whenever the source codec equals the target, for
**any** codec. When decoding to an uncompressed target with checksums on, it rebuilds
the RDB's CRC64 trailer so the file stays verifiable like a native save. It is
decoupled from the sink (an emit callback) and from wire framing (the caller
strips any EOF marker first).

### 5.6 Enabling and renegotiation

After negotiation the primary enables the codec on the replica client via
`replicaEnableCompressionIfNegotiated()` (stored on
`replica->repl_data->repl_compression`). Because the command stream must not
switch codecs mid-flight, compression is enabled at well-defined points (e.g.
right after `+CONTINUE`, before backlog bytes are queued). If configuration
changes so the live transport no longer matches what the current config would
negotiate, `replicaCompressionNeedsRenegotiation()` detects the mismatch so the
link can be re-established with the new codec.

---

## 6. Code reference

| Concern | Symbol / file |
|---|---|
| REPLCONF handling | `replconfCommand()` — `replication.c` |
| Full-sync grouping filter | `replication.c` (disk), `rdb.c` (diskless) |
| RDB version per replica | `replicaRdbVersion()` — `replication.c` |
| Wire ceiling from config | `replCompressionAlgorithm()` — `replication.c` |
| On-disk whole-stream codec | `rdbStreamCompressionAlgorithm()` — `replication.c` |
| Capability advertisement | `appendReplCompressionCapabilities()` — `replication.c` |
| Does replica accept a codec | `replicaAcceptsCompressionAlgorithm()` — `replication.c` |
| Strongest mutually-supported codec | `replicaNegotiatedCompressionAlgorithm()` — `replication.c` |
| Per-replica full-sync codec | `replSelectFullSyncCompression()` — `replication.c` |
| Enable / renegotiate on client | `replicaEnableCompressionIfNegotiated()`, `replicaCompressionNeedsRenegotiation()` — `replication.c` |
| Whole-stream framing | VCS envelope, `vcsCodecToAlgo()` — `compression_stream.{c,h}` |
| Replica-side RDB rewrite | `rdbTranscoder*` — `rdb_transcoder.{c,h}` |
| Capability / requirement bits | `REPLICA_CAPA_*`, `REPLICA_REQ_*` — `server.h` |
| Diskless EOF marker | `RDB_EOF_MARK_SIZE` (40) — `server.h`, `rdb.c` |

---

## 7. Configuration reference

| Parameter | Controls |
|---|---|
| `repl-diskless-sync` | Primary uses diskless (socket) full sync instead of disk-based. |
| `repl-diskless-sync-delay` | Seconds to wait before a diskless fork so replicas can batch into one cohort. |
| `repl-diskless-sync-max-replicas` | Max replicas per diskless round (`0` = unlimited). |
| `repl-diskless-load` | Replica load path: `disabled` (disk-receive), `swapdb`, `on-empty-db`. |
| `dual-channel-replication-enabled` | Enable dual-channel sync (requires `repl-diskless-sync`). |
| `rdbcompression` | On-disk `dump.rdb` codec: `no`, `yes`/`lzf`, `lz4`, `zstd`. |
| `repl-compression` | Replication wire codec ceiling: `no`, `yes` (=`lz4`), `lz4`, `zstd`. |
| `rdbchecksum` | Whether RDBs carry a CRC64 trailer. |
| `repl-backlog-size` | Size of the in-memory replication backlog. |
| `repl-backlog-ttl` | How long to keep the backlog after the last replica disconnects. |
| `repl-timeout` | Replication link timeout (both directions). |
| `repl-ping-replica-period` | How often the primary PINGs replicas. |
| `repl-disable-tcp-nodelay` | Trade latency for bandwidth on the replication socket. |
| `replica-read-only` | Reject writes on the replica. |
| `replica-serve-stale-data` | Serve reads while disconnected from the primary. |
| `min-replicas-to-write` / `min-replicas-max-lag` | Refuse writes unless N replicas are within a lag bound. |

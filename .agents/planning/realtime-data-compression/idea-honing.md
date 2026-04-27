# Idea Honing — Requirements Clarification

_Interactive Q&A to refine the rough idea into concrete requirements. One question at a time, answers recorded verbatim (or summarised with the user's final decision) as we go._

---

## Q1. Dictionary-version cap

How many zstd dictionary versions should the server keep live at the same time?

Context: ZSTD allows many DDicts to coexist (one per dictID) and pick the right one per frame. In steady state we usually have 2 (the **active** compression dict + a **retiring** decompression-only dict). But if retraining is faster than old frames get rewritten/expired, we can accumulate more. Each extra dict costs ~100 KB raw + ~100 KB digested DDict = ~200 KB.

Options:

1. **Hard cap at 2** (active + retiring). Before retraining a third time, we either force-sweep all keys referencing the oldest dict or refuse to retrain. Simplest story, but an aggressive retrain cadence can stall.
2. **Configurable small cap, default 4** (recommended). Covers the common case; admins can bump it if needed; forces action when hit.
3. **No cap**. Flexible but unbounded memory in adversarial / misconfigured workloads. Not recommended.

**Answer:** Option 2 — **configurable small cap, default 4**.

- New config: `compression-dict-max-versions` (int, default `4`, min `2`, `MODIFIABLE_CONFIG`).
- Behavior when the cap is reached and a new dict is about to be promoted:
  - **Block retraining** and emit a `LL_WARNING` server-log entry (see Q10c). No keyspace notification — compression is an infrastructure event, not a keyspace event.
  - The operator can unblock by running `COMPRESSION SWEEP` to force-rewrite any keys still referencing the oldest retiring dict (which allows it to retire), or by raising the cap.
- Old dicts are refcounted; they retire automatically when the last compressed frame referencing them is rewritten/overwritten/expired.

---

## Q2. Preshared / imported dictionary support in v1

Should the first release support importing a **preshared dictionary** — i.e., a dictionary bytes blob produced out-of-band (offline training, copied from another server, or built by a fleet-management tool) and injected via an admin command, bypassing the in-server training cycle?

**Context.** Two orthogonal ways to get a dictionary:

- **Self-training** (always required): server samples live values, trains periodically. This is the core mechanism and has to exist.
- **Preshared import** (optional extra): operator provides pre-built dictionary bytes.

**Why preshared matters for some operators:**

- **Deterministic fleet rollouts.** Large fleets want all nodes to compress with the same dictionary so that replicas, cluster migrations, and fleet-wide tooling all agree from day one. Self-training per node produces slightly different dicts even on identical data.
- **Cold start.** A fresh node with no data takes `compression-dict-training-samples` writes (default 10000) before it can train. A preshared dict makes compression effective immediately.
- **Expert tuning.** Admins can train offline on a representative dataset with tuned parameters (`ZDICT_optimizeTrainFromBuffer_fastCover`) and ship the result.

**Scope of the admin surface:**

- `COMPRESSION DICT IMPORT <base64-bytes>` — install as the new active dict.
- `COMPRESSION DICT EXPORT <id>` — retrieve dict bytes for backup / propagation (trivial once we have the registry; essentially free to add).

**Options:**

1. **Include in v1** (my recommendation). The code cost is small once the dictionary registry exists — it's just another source feeding the same promotion path. Avoids designing a v2 migration story later.
2. **Defer to v2.** Simpler v1 surface. Accepts that early adopters have no way to sync dicts across a cluster.

**Answer:** Option 1 — **include preshared dictionary import/export in v1**.

- `COMPRESSION DICT IMPORT <base64-bytes>` — install as a new dictionary version. Goes through the same promotion path as a trained dictionary (atomic publish, existing dict becomes retiring).
- `COMPRESSION DICT EXPORT <dictID>` — return raw dictionary bytes (base64) for backup / propagation.
- Both require admin ACL (`@admin`).
- Imported dictionaries are subject to the same cap (`compression-dict-max-versions`) and retirement rules as trained ones.

---

## Q3. RDB load behavior when the compression feature is disabled

If a Valkey server has `compression-enabled no` and is asked to load an RDB that contains compressed values (emitted by a prior run with the feature enabled), what should happen?

**Context.** The on-disk RDB will contain:
- One or more `RDB_OPCODE_AUX` entries carrying dictionary bytes (`"compression-dict-<N>"`).
- One or more values encoded with the new `RDB_ENC_ZSTDDICT` marker.

The server can either:

1. **Fail loud**: refuse to load, log a clear error pointing the operator at `compression-enabled` or a migration tool. Simple, safe, but operationally harsh (an operator who "just wanted to turn the feature off" now has a server that won't start).
2. **Transparently decompress on load** (my recommendation): when the loader sees `RDB_ENC_ZSTDDICT`, it rebuilds the DDict from the AUX entries that preceded it (as it would normally), decompresses each value, and stores it uncompressed in memory. The dictionaries stay in the registry but no new writes are compressed. Operator can later re-enable compression without data loss.
3. **Hybrid**: load compressed values compressed even though the feature is disabled, so that if the operator re-enables compression they don't pay a re-compression sweep. But new writes are never compressed. Most flexible, slightly more code.

**Additional dimension: missing dictionary**. If a value references a `dictID` for which no dictionary was emitted (corrupt or truncated RDB), that's always an error regardless of which option we pick. `rdb-version-check` already has a `strict`/`relaxed` enum — we can reuse that knob if needed.

**Recommendation.** Option 2. Reasons:
- The feature is opt-in; disabling it should be cheap and reversible.
- Decompression on load is paid once, at startup. It's a lot cheaper than refusing to load.
- Keeps the "feature toggle is safe to flip" operator property we want.
- Option 3 is a nice future extension but doubles the complexity of decompression-related code paths — compressed values now have to decompress correctly without the `compression-enabled` machinery being active. Not worth it in v1.

**Answer:** Option 2 — **transparently decompress on load**.

- When the loader encounters `RDB_ENC_ZSTDDICT` while `compression-enabled no`, it rebuilds the needed `ZSTD_DDict` from the preceding AUX entries, decompresses each value inline, and stores the result uncompressed.
- Dictionaries loaded this way are discarded after load (not kept in the registry) — no new writes are compressed.
- If a compressed value references a `dictID` that was never emitted, the RDB is rejected as corrupt regardless of the `compression-enabled` setting (same rule that applies when the feature is enabled).
- Consequence: toggling `compression-enabled` off is safe and reversible — operators can turn the feature on/off without data loss at any time.

---

## Q4. Adaptive kill-switch vs. pure observability

How should the server react when compression is providing **negative or near-zero net savings** (e.g., bad dict, incompressible workload, misconfiguration)?

**Context.** The feature has non-trivial fixed overhead per instance (~20–40 MB: worker `CCtx`/`DCtx`, dict registry, candidate queue) and per-value overhead (~16 B header). If the dataset turns out to be incompressible or the dictionary has badly drifted, compression can cost more memory than it saves, plus the CPU and latency overhead.

Two philosophies:

1. **Pure observability (my recommendation).** Server reports net savings in `INFO compression` (`compression_net_saved_bytes` = uncompressed - (compressed + headers + fixed_overhead)). If it goes negative, operators see it in dashboards. Server does nothing automatic. Operator disables the feature via `CONFIG SET compression-enabled no` or `COMPRESSION DISABLE`.

2. **Adaptive kill-switch.** Server watches `net_saved_bytes` over a rolling window. If it stays below `compression-min-net-savings` (default e.g. 5% of dataset) for longer than `compression-kill-switch-window` (default e.g. 10 minutes), the server:
   - Stops new compressions (stays `compression-enabled yes` for decompression of existing frames).
   - Logs a warning.
   - Emits a keyspace-notification event (`kill-switch-tripped`).
   - Publishes the state via `INFO compression` (`compression_kill_switch:1`).
   - Operator explicitly re-arms with `COMPRESSION DISABLE` + `ENABLE`, or by restarting the server.

3. **Both (hybrid).** Ship pure observability by default. Add the kill-switch as a separate opt-in config `compression-kill-switch yes/no` (default `no`) so cautious operators can turn it on.

**Trade-offs:**

- **Option 1** is minimal, predictable, "do one thing well." Lets operators keep full control. Downside: on a large fleet, an operator has to actually look at the metric — if they don't, a misbehaving instance silently wastes resources.
- **Option 2** is defensive by default. Downside: automatic behavior in a core data path is exactly the kind of surprise that makes operators wary of features. Also adds non-trivial state machine complexity.
- **Option 3** gives operators the choice, but adds yet another knob.

**Why I recommend option 1:**
- Valkey's operator culture (inherited from Redis) strongly prefers "the server does what you tell it, and tells you what's happening." Auto-disabling a feature the operator explicitly enabled is a surprise.
- The metric is cheap and well-defined; dashboards will catch the regression.
- We can always add option 2 as a non-breaking addition later (`compression-kill-switch no` default) without changing semantics for existing users.

**Answer:** Option 1 — **pure observability; no adaptive kill-switch in v1**.

- `INFO compression` exposes `compression_net_saved_bytes` (= uncompressed - compressed - per-value headers - fixed overhead) alongside other metrics. If it goes negative, it shows up in dashboards.
- The server **never auto-disables** the feature. Operators retain full control and disable via `CONFIG SET compression-enabled no` or `COMPRESSION DISABLE`.
- Leaves room for an opt-in kill-switch as a non-breaking v2 addition if operational experience argues for it.

---

## Q5. Master switch on/off model at runtime

How does the admin turn compression on/off at runtime, and what exactly happens to existing data when the switch flips?

**Context.** Two separate concerns:

1. **Surface**: do we have a dedicated `COMPRESSION ENABLE`/`DISABLE` command, only `CONFIG SET compression-enabled`, or both?
2. **Semantics** when flipping:
   - **Enable `no → yes`**: the background compressor starts. Existing uncompressed values get compressed opportunistically by the sweeper and by natural write activity. Simple and incremental.
   - **Disable `yes → no`**: the interesting case. Options:
     - **(a) Decompression-only mode**: stop compressing new values, but keep the dictionary registry alive and keep decompressing existing compressed values on read. Already-compressed values stay compressed in memory. Cheapest, transparent to clients.
     - **(b) Immediate eager decompress**: scan the keyspace and decompress everything synchronously. Safe final state (no compressed frames remain) but can take a long time and doubles peak memory.
     - **(c) Background sweep decompress**: like (a) but also kicks off a background job to decompress-in-place over time, so eventually the dictionary registry is unreferenced and can be dropped. Middle ground.

**What I recommend:**

- **Surface**: both. `CONFIG SET compression-enabled yes|no` is the primary, Valkey-idiomatic interface (persists via `CONFIG REWRITE`, covered by all existing tooling). `COMPRESSION ENABLE`/`DISABLE` is a convenience alias that additionally logs the event and emits a keyspace notification — nicer for runbooks and audit trails.
- **Enable semantics**: simple and opportunistic. The sweeper wakes up on next cron tick and starts walking kvstore shards. Users can run `COMPRESSION SWEEP` to kick it manually if they want immediate action.
- **Disable semantics**: **option (c) — background sweep decompress, with the sweep trigger being explicit (`COMPRESSION SWEEP`) rather than automatic.** Rationale:
  - Immediately on `DISABLE`: new writes stop being compressed. Existing compressed values keep working (decompress on read). The dictionary registry stays alive until explicitly swept.
  - Operator decides when to force-decompress everything: either never (option a is fine for "I just don't want new things compressed") or via `COMPRESSION SWEEP direction=decompress` (eventually drops all compressed frames and dictionaries).
  - Never auto-doubles peak memory — operator owns the timing.

- **Consequence**: there's a small third state, "compression-enabled yes but no active dictionary yet" (fresh server, not enough samples to train, or admin has dropped all dicts). In that state, decompression still works for anything that's compressed; new writes stay uncompressed until the first dictionary is published. This is the same code path as "compression-enabled no", just with a different reason.

**Options for your decision:**

1. **Both surfaces** (CONFIG + COMPRESSION ENABLE/DISABLE), **disable = keep decompressing, don't sweep unless asked** (my recommendation).
2. **CONFIG only**, simpler surface.
3. **CONFIG only, and disable = eager full-keyspace decompress** (option b above).
4. Some other combination — tell me.

**Answer:** Option 1 — **both surfaces; disable = keep decompressing; sweep only on explicit request**.

- **Surface (both):**
  - `CONFIG SET compression-enabled yes|no` is the primary, Valkey-idiomatic interface. Persists via `CONFIG REWRITE`. Covered by existing tooling.
  - `COMPRESSION ENABLE` / `COMPRESSION DISABLE` is a convenience alias. Identical effect on the toggle, plus writes a `LL_NOTICE` server-log entry (e.g., `"Compression feature enabled by operator"`) for runbooks and audit trails. Note: **no keyspace notification** is emitted — compression is a server-global infrastructure event, not a keyspace event (see Q10c).
- **`no → yes` semantics:** background sweeper starts on the next cron tick; values get compressed opportunistically by new writes and by the sweeper. `COMPRESSION SWEEP` can be invoked to request immediate action.
- **`yes → no` semantics (decompression-only + explicit sweep):**
  - Immediately: new writes stop being compressed.
  - Existing compressed values continue to work — decompress on read, dictionary registry stays alive.
  - No automatic sweep. Operator explicitly runs `COMPRESSION SWEEP direction=decompress` if they want all frames decompressed and the dictionary registry dropped.
  - Peak memory is never doubled automatically.
- **Third-state acknowledgement:** "compression-enabled yes, but no active dictionary yet" (fresh server pre-training, or all dicts dropped) behaves identically to "disabled" for new writes. Documented as expected behavior.

---

## Q6. Eligibility filter — which values get compressed (in v1, STRING-only)

Within `OBJ_STRING`, not every value is a good compression candidate. We need a clear, cheap eligibility filter that runs before a key is queued for background compression.

**Baseline filter (non-controversial, should be in v1):**

- `type == OBJ_STRING`.
- `encoding ∈ {OBJ_ENCODING_RAW, OBJ_ENCODING_EMBSTR}`. Skip `OBJ_ENCODING_INT` (already memory-optimal — a packed long).
- `refcount != OBJ_SHARED_REFCOUNT`. Shared RESP constants are never in a db anyway, but we assert it.
- `sdslen(val) >= compression-min-value-size` (default `256 B`). Values below this cannot recoup the ~16 B header plus dict-registry amortized cost. Research §6 of the previous message.
- **Skip hot items.**
  - LFU mode: skip if `LFU_freq >= compression-lfu-threshold` (default `5`, on the 0–255 log scale).
  - LRU mode: skip if `idle_seconds < compression-lru-idle-seconds` (default `60 s`).
  - `noeviction` / other policies with no useful counter: fall back to a **time-based "settle" window** — a key is eligible N seconds after its last write. Cheapest implementation: stamp the write time in a small per-candidate queue entry; we already need that queue.
- **Skip post-compression if we don't actually save.** After the worker compresses, on the main thread we compare `compressed_size + header >= uncompressed_size * (1 - compression-min-savings-ratio)` (default `10%`). If true: discard the compressed form, leave the value uncompressed, and **mark the key as "don't re-try"** for a cooldown period (e.g., `compression-retry-interval` = 1 h) so we don't waste CPU on incompressible keys.

**Open sub-questions for you:**

**(Q6a)** In `noeviction` / non-LRU / non-LFU mode, is the time-based settle window acceptable, or should we **disable compression entirely** in those policies?
- Time-based settle: simpler to reason about, always works. My recommendation.
- Disable: "compression requires LRU or LFU to be useful." Cleaner boundary, but surprises operators who use `noeviction` with a small dataset that would still benefit.

**(Q6b)** Should the "don't re-try incompressible keys" cooldown exist in v1?
- Include: prevents CPU waste on truly incompressible keys. My recommendation.
- Skip: simpler v1; we just recompress them on the next sweep and discard again. Wastes CPU but behavior is simpler.

**(Q6c)** Any value type beyond STRING you want to keep on the table as explicitly **in-scope for v1**? The rough idea says STRING-first; research agrees; but e.g. HASH field values are structurally similar. Confirming: **v1 is STRING-only, HASH/SET/ZSET/LIST/STREAM are v2+**. Right?

**Answer:** All three sub-decisions go with the recommendation.

- **(Q6a)** In `noeviction` / non-LRU / non-LFU policies, use a **time-based settle window**. A key is eligible for compression `compression-settle-seconds` (default `60 s`) after its last write. Implemented by stamping write time on the candidate-queue entry.
- **(Q6b)** Include the **"don't re-try incompressible keys"** cooldown in v1. When the post-compression savings check fails, stamp the key with `compression-retry-interval` (default `1 h`) so sweepers skip it. Counter tracked in `INFO compression` (`compression_skipped_incompressible`).
- **(Q6c)** **v1 scope is STRING only** (`type == OBJ_STRING`, encodings `RAW`/`EMBSTR`). HASH/SET/ZSET/LIST/STREAM are explicit v2+ work. The encoding-tag approach remains structurally extensible so future types can piggyback on the same infrastructure.

**Consolidated eligibility predicate (for the design doc):**

```
eligible(obj) ⇔
    obj->type == OBJ_STRING
 && obj->encoding ∈ {RAW, EMBSTR}
 && obj->refcount != SHARED
 && sdslen(val) >= compression-min-value-size
 && last_retry_failure_age(obj) >= compression-retry-interval
 && (
        (lfu_mode  && lfu_freq(obj)   <  compression-lfu-threshold) ||
        (lru_mode  && lru_idle(obj)   >= compression-lru-idle-seconds) ||
        (other     && write_age(obj)  >= compression-settle-seconds)
    )
```

**Post-compression net-savings guard (main thread, after worker returns):**

```
compressed_size + header_size >= uncompressed_size * (1 - compression-min-savings-ratio)
  → discard compressed form, mark key with retry cooldown.
```

Associated configs (all MODIFIABLE_CONFIG):

| Name | Default |
|---|---|
| `compression-min-value-size` | `256` bytes |
| `compression-lfu-threshold` | `5` |
| `compression-lru-idle-seconds` | `60` |
| `compression-settle-seconds` | `60` |
| `compression-min-savings-ratio` | `10%` |
| `compression-retry-interval` | `3600` seconds |

---

## Q7. Decompression policy — sync on main thread vs. async on worker

When a client reads a compressed key, should decompression run **synchronously on the main thread** or be **offloaded to a compression worker thread**?

**Context.** There are two legitimate patterns, and the POC used both:

- **Sync main-thread decompress**: simple. One extra branch + `ZSTD_decompress_usingDDict` on the main thread, on the command hot path. Cost scales with value size (zstd decompression is ~1 GB/s per core for level 3 with a dict, so ~1 µs/KB).
- **Async worker decompress**: main thread enqueues a job on the compression-worker SPMC inbox, the command / client yields back to the event loop, worker decompresses, result comes back via the MPSC outbox, main thread resumes the command. The POC called this "block the client."

Research already established **hard constraints**:

- **Scripts (`EVAL`/`FCALL`) must use sync.** Worker offload would break scripting atomicity (see `research/scripting-and-transactions.md`).
- **Transactions (`EXEC`) must use sync.** Same reasoning.
- **Replication/AOF feed must use sync.** The feed runs on the main thread; blocking-client semantics don't apply there.

So async is only ever an option for **top-level client commands outside `MULTI`/`EVAL`**.

**The real question for v1:**

1. **Always sync, no worker decompression path.** Much simpler — one decompression path only, no client-yielding, no round-trip logic. For 256 B–8 KB values (the dominant size in fleet data) main-thread decompression at ~1 µs/KB is a few microseconds, comparable to existing command overhead. The POC's perf numbers showed the round-trip itself is a major source of latency tax; sync avoids it entirely.
2. **Sync for small, async for large** (what the POC did). Threshold like `compression-async-decompress-threshold` (e.g., `4 KB`): values below that decompress on the main thread; values above that go through a worker with client yielding. Pays for itself only when the average value is big enough that main-thread decompression would block other clients noticeably.
3. **Always async** (for non-script, non-exec commands). Never blocks the main thread on decompression CPU. But pays the round-trip even for tiny values where it's net-negative. Not recommended.

**My recommendation: option 1 for v1.**

Reasons:

- **Simpler implementation** — only the background compression path needs workers. Decompression is always a synchronous helper.
- **POC data shows** the main cost of the async path was the block-client round-trip, not the compression CPU. Sync sidesteps that cost.
- **Multi-key commands (`MGET`, `MSET`) can still parallelize** if we ever need it, by fanning out _compression_ jobs; decompression on read is per-value and fast enough to do inline.
- **Workers are still needed** for (a) background compression sweep, (b) multi-key compression, (c) future extension points. So the pool is not wasted.
- Option 2 can be added in v2 non-breakingly (just introduce the threshold config, default `0` = disabled).

**Possible objection:** if workloads routinely store 100 KB+ values and read them often, main-thread decompression at ~100 µs per read could matter. Fair point. But: (a) such values compress well without a dictionary anyway (the dictionary is most valuable for small values — so large values may not even be in scope if we tune `compression-min-value-size` the other way), and (b) anyone with that workload can be directed to v2's opt-in async path when it lands.

**Options for your decision:**

1. **Always sync decompression** (v1), worker pool used only for background compression / training / multi-key compression. (Recommended.)
2. **Sync below threshold, async above**, with the threshold as a new config. More POC-like.
3. Something else — describe it.

**Answer:** Option 1 — **always sync decompression in v1**, plus introduce a new upper-bound config `compression-max-value-size` so operators can exclude values that are too large to decompress comfortably on the main thread.

- **Decompression path:** always synchronous on the main thread, regardless of client context (top-level command, `MULTI`/`EXEC`, `EVAL`/`FCALL`, replication/AOF feed). One code path, no client-yielding, no round-trip logic.
- **Worker pool usage (v1):** background compression sweep, dictionary training (via `bio`), and multi-key compression fan-out. **Not** used for decompression.
- **Why sync wins for v1 workloads:**
  - Common value size (256 B – 8 KB) decompresses in 1–8 µs on the main thread; worker round-trip alone is ~5–15 µs. Offload is a net loss below ~8 KB.
  - Scripts / `EXEC` / replication feed mandate sync anyway — async would be an *additional* code path, not a replacement.
  - Preserves per-connection arrival-order fairness.
  - Avoids making the worker pool serve three customers (sweep, multi-key compress, decompress) which would otherwise require a priority queue.
- **New upper-bound config to protect the main thread from pathologically large values:**

  | Name | Default | Meaning |
  |---|---|---|
  | `compression-max-value-size` | `1048576` bytes (1 MiB) | Values larger than this are **not eligible** for compression. They stay uncompressed in memory, so decompression on read is never paid for them. `0` = no upper bound. `MODIFIABLE_CONFIG`. |

  Added to the eligibility predicate in Q6:
  ```
  eligible(obj) ⇔ … && (compression-max-value-size == 0
                        || sdslen(val) <= compression-max-value-size)
  ```
  Rationale: the largest values both (a) compress well *without* a dictionary so the feature's unique benefit is smaller, and (b) incur the largest main-thread decompression stall. Capping them is the cheapest way to bound worst-case sync decompression cost.

- **v2 extension points (non-breaking):**
  - Add `compression-async-decompress-threshold` (default `0` = disabled) → sync below, async above. No change to semantics for existing users.
  - Add a coexistence (decompressed-view) cache for recently-accessed hot compressed keys.

- **Updated sync-mandatory predicate (for the design doc):** decompression is **always** synchronous in v1; no code path offloads reads to workers.

---

## Q8. Worker pool sizing and CPU budget

We've committed to a dedicated compression worker pool (separate from `io-threads`) for background compression, dictionary training fan-out, and multi-key compression. How should the pool be sized, and how do we bound its CPU cost?

**Context.** The pool competes with the main thread, `io-threads`, background persistence children (RDB fork, AOF rewrite), `bio`, and any other tenant on the same box for CPU. On a typical Valkey deployment (1 container, N vCPUs, one `valkey-server` instance), every core the compression pool uses is a core *not* available to the main thread or to the RDB child during a save.

Two parameters in tension:

1. **Pool size** — how many threads.
2. **Pacing** — how aggressively each thread runs.

### Pool size options

1. **Fixed default `0` (auto).** Server picks a number at startup: e.g., `max(1, floor(num_online_cpus / 4))`, capped at some reasonable value like 4. Operators can override with `compression-threads`.
2. **Fixed default `1`.** One dedicated worker always, regardless of machine size. Minimal footprint, predictable. (Recommended.)
3. **Fixed default `2`.** Slightly more headroom for concurrent training + sweep on medium boxes.
4. **No default pool — disabled unless operator opts in.**

### Pacing options

A. **No explicit pacing.** Workers run flat-out when there's work.
B. **Sweep-only pacing** (recommended). A soft cap on sweep throughput: `compression-sweep-max-cpu-pct` (default `25%`), implemented as "work for X ms, sleep for Y ms." Does not apply to on-demand jobs (training, multi-key compress).
C. **Global per-thread quota.** Every compression thread capped at some CPU percentage.

**Answer:** Pool default `1`, sweep pacing at `25%`, plus a dedicated `compression_cpulist`.

- **Pool size:** `compression-threads` (int, default `1`, range `0..16`, `MODIFIABLE_CONFIG`). `0` explicitly disables the pool, making the feature a no-op (useful for pause without touching `compression-enabled`).
- **Sweep pacing:** `compression-sweep-max-cpu-pct` (int, default `25`, range `1..100`, `MODIFIABLE_CONFIG`). Applies **only** to the background sweep. Implementation: each sweep tick processes a batch, measures wall time, then sleeps `(100/target_pct - 1) × batch_ms` before the next tick. On-demand jobs (training, multi-key compression) are not paced — they are naturally bounded by arrival rate.
- **CPU isolation:** `compression_cpulist` (string, default empty). Follows the existing `bio_cpulist` / `aof_rewrite_cpulist` precedent in `src/server.c`. If empty, workers inherit the system default. Workers MUST NOT share affinity with the main thread when `server_cpulist` is set.
- **Hard invariants:**
  - Compression workers never touch `robj` directly — they consume/produce flat byte buffers; main thread owns the `robj` mutation.
  - Worker pool is separate from `io-threads`; the two are sized independently.

---

## Q9. Dictionary training — trigger, sampling, and algorithm

**Trigger:**
- **First training** fires once the server has seen `compression-dict-first-training-keys-count` eligible `OBJ_STRING` keys (default `10000`). Counted on the write path via a cheap counter; no per-key copy.
- **Steady-state retraining** fires on **drift**: when the live compression ratio regresses to less than `compression-dict-drift-ratio` × `post_training_ratio` (default drift ratio `70%`). Live ratio is maintained as a rolling average in `INFO compression`.
- **Optional time-based retraining** is exposed via `compression-dict-refresh-interval` (default `0` = disabled). Operators with drifting workloads can opt in; v1 otherwise relies on drift + manual `COMPRESSION TRAIN`.
- **Manual retraining:** `COMPRESSION TRAIN` forces an immediate training job regardless of triggers.

**Sampling: keyspace scan, not reservoir.**
- Rationale: a reservoir of ~10000 samples at ~1 KB each costs 10–16 MiB of *extra* memory holding copies of values that already exist in the keyspace. For a memory-saving feature, that overhead is unacceptable (1–2% of a 1 GB instance before any compression win).
- The training job (on `bio`) walks kvstore shards in random shard order, reuses the active-expiry/defrag iteration pattern, and collects sample pointers via `incrRefCount`. Samples are passed directly to `ZDICT_trainFromBuffer` as pointers into sds bodies — zero copies, zero sustained overhead.
- Held references are released (`decrRefCount`) after training completes.
- Scan must read with `LOOKUP_NOTOUCH` semantics — training reads do not touch LRU/LFU.
- Known bias: long-lived keys are over-represented in samples. Documented as expected behavior — we *want* the dictionary to compress data that sticks around.
- If the keyspace has fewer than `first-training-keys-count` eligible values when training is requested, training aborts with a logged warning and retries on the next trigger.

**Training location:** new `bio` job type `BIO_COMPRESSION_TRAIN`. Fits the `bio` model (long-running, infrequent, one-at-a-time). Does not occupy a compression worker.

**Algorithm:** `ZDICT_trainFromBuffer` with `compression-dict-size` (default `102400` bytes) as the only exposed tuning parameter. Advanced trainer parameters (`ZDICT_optimizeTrainFromBuffer_fastCover`) are a v2 extension point.

**Associated configs** (all `MODIFIABLE_CONFIG`):

| Name | Default | Meaning |
|---|---|---|
| `compression-dict-first-training-keys-count` | `10000` | Eligible-key count that triggers the first training. |
| `compression-dict-size` | `102400` | Target size passed to the zstd trainer. |
| `compression-dict-drift-ratio` | `70` (percent) | Retrain trigger: live ratio < drift-ratio × post-training ratio. |
| `compression-dict-refresh-interval` | `0` (disabled) | Optional periodic retrain cadence, in seconds. `0` disables time-based retraining. |

**Post-training promotion** follows the registry path from Q1 (atomic pointer swap, previous active becomes retiring, subject to `compression-dict-max-versions` cap).

---

## Q10. Observability — INFO section, latency events, keyspace notifications

### Q10a. `INFO compression` section fields

**Final set** (all fields cheap to compute; rolling windows maintained as existing `stats.c`-style EMAs):

| Field | Meaning |
|---|---|
| `compression_enabled` | `0` / `1` — current master switch. |
| `compression_state` | `idle` / `training` / `active` / `disabled`. Captures the "enabled but no dict yet" third state from Q5. |
| `compression_active_dict_id` | dictID currently used for new compressions, or `0` if none. |
| `compression_dict_age_seconds` | Age of the active dict (since promotion). Helps operators decide to force-retrain. |
| `compression_known_dicts` | Count of dicts in the registry (active + retiring). |
| `compression_dict_cap_reached` | `0` / `1` — last retrain was blocked by `compression-dict-max-versions`. |
| `compression_compressed_objects` | Number of `robj`s currently holding a compressed frame. |
| `compression_total_uncompressed_bytes` | Sum of uncompressed payload bytes across all compressed objects. |
| `compression_total_compressed_bytes` | Sum of compressed frame bytes (including per-value header). |
| `compression_ratio` | `compressed / uncompressed`. All-time. |
| `compression_live_ratio_10m` | Rolling ratio over the last 10 min. Drift trigger input. |
| `compression_net_saved_bytes` | `uncompressed - (compressed + headers + fixed_overhead)`. Negative = feature costs more than it saves (Q4: no auto kill-switch; operator acts). |
| `compression_candidates_pending` | Jobs queued for the worker pool. |
| `compression_compressions_per_sec` | Rolling rate, 1 s window. |
| `compression_decompressions_per_sec` | Same, for reads. |
| `compression_skipped_incompressible` | Count of keys on the retry-cooldown list (Q6b). |
| `compression_training_last_duration_ms` | Duration of most recent training job. |
| `compression_training_last_sample_count` | Samples used in most recent training. |
| `compression_errors_total` | zstd errors, missing-dict-on-load errors, etc. |

**Dropped from the initial proposal** (keep the section tight):
- `compression_live_ratio_10s` — 10m window is sufficient.
- `compression_worker_utilization_pct` — expensive to measure accurately; not worth the cost.

### Q10b. Latency monitor events

- `compress-sync` — sampled per inline compression (rare).
- `decompress-sync` — sampled per inline decompression (per read of a compressed key).
- `compression-train` — sampled per training job.

All three use the **existing** `latency-monitor-threshold` floor — events only fire when `duration_ms >= threshold`. No new config needed. On a typical `latency-monitor-threshold 100`, 1–8 µs decompressions never sample; only outliers do.

Accessible via `LATENCY HISTORY compress-sync` / `decompress-sync` / `compression-train`.

### Q10c. Keyspace notifications — **dropped**

**Decision: no new keyspace-notification class or events for the compression feature.**

Rationale (settled by explicit discussion):
- Keyspace notifications (`NOTIFY_*` in `src/notify.c`) are designed for **logical keyspace changes** — events that are meaningful to application clients reacting to data state (set, delete, expire, evict, modify).
- Compression is an **infrastructure-level optimization** — no key's logical value changes when a dict is promoted, a sweep runs, or a training job completes. Application clients have no reason to subscribe.
- The channel scheme (`__keyevent@<db>__:<event>`) ties events to a database; compression is server-global and db-agnostic. Using the mechanism would misrepresent the semantics.
- The real audience for these events is the **operator**, and operators already have better surfaces for operator-facing information: server logs, `INFO`, latency monitor.
- Avoiding a new `NOTIFY_COMPRESSION` bit keeps the notification character namespace untouched and removes an entire test / documentation surface from v1.

**What replaces notifications: server-log entries at appropriate levels:**

| Event | Log level | Example message |
|---|---|---|
| Master switch flipped | `LL_NOTICE` | `Compression feature enabled by operator` / `Compression feature disabled by operator` |
| Training job submitted | `LL_NOTICE` | `Compression: dictionary training started (samples target: 10000)` |
| Training completed | `LL_NOTICE` | `Compression: dictionary <id> trained in <ms> ms from <N> samples` |
| Training failed | `LL_WARNING` | `Compression: dictionary training failed: <reason>` |
| Dictionary promoted | `LL_NOTICE` | `Compression: dictionary <new_id> active; dictionary <old_id> retiring` |
| Dictionary retired (last reference released) | `LL_NOTICE` | `Compression: dictionary <id> retired and freed` |
| Dict-version cap reached | `LL_WARNING` | `Compression: dict-version cap (<N>) reached; retraining blocked. Raise compression-dict-max-versions or run COMPRESSION SWEEP.` |
| Sweep started | `LL_NOTICE` | `Compression: background sweep started (direction=<compress|decompress>)` |
| Sweep completed | `LL_NOTICE` | `Compression: background sweep completed in <ms> ms, <N> keys touched` |

Logs are the standard operator audit trail in Valkey; this integrates cleanly without inventing new semantics.

---

## Q11. Transactions and scripts — invariants

Two hard rules, lifted from `research/scripting-and-transactions.md`:

**Rule 1 — Sync-mandatory predicate.** Decompression MUST run synchronously on the main thread whenever the read is on one of:
- Inside `EVAL` / `EVALSHA` / `FCALL` / `FCALL_RO` (scripts and Functions).
- Inside a queued `MULTI` / `EXEC` transaction.
- On the replication feed (`feedReplicationBufferWithObject`).
- On the AOF writer.

v1 ships only a sync decompression path (Q7), so the rule is automatically satisfied. The rule remains as a **forward-compatibility constraint for v2**: if async decompression is later added, the same predicate must gate it.

**Rule 2 — Never call `signalModifiedKey` from the compression path.** Background compression and in-place decompression (e.g., by the sweep or by `COMPRESSION SWEEP direction=decompress`) are storage changes, not logical value changes. Calling `signalModifiedKey` would:
- Set `dirty_cas` on every `WATCH`ed key, silently aborting `EXEC` transactions of innocent clients.
- Emit spurious RESP3 client-side-caching (`CLIENT TRACKING`) invalidations.
- Trigger keyspace notifications for modifications that did not happen logically.

Enforcement:
- A single helper — `robj *objectGetUncompressedView(robj *o, sds *scratch)` — is the only function that touches compressed values on read paths. The helper never calls `signalModifiedKey`.
- Code comment on the helper and on the background compression / decompression routines explicitly forbidding `signalModifiedKey` calls.
- Tcl tests that `WATCH` a key, trigger background compression of that key, and verify `EXEC` does not abort.
- Tcl tests that subscribe to `CLIENT TRACKING` invalidations and verify no invalidations are emitted by background compression/decompression.

Both rules become explicit invariants in the design document.

---

## Q12. Replication and AOF wire format

Lifted from `research/persistence-and-replication.md` and confirmed:

- **Replication feed** (`feedReplicationBufferWithObject` in `src/replication.c`): route every `robj` through `objectGetUncompressedView(o, &scratch)` before copying bytes into the backlog. Wire stays uncompressed RESP. No replica-side awareness of compression needed. Cross-version replication unaffected.
- **AOF writer**: same helper, same contract. AOF log is uncompressed RESP.
- **`DUMP` / `RESTORE` / `MIGRATE`**: for v1, decompress before serializing the RDB chunk. Compressed-in-place migration (ship the dict prelude within the chunk) is a v2 optimization. Trade-off accepted: migration pays a decompression cost in exchange for not having to move dictionary bytes across node boundaries.
- **RDB on-disk**: compressed with the new `RDB_ENC_ZSTDDICT` marker, with dictionary bytes written as `RDB_OPCODE_AUX` entries before the first compressed value. Bump `RDB_VERSION` so pre-feature loaders refuse the file cleanly. See `research/persistence-and-replication.md` for the layout.

Implementation note: `objectGetUncompressedView()` must reuse its scratch sds buffer (thread-local or caller-owned) to avoid per-write allocator churn on the replication feed path.

---

## Q13. Debug and introspection surface

Compressed strings are `type == OBJ_STRING` with a new encoding tag (`OBJ_ENCODING_COMPRESSED`). Admin-visible surfaces are extended as follows:

- **`TYPE key`** → `string`. Unchanged.
- **`OBJECT ENCODING key`** → `compressed` for compressed values (new string; documented alongside existing `raw`/`embstr`/`int`). Precedent: Valkey already exposes implementation-detail encodings like `intset`, `listpack`, `quicklist`, `skiplist`.
- **`DEBUG OBJECT key`** for compressed values adds three fields to its debug line:
  - `dictID:<N>` — which dictionary the frame was built with.
  - `compressedlength:<N>` — bytes held on-heap by the compressed frame (excluding the per-value header; matches `MEMORY USAGE` accounting).
  - `uncompressedlength:<N>` — original payload length.

  Operators can compute per-key ratio from those two fields.
- **`MEMORY USAGE key`** → returns **compressed size** (including the per-value header and `robj` overhead). This is the only answer consistent with Valkey's memory model: eviction, `maxmemory` enforcement, and `INFO memory` all see the compressed footprint.
- **`OBJECT FREQ` / `OBJECT IDLETIME`** → unchanged. Compression must not touch `robj.lru` (already required by Q11 rule 2).

**Module API:**

- **Read paths** (`RedisModule_StringDMA` for read, `ValkeyModule_OpenKey` → value access, etc.) **decompress transparently** via `objectGetUncompressedView`. Modules never see a compressed frame. ABI is preserved; existing modules need no changes.
- **`RedisModule_StringDMA` with write intent** on a compressed value: decompress in place first (mutate the `robj` to `raw`/`embstr` encoding, free the compressed frame), then return the sds pointer. The value becomes eligible for re-compression on the next sweep tick. Simpler and safer than scratch-buffer-plus-reencode.

---

## Q14. Eviction, `MEMORY USAGE`, and `maxmemory` accounting

Eviction, `maxmemory` enforcement, and `INFO memory` all price an `robj` by its allocated footprint via `zmalloc_size` and the global `used_memory` counter. For compressed values this must be the **compressed size** (frame + per-value header + `robj` overhead), never the uncompressed size — otherwise `maxmemory` enforcement is against a number the server doesn't actually consume and the server can OOM while thinking it's under budget.

**The accounting falls out naturally from standard allocator calls**, no custom layer needed:

- On compression: main thread `zfree`s the old uncompressed sds (decrementing `used_memory` by `zmalloc_size(old_sds)`) and `zmalloc`s the compressed buffer (incrementing `used_memory` by the new size).
- On decompression (sweep `direction=decompress`, DMA-write conversion, or toggle `compression-enabled no` + explicit sweep): the reverse.
- Eviction sampler, `MEMORY USAGE`, and `INFO memory` read the same counters and thus report the compressed footprint automatically.

**Consequences and confirmations:**

- **Per-value header** is part of the compressed buffer allocation → counted automatically.
- **Fixed overhead** (CCtx/DCtx, digested dicts in the registry, candidate queue) is allocated via `zmalloc` → counted in `used_memory`. `INFO compression` additionally reports it under `compression_net_saved_bytes` so operators can reason about net savings separately.
- **`MEMORY STATS`** unchanged in v1; compressed values are part of `dataset.bytes`. A dedicated compression sub-aggregate is a v2 nice-to-have.
- **Eviction policy unaffected**: hot/cold selection reads `robj.lru` / LFU counter, which the compression path never touches (Q11 rule 2). Hot keys are still skipped for compression (Q6) and cold keys are still evicted first.
- **Rejected alternative**: returning uncompressed size from `MEMORY USAGE`. That would decouple `MEMORY USAGE` from eviction and mislead operators about `maxmemory` budgets.

---

## Q15. Testing strategy

**Guiding principle (two tiers):**
1. **Transparency** — the existing Tcl test corpus MUST pass with compression enabled exactly as it passes with compression disabled. Compression is a transparent optimization; if the existing suite doesn't stay green, transparency is broken.
2. **Feature-specific** — a rich new test surface exercises semantics the existing corpus cannot see (dict lifecycle, RDB `ZSTDDICT` layout, retry cooldown, per-key introspection, module DMA).

### 1. Transparency mode

A new test-harness flag, `--compression`, starts each test server with an **aggressive** configuration that forces the compression path on essentially every eligible value:

```
--compression-enabled yes
--compression-min-value-size 0
--compression-max-value-size 0            # no upper bound
--compression-lfu-threshold 255
--compression-lru-idle-seconds 0
--compression-settle-seconds 0
--compression-min-savings-ratio 0
--compression-retry-interval 0
--compression-dict-first-training-keys-count 10
--compression-threads 1
--compression-sweep-max-cpu-pct 100
```

Deliverables:
- `--compression` flag on `runtest`, `runtest-cluster`, `runtest-sentinel`, `runtest-moduleapi`.
- `make test-compression`, `make test-cluster-compression`, `make test-sentinel-compression`, `make test-moduleapi-compression` targets.
- CI matrix adds a `compression=on` cell for each suite, running in parallel with the baseline.
- New test tag `compression:skip` for the handful of tests that are legitimately encoding-dependent (exact `OBJECT ENCODING` assertions, exact `MEMORY USAGE` values, etc.).
- Test-author guidance added to `tests/README.md`: prefer memory-range assertions over exact values; don't hard-code encoding strings; use a helper such as `assert_string_encoding` that accepts `compressed` alongside `raw`/`embstr`.

### 2. Feature-specific Tcl tests

| File | Covers |
|---|---|
| `tests/unit/compression.tcl` | Core feature: toggle, training trigger, sweep compresses eligible values, hot-key skip, size bounds, post-compression net-savings guard, retry-cooldown counter, per-key introspection (`OBJECT ENCODING` = `compressed`, `DEBUG OBJECT` fields, `MEMORY USAGE` = compressed size). |
| `tests/unit/compression-dict.tcl` | `COMPRESSION TRAIN` force-trains; `DICT LIST/EXPORT/IMPORT/DROP`; drift detection triggers retraining; `compression-dict-max-versions` cap blocks retraining and unblocks via `COMPRESSION SWEEP`. |
| `tests/unit/compression-multi.tcl` | Q11 invariants: `WATCH` + background compression → `EXEC` does not abort; `CLIENT TRACKING` → no spurious invalidations; `EVAL` / `EXEC` on compressed keys = baseline semantics. |
| `tests/unit/compression-persistence.tcl` | RDB save/load with compressed values (active + retiring dict AUX entries); load with `compression-enabled no` decompresses on the fly (Q3); RDB with missing dict AUX = corrupt rejection; AOF log stays uncompressed; cross-version replication works (wire stays uncompressed). |
| `tests/integration/compression-replication.tcl` | Primary compresses, replica does not; toggling on primary does not disrupt replica stream. |
| `tests/unit/cluster/compression-migrate.tcl` | `MIGRATE` decompresses on source; destination receives uncompressed RDB chunk. |
| `tests/unit/compression-transparency.tcl` | Thin meta-test canary: identical result for a handful of commands (GET/SET/DEL/EXPIRE/EXISTS/TYPE/OBJECT/DEBUG OBJECT) with compression on vs. off. |

### 3. C++ gtest units

| File | Covers |
|---|---|
| `src/unit/test_compression_registry.cpp` | Dictionary registry add / lookup / promote / retire; refcount-based retirement; cap enforcement. |
| `src/unit/test_compression_eligibility.cpp` | Every branch of the Q6 `eligible(obj)` predicate. |
| `src/unit/test_compression_header.cpp` | Per-value header encode/decode round-trips; malformed-header rejection. |
| `src/unit/test_compression_sweep_pacing.cpp` | Pacing math: target CPU pct → sleep interval. |

Symbols needing mocks are added to `src/unit/wrappers.h` per Valkey's existing `ld --wrap` pattern.

### 4. Module API test

`tests/modules/compression.c` — module uses `ValkeyModule_StringDMA` and `ValkeyModule_OpenKey` on compressed keys; sees decompressed bytes transparently; write-intent DMA decompresses in place.

### 5. Performance regression harness

One scenario added to the existing `benchmark-on-label.yml` / `benchmark-release.yml` workflows:
- Workload: 80/20 GET/SET, 1 KiB values, warm cache.
- Compared: `compression-enabled no` vs. `yes` (default production config).
- Expected: ~30% lower `used_memory`, ≤20% TPS degradation (consistent with POC baseline).
- Status: **informational**, not a merge gate (matches Valkey's current perf-workflow policy).

### 6. Reply-schema and formatting

- JSON reply schemas for every new command under `src/commands/`: `COMPRESSION ENABLE|DISABLE|TRAIN|SWEEP|STATUS|HELP`, `COMPRESSION DICT LIST|EXPORT|IMPORT|DROP`. Validated by `.github/workflows/reply-schemas-linter.yml`.
- All new C sources pass `clang-format-18` (enforced by `clang-format.yml`).
- All new text passes the typos check (`.config/typos.toml`).

### 7. CI-time cost

Transparency matrix cells parallelize with baseline cells, so wall-clock CI time does not double even though CPU use does. Expected one-time tagging pass: fewer than ~20 existing tests will need `compression:skip` (identified empirically during implementation).

---

## Q16. Explicit v1 non-goals

The following are **out of scope for v1** and are explicitly deferred or left open for a future version. Writing them down prevents scope creep during implementation reviews and clarifies v2 planning.

| Area | Out of v1 | Rationale | v2 status |
|---|---|---|---|
| Non-string data types (HASH, SET, ZSET, LIST, STREAM, module types) | Yes | Q6c: STRING-first. Encoding-tag approach structurally extends later. | Planned |
| Async decompression on worker pool | Yes | Q7: sync wins at common value sizes; round-trip tax dominates otherwise. | Planned as opt-in threshold config, default `0` (disabled). |
| Decompressed-view "coexistence" cache | Yes | Q7: standalone complexity; addresses hot-large-key case only. | Planned as orthogonal extension. |
| Adaptive kill-switch | Yes | Q4: pure observability; operator keeps control. | Planned as opt-in config, non-breaking. |
| Compressed-in-place cluster `MIGRATE` | Yes | Q12: v1 decompresses before emitting RDB chunk. | Planned — ship dict prelude inside chunk. |
| Cluster-wide dictionary gossip | Yes | Each node self-trains; preshared import (Q2) handles deterministic fleet rollouts. | Conditional — only if real operational pain surfaces. |
| Advanced trainer parameters (`fastCover` tuning) | Yes | Q9d: `ZDICT_trainFromBuffer` default-tuned in v1. | Planned — expose parameters via new configs if measurement justifies. |
| Per-key "force uncompressed" pinning | Yes | v1 treats every eligible `OBJ_STRING` value uniformly. | Conditional — `OBJECT` subcommand could pin specific keys if demanded. |
| Module-provided compression backends (lz4/snappy/hardware) | Yes | v1 hard-codes zstd. No plugin surface. | Conditional — only if use case emerges. |
| Compressed full-sync replication stream | Yes | Q12: full-sync RDB over the wire is uncompressed; disk RDB is compressed. | Conditional — ship compressed full-sync as a distinct optimization, requires both sides to support. |
| `MEMORY STATS` compression sub-aggregate | Yes | Q14: `INFO compression` covers the numbers. | Planned — small code cost. |
| Primary↔replica dictionary synchronization | Yes | Replicas operate independently; each self-trains or runs uncompressed. | Not planned — independent operation is the intended model. |

**Headline scope statement (for the design doc):**

> v1 ships compression for `OBJ_STRING` values only, with synchronous decompression on the main thread, one active dictionary per server, self-trained by a keyspace-scan job on `bio`, with explicit operator controls (`COMPRESSION` subcommands + `compression-*` configs). All other value types, async decompression, adaptive behaviors, and cluster-level dictionary coordination are explicit v2 scope.

---
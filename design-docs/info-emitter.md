# Info Emitter: a pluggable sink for INFO field generation

## Status

Proposal / RFC. Prerequisite for direct OpenTelemetry (OTLP) metric export
without an INFO string round-trip.

## Problem

`genValkeyInfoString()` (`src/server.c`) builds the `INFO` output by writing
`key:value\r\n` text directly into an `sds`, using inline `sdscatfmt`/
`sdscatprintf` calls across ~13 sections (~700 lines). Anything that wants
INFO data as *structured* values must generate this text and then parse it back:

- `VM_GetServerInfo()` (module API) does exactly this: `genValkeyInfoString()`
  then `sdssplitlen()` + parse into a rax.
- A metrics exporter (e.g. OpenTelemetry/OTLP) must do the same: generate the
  string, split it, re-parse each `key:value`, classify the number, and
  re-encode. A build -> parse -> build round trip.

The round trip is wasteful (format numbers to text, then parse text back to
numbers) and loses type information (counter vs gauge, int vs double) that the
producing code knew but the text does not carry.

## Goal

Introduce an **info emitter**: an abstraction over "emit a section / emit a
field" so the *same* generation code can target multiple backends:

1. **text backend** — reproduces the current `INFO` output byte-for-byte (used
   by the `INFO` command, crash reports, sentinel, etc.).
2. **structured backends** — e.g. an OTLP metrics backend that receives typed
   values directly, with no string round trip. (Also usable by
   `VM_GetServerInfo` to build the rax directly.)

Non-goals: changing the `INFO` wire format, or changing module-facing APIs in a
breaking way.

## Prior art in the codebase

The module INFO API is already an emitter in everything but name
(`src/module.c`): modules call `ValkeyModule_InfoAddSection()` and
`ValkeyModule_InfoAddField{LongLong,ULongLong,Double,CString,String}()`, plus
`ValkeyModule_InfoBeginDictField()/EndDictField()` for composite lines. Today
those functions write text into `ctx->info`. The refactor generalizes this
shape (add a backend vtable) and, ideally, **routes both core sections and
module callbacks through the same emitter** — so any backend automatically
covers module-provided fields with no extra work.

## Interface

As shipped (see `src/info_emitter.h`):

```c
/* Metric semantics carried on every numeric field (ignored by the text backend
 * except INFO_UNIT_PERCENT; used by structured backends). */
typedef enum { INFO_KIND_GAUGE = 0, INFO_KIND_COUNTER } infoKind;
typedef enum {
    INFO_UNIT_NONE = 0, INFO_UNIT_BYTES, INFO_UNIT_SECONDS,
    INFO_UNIT_MILLISECONDS, INFO_UNIT_MICROSECONDS, INFO_UNIT_PERCENT,
} infoUnit;

/* prec >= 0 renders "%.*f"; prec == INFO_PREC_FULL renders "%.17g". */
#define INFO_PREC_FULL (-1)

typedef struct infoEmitter infoEmitter;

typedef struct infoEmitterOps {
    void (*begin_section)(infoEmitter *e, const char *name);
    void (*field_ll)(infoEmitter *e, const char *key, long long v, infoKind kind, infoUnit unit);
    void (*field_ull)(infoEmitter *e, const char *key, unsigned long long v, infoKind kind, infoUnit unit);
    void (*field_double)(infoEmitter *e, const char *key, double v, int prec, infoKind kind, infoUnit unit);
    void (*field_str)(infoEmitter *e, const char *key, const char *v);
    /* Length-aware string, reproducing sds %S semantics (embedded NULs). */
    void (*field_strn)(infoEmitter *e, const char *key, const char *v, size_t vlen);
    /* Fixed-point seconds.microseconds (%lld.%06lld); CPU/duration fields. */
    void (*field_usec)(infoEmitter *e, const char *key, long long usec, infoKind kind);
    /* Composite ("dict") lines: key:sub1=v1,sub2=v2  (keyspace, commandstats). */
    void (*begin_dict)(infoEmitter *e, const char *key);
    void (*dict_ll)(infoEmitter *e, const char *sub, long long v, infoKind kind, infoUnit unit);
    void (*dict_ull)(infoEmitter *e, const char *sub, unsigned long long v, infoKind kind, infoUnit unit);
    void (*dict_double)(infoEmitter *e, const char *sub, double v, int prec, infoKind kind, infoUnit unit);
    void (*dict_str)(infoEmitter *e, const char *sub, const char *v);
    void (*dict_strn)(infoEmitter *e, const char *sub, const char *v, size_t vlen);
    void (*end_dict)(infoEmitter *e);
    /* Escape hatch for irregular lines. Text backend prints it verbatim;
     * structured backends may skip it. */
    void (*raw)(infoEmitter *e, const char *fmt, va_list ap);
} infoEmitterOps;

/* The emitter is embedded as the first member of a backend-specific container
 * (e.g. infoEmitterText), so the container is recovered from the base pointer. */
struct infoEmitter {
    const infoEmitterOps *ops;
};

/* Terse dispatch helpers default kind/unit to the common cases, e.g.
 * infoEmitFieldLL (gauge), infoEmitCounterLL (counter),
 * infoEmitMetricULL(e, key, v, kind, unit), infoEmitFieldDouble(e, key, v, prec). */
```

### Why these field kinds

Auditing `genValkeyInfoString()` shows every field is one of:

| Current format         | Emitter call            | Notes                                  |
|------------------------|-------------------------|----------------------------------------|
| `%lld`, `%d`, `%ld`    | `field_ll`              | signed integers                        |
| `%llu`, `%lu`, `%zu`   | `field_ull`             | unsigned/size_t (same digits as `%llu`)|
| `%.2f`, `%.3f`, `%.6f` | `field_double(.., prec)`| precision preserved via `prec`; `INFO_PREC_FULL` (-1) = `%.17g` |
| `%s`                   | `field_str`             | NUL-terminated strings                 |
| `%S` (sds)             | `field_strn`            | length-aware (preserves embedded NULs) |
| `%ld.%06ld` (sec.usec) | `field_usec`            | CPU times; text = sec.usec, OTLP = seconds/usec |
| `db0:keys=..,..`       | `begin_dict`/`dict_*`   | keyspace, commandstats, latencystats (`dict_ull` for unsigned) |
| irregular              | `raw`                   | rare; verbatim in text, skipped in OTLP|

## Byte-for-byte parity strategy

Integers and strings reproduce exactly by construction. The only formatting
that varies per field is floating point, which the `prec` argument preserves
(`field_double(e,"mem_fragmentation_ratio",v,2)` -> `%.2f`). The `field_usec`
kind preserves the `sec.usec` layout. The `raw` escape hatch covers anything
that does not fit, guaranteeing the text backend can always reproduce the
current output.

Parity is enforced by a test that captures `INFO everything` from an unmodified
build and from the emitter build and diffs them (value-independent fields), plus
a unit test that asserts the text backend produces the exact legacy string for
each field kind and precision.

## Migration plan (incremental, always green)

1. Land the emitter interface + text backend (no behavior change).
2. Convert core sections one at a time, verifying INFO parity after each. Start
   with a self-contained section (CPU or Cluster), then the large ones (Stats,
   Memory, Persistence, Replication, Keyspace).
3. Route the module INFO API (`VM_InfoAddField*`) through the emitter so module
   fields flow to any backend. Keep the public module API unchanged.
4. Only after core + modules are on the emitter, add the OTLP backend and switch
   the exporter to run `genValkeyInfoString()` with the OTLP emitter — deleting
   its INFO-parsing code.

Each step is independently shippable and reviewable; steps 1-3 are pure
refactors with zero functional change, which is the bar for maintainer review.

## Performance

The text backend replaces one big batched `sdscatfmt` per section with N
per-field calls. This is a few extra function calls per field (indirect through
the vtable) but the same number of `sds` appends; net cost is negligible and off
the command hot path (INFO is not a hot path). Structured backends avoid the
round-trip entirely. No change to command processing.

## Risks

- **Parity regressions** in floating-point/edge formats — mitigated by the
  parity + unit tests above and incremental conversion.
- **Large diff** to a core file — mitigated by section-at-a-time PRs.
- **Module API coupling** — step 3 must keep the public API and output identical.

## Open questions

- Should `field_ull` values above `INT64_MAX` be represented in structured
  backends as double or as a string? (Text is unaffected.)
- Do we want per-field unit/type metadata (By, seconds, counter vs gauge) in the
  emitter so structured backends get correct semantics without heuristics?
  **Resolved: implemented.** The interface carries `infoKind` (GAUGE/COUNTER)
  and `infoUnit` (NONE/BYTES/SECONDS/MILLISECONDS/MICROSECONDS/PERCENT) on every
  numeric field. The text backend ignores them except `PERCENT` (which appends a
  literal `%` to reproduce `%.2f%%`); structured backends read them directly.

## Implementation status

The emitter interface + text backend are in place, and **every section** of
`genValkeyInfoString()` and its helpers (`genValkeyInfoStringCommandStats`,
`genValkeyInfoStringLatencyStats`, `genValkeyInfoStringACLStats`,
`genValkeyInfoStringScriptingEngines`, `fillPercentileDistributionLatencies`)
now emit through the emitter. The inter-section separator is owned by
`begin_section` (format-agnostic).

The module INFO callback API (`VM_InfoAddSection` / `VM_InfoAddField*` /
`VM_InfoBeginDictField` / `VM_InfoEndDictField` in `module.c`) is **also routed
through the emitter** via `modulesCollectInfo()`, so module-provided fields reach
structured backends as typed values too. The public module header
(`valkeymodule.h`) and the opaque `ValkeyModuleInfoCtx` are unchanged, so
existing third-party modules are unaffected and their INFO output is
byte-identical. Small interface additions support the module formats exactly:
`dict_ull` (unsigned dict fields), `INFO_PREC_FULL`/`%.17g`
(`VM_InfoAddFieldDouble`), and `field_strn`/`dict_strn` (sds `%S` semantics).

With both core and module INFO on the emitter, the only remaining step for OTLP
is adding the OTLP backend and switching the exporter to run
`genValkeyInfoString()` with it.

### Parity

`INFO everything` from an unmodified build and the emitter build are
byte-for-byte identical, after normalizing inherently non-deterministic values
(timestamps, memory, random ids, build path) and accounting for the
process-nondeterministic hashtable iteration order of the `Commandstats` and
`Latencystats` sections (identical field sets, different order between
processes). Verified via a diff harness plus GoogleTest unit tests
(`src/unit/test_info_emitter.cpp`) covering each field kind, the `PERCENT`
suffix, the section separator, and byte-parity of the CPU/Cluster/Stats
formatting.

### Performance

Measured with `valkey-benchmark`, CPU-pinned, interleaved base-vs-modified over
multiple rounds:

- **Command hot path unaffected:** SET/GET throughput is identical (the emitter
  is not on the command path).
- **INFO generation:** a naive one-`sdscatprintf`-per-field text backend
  regressed `INFO everything` generation by ~19–24% vs the legacy
  one-`sdscatprintf`-per-section batching. This was closed — and then reversed
  into a speedup — in stages:
  1. format integers with `ll2string`/`ull2string`, dropping per-field
     `vsnprintf` → ~11–14% slower;
  2. assemble each line with a single `sdscatlen` → ~5% slower;
  3. a per-emitter scratch buffer (memcpy fields, flush per chunk) → ~1% slower,
     within noise — but still double-copied the bytes (field→scratch→sds);
  4. **write fields directly into the sds buffer**: `ie_reserve()` grows the sds
     in chunks and commits the length per-chunk, and each field is formatted in
     place (`ll2string`/`ull2string`, or one `snprintf` for doubles/usec). This
     is a single copy per byte with no format parsing for integers, so it does
     strictly less work than the legacy `vsnprintf`-per-section path.
  Result: the emitter build is **faster than the unmodified baseline** at INFO
  generation — roughly +5% single-client and +10% at concurrency 10 (medians,
  CPU-pinned, interleaved, 8 rounds; the emitter beat baseline in every round),
  with output still byte-for-byte identical. Structured backends avoid text
  formatting entirely.


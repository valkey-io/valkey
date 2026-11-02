# Measuring stream memory in O(1)

## The problem

`MEMORY USAGE` on a stream used to walk the stream. It sampled some listpacks
and some consumer groups, then guessed the rest. This had two problems:

- It cost O(N) work for each call.
- The result was only an estimate.

## How a stream is built

```
 stream
 |-- rax (data tree): ID -> listpack
 |     [listpack 1-0..100-0] [listpack 101-0..200-0] [listpack 201-0..230-0, tail]
 |-- tracked_memory_bytes    <- counter 1: bytes of all listpacks
 |
 |-- cgroups (rax): group name -> streamCG
 |     "shipping" -> streamCG
 |                   |-- pel (rax): ID -> NACK ----------------+
 |                   |-- consumers (rax): name -> streamConsumer |
 |                         "worker-1" -> streamConsumer         |
 |                                       |-- name (sds)         |
 |                                       |-- pel (rax): ID -> --+  (same NACK)
 |-- cgroups_memory_bytes    <- counter 2: bytes of all groups
```

- **Listpack**: a block of memory that holds many stream entries.
- **Consumer group**: a set of readers that share the work of reading the
  stream.
- **NACK**: a small record for a message that a consumer got but did not
  confirm with `XACK` yet. The group PEL and the consumer PEL point to the same
  NACK, so we count it once.

## The idea: keep running totals

We do not walk the stream when someone asks for its size. We keep two counters
inside `struct stream`, and we update them on every change:

| Counter | What it holds |
|---|---|
| `tracked_memory_bytes` | Allocated bytes of all listpacks |
| `cgroups_memory_bytes` | Allocated bytes of all groups, consumers, consumer names, PELs and NACKs |

Reading the size is a sum of five O(1) values (`streamMemUsage()`):

```
sizeof(stream) + raxAllocSize(data rax) + tracked_memory_bytes
               + raxAllocSize(cgroups rax) + cgroups_memory_bytes
```

The rax already keeps its own size (`raxAllocSize()` is O(1)), so we only had to
add the parts outside the rax.

## How the counters stay correct

- **Before and after.** When code changes a listpack or a rax, it reads the
  allocated size before and after the change, and adds the difference. Small
  helpers do this: `streamShrinkLpToFit()` for listpacks, `streamCGRaxInsert()`
  and `streamCGRaxRemove()` for group trees.
- **Allocated size, not used size.** We use `zmalloc_size()`, because a new
  listpack gets extra memory at first. This is the real memory cost.
- **One size for every NACK.** All NACKs have the same size, so we measure one
  NACK once and use that value everywhere.
- **No exceptions.** RDB load and `COPY` build the stream with the same
  helpers, so they fill both counters while they add each item. No extra walk
  is needed at the end.
- **Self-check.** When `enable-debug-assert` is on (all tests), every
  `MEMORY USAGE` call also walks the whole stream and checks that both counters
  are right. A missed update makes the test fail at once.

## Cost

- `struct stream` grows from 80 to 96 bytes (two `size_t` fields).
- Each write does a few more integer operations.
- `MEMORY USAGE` on a stream is now exact, and O(1) no matter how big the
  stream is.

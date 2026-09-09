# Design Document: I/O Threads

## Overview

Valkey uses a fixed pool of worker threads to move expensive socket and
memory-management work off the main thread. The main thread owns all data
structure mutation; workers only execute predefined job kinds (socket reads,
socket writes, accepts, polls, deferred frees) and publish their results back
for the main thread to apply.

This document covers the shared I/O threading infrastructure: queue topology,
job lifecycle, draining and backpressure, and the rules a feature must follow
to add itself as a consumer. Feature-specific consumers (client I/O, cluster
bus I/O) are documented separately and reference this skeleton.

## Threading Model

- **Thread 0 is the main thread.** It runs the event loop and owns all server
  state. Workers must not touch shared state outside the data their job
  explicitly carries.
- **Threads 1..N are workers.** Each worker has a stable thread id used to
  index per-thread state.
- `inMainThread()` and `getCurTid()` identify the current thread; correctness
  asserts use `inMainThread()` liberally.
- Workers pin to the configured CPU list and block on a per-thread mutex when
  idle. The main thread parks workers by holding their mutex and wakes them
  by releasing it.

The thread count is controlled by `io-threads`. Live updates go through
`updateIOThreads()`, which drains in-flight jobs before changing the active
count. See [Live reconfiguration](#live-reconfiguration).

## Queue Topology

Three queue primitives connect the main thread and workers. All three live in
`src/queues.{c,h}`.

```text
+-------------+   io_shared_inbox (SPMC)    +----------------+
| Main thread |---------------------------->| Worker threads |
| (thread 0)  |   io_private_inbox[i]       | (1..N)         |
|             |---- (SPSC, per worker) ---->|                |
|             |                             |                |
|             |   io_shared_outbox (MPSC)   |                |
|             |<----------------------------|                |
+-------------+                             +----------------+
```

| Queue | Kind | Direction | Purpose |
|---|---|---|---|
| `io_shared_inbox` | SPMC | main → any worker | Default request channel. Any worker pulls the next job. |
| `io_private_inbox[i]` | SPSC | main → worker `i` | Targeted request channel for jobs that must run on a specific worker. |
| `io_shared_outbox` | MPSC | any worker → main | Single response channel. |

A job uses the private inbox (instead of the shared one) when its execution
must be pinned to a specific worker. Two reasons drive this today:

- **Thread-affinity invariants.** `FREE_ARGV` is enqueued on the worker whose
  thread id (`cur_tid`) is recorded on the argv batch — so the same thread
  that built it tears it down. This avoids cross-thread free contention on the
  per-thread argv allocator.
- **Avoiding shared-queue contention at high thread counts.** `POLL` jobs are
  steered to a specific worker once thread counts are large enough that
  contention on the SPMC head would dominate the work itself.

When dispatch is non-pinned, the main thread pushes onto `io_shared_inbox` and
lets any idle worker claim the job.

Worker priority: a worker drains its private SPSC inbox in batches before it
checks the shared SPMC inbox. SPSC enqueues may be batched by the producer
and committed via `spscCommit()`; the main thread calls `commitIOJobs()`
before sleep so batched work becomes visible to workers.

### Tagged pointers

Jobs are passed as tagged pointers — the low 3 bits encode a `JobRequest` or
`JobResult` enum value, the rest is the data pointer. This requires data
pointers to be 8-byte aligned (always true for `zmalloc`-allocated objects).
Helpers `tagJob()` and `untagJob()` enforce this in `io_threads.c`.

## Job Kinds

Request kinds (main → worker) and result kinds (worker → main) are defined in
`io_threads.h`:

```c
JobRequest  := READ_CLIENT | WRITE_CLIENT | FREE_ARGV | FREE_OBJ | POLL | ACCEPT
JobResult   := READ_CLIENT | WRITE_CLIENT
```

Both enums are capped at 8 entries so they fit in the 3 tag bits.

A new consumer adds:
- a request kind (and, if it needs a completion, a result kind)
- a `try*ToIOThreads()` dispatch helper on the main thread
- a worker handler invoked from the dispatch loop in `IOThreadMain`
- a completion handler invoked from `processIOThreadsResponses()`

The dispatch helper is also where eligibility is checked — i.e. whether the
job is safe to hand off at all. Common gates used today:

- **Workers are available.** `server.active_io_threads_num <= 1` means the
  main thread is the only worker; offload returns `C_ERR`.
- **Per-client state.** Read offload skips clients that are already in flight
  (`c->io_read_state != CLIENT_IDLE`), blocked, marked `close_asap`, or have
  no relevant work pending (e.g. write offload requires `clientHasPendingReplies`).
- **Per-job preconditions.** `ACCEPT` only offloads when the connection
  carries `CONN_FLAG_ALLOW_ACCEPT_OFFLOAD`; `POLL` only runs on a worker if
  there are pending IO responses to interleave with the wait.

When a check fails the helper returns `C_ERR` and the caller runs the work
inline on the main thread — offload is best-effort, never required for
correctness.

## Job Lifecycle

```text
[MAIN] try*ToIOThreads()
    | validate eligibility
    | snapshot any state the worker needs
    | mark caller as "pending"
    | spmcEnqueue / spscEnqueue
    | io_jobs_submitted++
    v
[QUEUE] io_shared_inbox or io_private_inbox[i]
    v
[WORKER] IOThreadMain
    | untag, dispatch by JobRequest
    | execute handler (pure transport / memory work)
    | atomic_fetch_add(io_jobs_finished)
    | sendToMainThread(data, JobResult)   (only if completion is needed)
    v
[QUEUE] io_shared_outbox
    v
[MAIN] processIOThreadsResponses()
    | mpscDequeueBatch
    | dispatch by JobResult
    | apply state changes, reinstall handlers, etc.
```

Counters:
- `io_jobs_submitted` — main-thread-only; incremented at every successful enqueue.
- `io_jobs_finished` — atomic; incremented by workers after handler runs.
- `getPendingIOThreadsJobs() = io_jobs_submitted - io_jobs_finished`.

`stat_io_reads_pending` and `stat_io_writes_pending` track jobs that still
owe a response on the outbox (separate from `io_jobs_finished`, which marks
worker-side completion).

## Draining and Backpressure

### Outbox backpressure

When a worker can't enqueue a result because `io_shared_outbox` is full it
buffers the response in a thread-local `pending_io_responses` list and retries
on each loop iteration via `flushPendingIOResponses()`. This keeps workers
making forward progress even under burst load. On thread shutdown,
`cleanupThreadResources()` performs a blocking flush so no responses are lost.

### Main-thread drain

`drainIOThreadsQueue()` is the synchronous barrier used before any change
that requires no in-flight worker activity (config reload, thread count
change, shutdown). It commits batched SPSC jobs and spins until
`getPendingIOThreadsJobs() == 0`.

> **Caveat:** `drainIOThreadsQueue()` does not itself drain `io_shared_outbox`.
> Callers must ensure the outbox cannot fill while draining, otherwise workers
> can stall on `flushPendingIOResponses` and the spin will not progress. The
> current guard is in `updateIOThreads()`, which refuses the resize if
> `getPendingIOResponsesCount() > io_shared_outbox.queue_size`.

### Per-client wait

`waitForClientIO()` is a finer-grained primitive used when the main thread
needs a single client's I/O to settle (e.g. before freeing it). It spins on
the per-client `io_read_state` / `io_write_state` rather than the global
counter.

## Live Reconfiguration

`updateIOThreads()` handles `CONFIG SET io-threads`:

1. Compute the previous active thread count from `io_threads[]`.
2. Refuse the change if the outbox could overflow during the drain
   (`pending > queue_size`).
3. Drain in-flight jobs (`drainIOThreadsQueue`).
4. Park all workers (lock their mutexes), set `active_io_threads_num = 1`.
5. Spawn or shut down workers to reach the new target.
6. The scaling policy in `IOThreadsAfterSleep` re-activates workers as load
   warrants.

`IOThreadsBeforeSleep` and `IOThreadsAfterSleep` implement the dynamic scaling
policy: ignite when main-thread active time crosses
`IO_IGNITION_MAIN_THREAD_ACTIVE_PERCENT`, scale up when the SPMC queue is
non-empty, scale down after `IO_COOLDOWN_MS` of idle. `io-threads-always-active`
disables the policy and keeps all configured workers awake.

## Relevant Code

- `src/io_threads.{c,h}` — main thread dispatch helpers, worker loop,
  scaling policy.
- `src/queues.{c,h}` — SPMC, MPSC, and SPSC queue primitives.
- `src/networking.c` — client read/write handlers invoked from worker job
  dispatch (`ioThreadReadQueryFromClient`, `ioThreadWriteToClient`).

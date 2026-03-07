# Filtering on Duration with LTTng Snapshots

Exit events in Valkey's LTTng tracepoints carry a `duration` field marked
as `nowrite`. This field is **not** written to the trace output, but it
**is** available for real-time filtering. This is useful for snapshot-based
tracing: the trace buffers keep rolling in flight mode, and a snapshot is
captured only when a slow event is detected.

## Quick Start

### 1. Create a snapshot session

```bash
lttng create valkey-slow --snapshot
```

### 2. Enable events with a duration filter

Capture only exit events where duration exceeds a threshold (in
microseconds):

```bash
# Commands slower than 1ms
lttng enable-event -u 'valkey_server:command' --filter 'duration > 1000'
lttng enable-event -u 'valkey_server:fast_command' --filter 'duration > 1000'

# Per-command detail slower than 1ms
lttng enable-event -u 'valkey_commands:command_call' --filter 'duration > 1000'
```

### 3. Add context and start

```bash
lttng add-context -u -t vtid -t procname
lttng start
```

### 4. Take a snapshot when needed

```bash
lttng snapshot record
```

### 5. View results

```bash
lttng stop
lttng destroy valkey-slow
babeltrace2 ~/lttng-traces/valkey-slow-*
```

## Filter Examples

```bash
# Event loop iterations longer than 10ms
lttng enable-event -u 'valkey_server:eventloop' \
    --filter 'duration > 10000'

# AOF writes longer than 5ms
lttng enable-event -u 'valkey_aof:aof_write' \
    --filter 'duration > 5000'

# AOF fsync longer than 1ms (always policy)
lttng enable-event -u 'valkey_aof:aof_fsync_always' \
    --filter 'duration > 1000'

# Expire cycle longer than 2ms
lttng enable-event -u 'valkey_db:expire_cycle' \
    --filter 'duration > 2000'

# Fork operations longer than 50ms
lttng enable-event -u 'valkey_rdb:fork' \
    --filter 'duration > 50000'
lttng enable-event -u 'valkey_aof:fork' \
    --filter 'duration > 50000'

# Cluster config fsync longer than 1ms
lttng enable-event -u 'valkey_cluster:cluster_config_fsync' \
    --filter 'duration > 1000'

# Combine filters: slow SET/GET commands from a specific connection type
# (enum_field 0 = SOCKET)
lttng enable-event -u 'valkey_commands:command_call' \
    --filter 'duration > 1000 && enum_field == 0'
```

## Continuous Monitoring with Snapshots

A typical production workflow: run in snapshot mode and trigger a snapshot
from an external monitor when latency spikes are observed.

```bash
# Setup
lttng create valkey-monitor --snapshot
lttng enable-event -u 'valkey_server:command' --filter 'duration > 5000'
lttng enable-event -u 'valkey_server:fast_command' --filter 'duration > 5000'
lttng enable-event -u 'valkey_server:eventloop' --filter 'duration > 10000'
lttng enable-event -u 'valkey_commands:command_call' --filter 'duration > 5000'
lttng add-context -u -t vtid -t procname
lttng start

# From a monitoring script, when p99 latency spikes:
lttng snapshot record

# Cleanup
lttng stop
lttng destroy valkey-monitor
```

## Notes

- The `duration` field is in microseconds.
- Since `duration` is `nowrite`, it does not appear in the trace output.
  Use entry/exit timestamps to compute duration in post-analysis tools.
- Entry events (`*_entry`) have no duration field and cannot be filtered
  on duration.
- Snapshot mode uses flight recorder buffers — only the most recent data
  is captured when `lttng snapshot record` is called.

## Combining Kernel and Userspace Tracing

LTTng can trace kernel events alongside Valkey's userspace tracepoints in
the same session. This lets you correlate Valkey operations with system
calls, scheduling, block I/O, and CPU migrations.

### Setup

```bash
# Create a session (requires root for kernel tracing)
sudo lttng create valkey-full

# Enable Valkey userspace events
sudo lttng enable-event -u 'valkey_server:*'
sudo lttng enable-event -u 'valkey_commands:*'
sudo lttng enable-event -u 'valkey_aof:*'
sudo lttng enable-event -u 'valkey_db:*'
sudo lttng enable-event -u 'valkey_rdb:*'
sudo lttng enable-event -u 'valkey_cluster:*'

# Enable kernel syscall tracing
sudo lttng enable-event -k --syscall --all

# Enable scheduler events (context switches, wakeups)
sudo lttng enable-event -k sched_switch,sched_wakeup,sched_migrate_task

# Enable block I/O events
sudo lttng enable-event -k block_rq_issue,block_rq_complete

# Add context: thread ID, CPU ID, process name
sudo lttng add-context -u -t vtid -t procname
sudo lttng add-context -k -t tid -t procname

sudo lttng start
```

### Targeted Kernel Tracing

To reduce overhead, filter kernel events to the Valkey process:

```bash
VALKEY_PID=$(pidof valkey-server)

# Syscalls from Valkey only
sudo lttng track -k --pid $VALKEY_PID

# Only trace specific syscalls (fsync, write, read, epoll_wait)
sudo lttng enable-event -k --syscall write,read,fsync,fdatasync,epoll_wait
```

### Analyzing System Call Cost

With both kernel and userspace events, you can see exactly which system
calls happen inside a Valkey operation:

```
valkey_aof:aof_write_entry          [cpu 2]  vtid=1234
  syscall_entry_write               [cpu 2]  tid=1234  fd=7 count=4096
  syscall_exit_write                [cpu 2]  tid=1234  ret=4096
valkey_aof:aof_write                [cpu 2]  vtid=1234
```

### Critical Path Analysis

Use Trace Compass or babeltrace2 to identify:

- **CPU time per operation**: correlate `sched_switch` events with
  entry/exit pairs to see how much wall time vs CPU time an operation
  consumes.
- **I/O latency**: match `block_rq_issue` / `block_rq_complete` pairs
  inside AOF or RDB operations to measure actual disk latency.
- **Scheduling delays**: `sched_wakeup` to `sched_switch` gaps show
  how long Valkey waited in the run queue.
- **CPU migrations**: `sched_migrate_task` events reveal when the
  scheduler moves the Valkey thread between cores, which can cause
  cache misses.

### Example: Finding fsync Bottlenecks

```bash
sudo lttng create valkey-fsync --snapshot
sudo lttng enable-event -u 'valkey_aof:aof_fsync_always' \
    --filter 'duration > 1000'
sudo lttng enable-event -k --syscall fsync,fdatasync
sudo lttng enable-event -k block_rq_issue,block_rq_complete
sudo lttng track -k --pid $(pidof valkey-server)
sudo lttng add-context -u -t vtid
sudo lttng add-context -k -t tid
sudo lttng start

# When latency spikes:
sudo lttng snapshot record

sudo lttng stop
sudo lttng destroy valkey-fsync
babeltrace2 ~/lttng-traces/valkey-fsync-*
```

This captures the full picture: the Valkey `aof_fsync_always` event
filtered to slow occurrences, the underlying `fdatasync` syscall, and
the block I/O requests that show actual disk latency.

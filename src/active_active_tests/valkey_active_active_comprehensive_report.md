# Comprehensive Analysis and Verification of Valkey `bet0x` Active-Active Fork

This report provides a comprehensive technical deep-dive into the `bet0x` multi-master (Active-Active) replication fork of Valkey. It covers the overall replication architecture, logical clock conflict resolution semantics, a critical structural gap in replication ID invariants, and a step-by-step verification of a split-brain data loss vulnerability using test automation.

---

## 1. Executive Summary

The `bet0x` fork extends Valkey to support a multi-master mesh topology where peer nodes function as mutual primary upstreams. Replicated writes are fanned out using a custom `RREPLAY` envelope that encapsulates database identifiers, logical clocks (`mvcc-ts`), and transaction identifiers (`replay-id`).

While LWW (Last-Writer-Wins) logical clocks successfully merge concurrent updates under low latency, the system exhibits critical design gaps under network partitions:
1. **Replication Stream Invariant Gap**: The system continues to use standard master-replica replication IDs (`replid`) across multiple concurrent masters. During partitions, this leads to branched timelines under the same ID, violating a core mathematical invariant of the Valkey replication engine.
2. **Backlog Trim-Driven Full Sync**: Multi-master connections bypass the custom memory queues and rely on standard backlog buffers (`server.repl_backlog`). When writes exceed the backlog, trimming forces full resynchronizations (`REPLICAOF`).
3. **Asymmetric Data Wipe**: During concurrent full syncs, the node that finishes transferring its snapshot first terminates all replica connections before loading. This aborts the peer's RDB download, resulting in an asymmetrical state where one node flushes its database and accepts the peer's writes (losing its own partition history), while the other node aborts loading and preserves its local writes.

---

## 2. Replication Architecture & Propagation Path

The `bet0x` fork establishes a bi-directional topology by configuring masters as mutual upstreams.

### Scaffolding Structures
Upstream configurations and link states are tracked via the following key structures in `src/server.h`:
- **`valkeyUpstream`**: Represents a configured upstream master node endpoint (host/port).
- **`valkeyUpstreamRuntime`**: Tracks the active connection (`link_client`), replication states, logical clock offsets, and the pending frame queue for each upstream peer.

### Interception & Propagation Workflow
When a write command executes on a master node:
1. **Interception**: Command propagation is intercepted in `propagateNow` inside `src/server.c`.
2. **Gatekeeper Check**: It verifies whether to replicate via active-active envelopes by calling `shouldForwardToPrimaryViaRReplay`. The traffic is intercepted only if `multi_master` is active and the command did not arrive from a replica link (preventing replication feedback loops).
3. **Canonicalization of Read-Modify-Write (RMW)**: In `replicationFeedPrimaryWithRReplay`, if a command is a relative write (e.g., `INCR`, `APPEND`), it is canonicalized to an absolute write using `rreplayBuildCanonicalRmwPayload`. This converts state-dependent updates into deterministic writes (such as `SET` or `HSET`) based on the current state of the database.
4. **Envelope Packaging**: The command is wrapped into a special `RREPLAY` envelope command:
   `RREPLAY <origin-uuid> <dbid> <replay-id> <mvcc-ts> <payload_command> [args...]`
   - `origin-uuid`: The source server's `runid`.
   - `replay-id`: An incrementing sequence number for ordering and ACKs.
   - `mvcc-ts`: The logical clock timestamp generated via `mvccNextLocalClock()`.
5. **Fanout Dispatch**: The RESP-encoded `RREPLAY` frame is sent to all peers by calling `fanoutRReplayFrameToUpstreams` in `src/replication.c`.

---

## 3. Conflict Resolution & Loop Prevention

To achieve eventual consistency across masters, `bet0x` implements a Last-Writer-Wins (LWW) strategy using key-level logical clocks.

### Clock Management
- Key logical clocks are stored in `server.mvcc_key_clock` (a hash table mapping keys to `uint64_t` logical timestamps).
- Conflicts with identical timestamps are resolved using tie-breakers in `server.mvcc_key_tie_break` (maps keys to `"<origin-uuid>:<replay-id>"` strings).
- Clocks are advanced locally on writes and updated on replayed commands.

### Replay Execution & LWW Verification
When an `RREPLAY` frame is received:
1. **Loop Prevention**:
   - **Own-Origin Check**: In `rreplayCommand`, if the incoming `origin-uuid` matches the local `server.runid`, the command is discarded immediately.
   - **Seen-Deduplication**: If the frame's `origin-uuid` and `replay-id` match a key in `server.rreplay_seen`, it is ignored. The deduplication table is capped at 10,000 entries.
2. **LWW Check**: For every key in the replayed payload, `mvccCommandIsFresh` compares the frame's `mvcc_ts` with the key's current clock in `server.mvcc_key_clock`:
   - If `mvcc_ts < current_key_clock`, the write is dropped as stale.
   - If `mvcc_ts == current_key_clock`, the tie-breaker is checked lexicographically. If the local tie-breaker wins, the update is dropped.
3. **Execution & Stamping**: If the write is fresh, it is executed via a fake client, and the clocks are updated via `mvccStampCommandKeys`.

---

## 4. Logical Clocks, sequence IDs, and Tie-Breakers

The active-active replication protocol relies on two distinct identifiers carried by each `RREPLAY` frame: the **`replay-id`** and the **`mvcc-ts`** (logical timestamp).

### `replay-id`: Delivery Tracking & Tie-Breaking
The `replay-id` is a sequential, incrementing ID generated by a master for each outbound change it propagates to a specific peer.
- **Delivery Guarantee & Replay Buffering**: On disconnection, the sender buffers unacknowledged updates. The receiver tracks the last processed ID via `replay_last_received_id` and periodically sends ACKs back. The sender uses these ACKs to prune its memory queue.
- **Deduplication Check**: Combined with the originating node's UUID, the `replay-id` formulates a unique transaction identifier used in the seen deduplication table to discard duplicate frames that arrive via multiple network paths.
- **Tie-Breaking (UUID Namespacing)**: If two writes on different masters happen at the exact same logical time (`mvcc-ts`), the tie-breaker is generated as `<origin-uuid>:<replay-id>` to ensure deterministic resolution.
  - **Why it is globally unique**: While `replay-id` will frequently overlap across different masters, the `<origin-uuid>` prefix is the master's randomly generated 40-character SHA1 run ID (`server.runid`). This prefix guarantees that the combined string `<origin-uuid>:<replay-id>` (e.g., `a1b2c3d4...:101` vs `e5f6g7h8...:101`) is unique.
  - **How it resolves conflicts**: If `mvcc-ts` is equal, nodes compare the tie-breaker strings lexicographically using standard `strcmp`. Since the strings are unique and the sorting behavior of `strcmp` is entirely deterministic, all nodes in the cluster evaluate the comparison identically, agreeing on the same winning write.

### `mvcc-ts`: Logical Clock Timestamp for Conflict Resolution
The `mvcc-ts` is a logical clock timestamp assigned to a write at the time it originates on a master.
- **Eventual Consistency (Last-Writer-Wins)**: When multiple masters accept concurrent updates to the same key, they use the `mvcc-ts` at the key level to determine which write is the "newest" and should win.
  - **Why logical timestamps can overlap**: `mvcc-ts` uses the system's microsecond-resolution clock (`ustime()`) as its base. If two masters receive concurrent writes within the exact same microsecond, they will generate identical timestamps. Alternatively, if a partition occurs and the nodes sync their clocks afterward, new writes on both sides might increment the local clock from the same base value (`server.mvcc_clock + 1`), also resulting in overlapping timestamps.
- **Causal Time Alignment**: When a node receives an `RREPLAY` frame, it compares the incoming `mvcc_ts` to its local logical clock `server.mvcc_clock`. If the incoming timestamp is higher, the local clock is advanced to align logical time across the cluster.

---

## 5. The Replication Stream Invariant Gap

Standard Valkey (and Redis) replication operates on a core mathematical invariant:
> **A Replication ID (`master_replid`) uniquely identifies a single, linear, immutable history of write commands. Any node at replication offset `N` on stream `XYZ` must have the exact same dataset state.**

The `bet0x` active-active fork breaks this invariant:
1. **Branched Histories under a Single ID**: Because both masters are permanent masters and accept writes independently during a network partition, they both retain the same replication ID (`replid`). However, they append different local writes to their streams. Node A appends `keyA` writes, and Node B appends `keyB` writes.
2. **Backlog offset mismatch**:
   - Both nodes now claim to represent stream `XYZ` at offset `200`.
   - However, the byte sequence in the range `100 -> 200` represents `keyA` commands on Node A, and `keyB` commands on Node B.
3. **Consequences**:
   - **In Short Outages**: If the backlog does not overflow, they attempt `PSYNC` and exchange the different history chunks. While the `RREPLAY` LWW logic eventually merges them on the database layer, the physical replication backlogs remain inconsistent. A third node connecting to Node A vs Node B at offset 100 will receive physically different byte streams.
   - **In Long Outages**: If the backlog overflows, the missing offset range forces a full sync. Since the replication engine assumes a single linear source of truth, it flushes the local database rather than merging the branched histories, leading directly to data loss.

---

## 6. Full Sync Mechanism & RDB Metadata

During full sync, multi-master metadata is written to the RDB as snapshot AUX fields and loaded on the target node.

- **`rdbSaveInfo`**: Extends RDB load info with active-active structures (including `mvcc_clock`, `mvcc_key_clock`, `repl_masters`, and `rreplay_seen` arrays).
- **Importing Clock State**: Upon successfully loading the RDB snapshot, the replica calls `replicationApplyRdbMVCCState` to clear local clock hashes and import the clocks from the RDB.
- **Importing Deduplication History**: The replica calls `replicationApplyRdbRReplaySeen` to restore the seen-frame filter table from the RDB.

---

## 7. Outage Recovery & Dynamic Swapping

The multi-master topology manages outages by dynamically swapping between incremental queue buffering and full synchronization requests.

### Outbound Buffering & Handshake
- Outbound updates are queued as `rreplayPendingFrame` structs inside the `replay_pending_frames` list of `valkeyUpstreamRuntime`.
- Connection monitoring runs inside `maintainUpstreamRuntimeLinks` and `ensureUpstreamForwardLink`.
- When the outbound link connects, `connectConfiguredUpstreamForwardLink` performs the handshake:
  - If `replay_fullsync_required` is `0` (short outage), it flushes the pending queue incrementally.
  - If `replay_fullsync_required` is `1` (long outage queue overflow), it triggers a peer full sync.

### Peer-to-Peer Sync Inversion
- If the pending queue exceeds `server.rreplay_pending_max_entries`, the queue is cleared, and `replay_fullsync_required` is set to `1`.
- On reconnection, the node invokes `upstreamRuntimeRequestPeerFullResync` to send a `REPLICAOF <my-ip> <my-port>` command to the peer, forcing the peer to load a full RDB snapshot from this node.

---

## 8. Vulnerability Case Study: Split-Brain Mutual Full-Sync Data Loss

To verify the consequences of these structural gaps under network partitions, a reproducible test was executed inside a sandbox environment.

### 8.1 Step-by-Step Execution Log Trace

#### Step 1: Healthy Baseline
Bidirectional replication is established, and baseline keys are set:
*   **Node A**: `initkeyA = "valA_healthy"`
*   **Node B**: `initkeyB = "valB_healthy"`

#### Step 2: Induce Network Partition
TCP proxies are blocked. Both nodes lose connection to their upstream peer.
*   **Node A Log**:
    ```
    2885621:S 04 Jul 2026 22:08:49.753 # PRIMARY timeout: no data nor PING received...
    2885621:S 04 Jul 2026 22:08:49.754 * Connection with primary lost.
    ```
*   **Node B Log**:
    ```
    2884093:S 04 Jul 2026 22:11:15.427 # PRIMARY timeout: no data nor PING received...
    2884093:S 04 Jul 2026 22:11:15.428 * Connection with primary lost.
    ```

#### Step 3: Partition Writes & Backlog Overflow
We write 150 unique keys (each with 500 bytes of padding value, total command size ~580 bytes) to both Node A (`keyA1..150`) and Node B (`keyB1..150`).
- **Total Payload Size**: 150 keys * ~580 bytes = ~87 KB of replication stream data.
- Since the replication backlog size is configured to **16KB**, the incoming writes occupy multiple internal buffer blocks.
- Valkey's incremental trim algorithm calls `incrementalTrimReplicationBacklog()` to trim older blocks, keeping the history as close to 16KB as possible. This purges the earlier blocks (including the offset where the partition started).
- Node A replication backlog info confirms it rolled over:
  `repl_backlog_first_byte_offset:163521` (History begins long after the partition start offset `83953`).

#### Step 4: Heal Partition & Reconnect
The block files are removed, restoring network paths. Both nodes attempt to reconnect.
*   **Node A Log**:
    ```
    2884092:S 04 Jul 2026 22:11:25.884 * Primary replied to PING, replication can continue...
    2884092:S 04 Jul 2026 22:11:25.905 * Trying a partial resynchronization (request 324dbd44c112ba6d6c9f9a7e0b423e76c76f99d3:83954).
    ```
*   **Node B Log**:
    ```
    2884093:S 04 Jul 2026 22:11:25.886 * Primary replied to PING, replication can continue...
    2884093:S 04 Jul 2026 22:11:25.907 * Trying a partial resynchronization (request 324dbd44c112ba6d6c9f9a7e0b423e76c76f99d3:83797).
    ```

#### Step 5: PSYNC Rejection & Mutual Full Sync Trigger
Because the backlog buffers trimmed the initial partition offsets, the requested offsets (`83954` and `83797`) are no longer present in the peers' backlogs. The nodes reject `PSYNC` and fall back to Full Sync.
*   **Node A Log**:
    ```
    2884092:S 04 Jul 2026 22:11:25.918 * Replica 127.0.0.1:7001 asks for synchronization
    2884092:S 04 Jul 2026 22:11:25.918 * Unable to partial resync with replica 127.0.0.1:7001 for lack of backlog (Replica request was 324dbd44c112ba6d6c9f9a7e0b423e76c76f99d3:83797, and I can only reply with the range [163521, 180124]).
    ```
*   **Node B Log**:
    ```
    2884093:S 04 Jul 2026 22:11:25.916 * Replica 127.0.0.1:7000 asks for synchronization
    2884093:S 04 Jul 2026 22:11:25.916 * Unable to partial resync with replica 127.0.0.1:7000 for lack of backlog (Replica request was 324dbd44c112ba6d6c9f9a7e0b423e76c76f99d3:83954, and I can only reply with the range [143081, 179824]).
    ```

Both nodes concurrently start streaming RDB files to each other.

#### Step 6: Asymmetric Sync Disconnection & Data Loss
1. **Node A completes downloading B's RDB first**:
   At `22:11:30.581`, Node A is ready to load B's incoming RDB.
2. **Node A kills its replica connections**:
   Before loading, Node A runs `replicationAttachToNewPrimary()`, which calls `disconnectReplicas()`. This immediately drops the connection to Node B (acting as A's replica).
   *   **Node A Log**:
       ```
       2884092:S 04 Jul 2026 22:11:30.581 * Connection with replica 127.0.0.1:7001 lost.
       ```
3. **Node B's incoming download is aborted**:
   At that exact moment, Node B is in the middle of downloading Node A's RDB. Due to Node A closing the connection, Node B's socket read fails, and the download is aborted.
   *   **Node B Log**:
       ```
       2884093:S 04 Jul 2026 22:11:49.341 # I/O error reading bulk count from PRIMARY: Success
       2884093:S 04 Jul 2026 22:11:49.342 # Replica bio thread: Error reading sync metadata
       2884093:S 04 Jul 2026 22:11:49.342 # Replica bio thread: Error downloading RDB
       ```
4. **Node A flushes its DB and loads B's data**:
   Node A successfully loads the completed RDB from B, flushing its own database (which wipes `keyA1..150` forever) and loading B's keys (`keyB1..150`).
   *   **Node A Log**:
       ```
       2884092:S 04 Jul 2026 22:11:30.584 * PRIMARY <-> REPLICA sync: Loading DB in memory
       2884092:S 04 Jul 2026 22:11:30.586 * RDB signature and version check passed. Flushing old data
       2884092:S 04 Jul 2026 22:11:30.590 * Done loading RDB, keys loaded: 152, keys expired: 0.
       ```
5. **Node B retains its own database**:
   Because Node B's RDB load was aborted, Node B never executes the database flush. It retains its local database containing `keyB1..150`. Node A's RDB containing `keyA1..150` is discarded.

### 8.2 Keyspace Verification Results

Following reconnection and synchronization cooldown:

| Key | Node A Value | Node B Value | Status | Analysis |
| :--- | :--- | :--- | :--- | :--- |
| **`initkeyA`** | `"valA_healthy"` | `"valA_healthy"` | **✅ Replicated** | Survived because it was in B's RDB and loaded into A. |
| **`initkeyB`** | `"valB_healthy"` | `"valB_healthy"` | **✅ Replicated** | Survived because it was in B's local DB. |
| **`keyA1` – `keyA150`** | *Missing* (nil) | *Missing* (nil) | **⚠️ Lost / Deleted** | **Wiped from existence**. Node A flushed them to load B's RDB; Node B aborted download and never loaded them. |
| **`keyB1` – `keyB150`** | `"valB1_bbb..."` | `"valB1_bbb..."` | **✅ Replicated** | Survived on Node B (due to aborted load) and loaded successfully into Node A. |

This proves that **150 keys written to Node A during the partition were permanently deleted from the cluster**, while B's writes survived. This constitutes absolute data loss in an active-active multi-master setup.

---

## 9. Proposed Mitigations

To resolve this issue, the multi-master replication engine must avoid database flushes during full synchronization:

### Option A: Key-by-Key RDB Merging (LWW-RDB)
- **Concept**: Modify `readSyncBulkPayload()` so that during a multi-master full sync, it does not call `emptyDb()`.
- **Conflict Resolution**: Instead, parse the incoming RDB stream object-by-object. If a key already exists, compare the local metadata (MVCC clock or timestamp + tie-breaker) and apply LWW conflict resolution. Only overwrite/insert the key if the incoming one wins.

### Option B: Isolated Per-Master Replication Backlogs
- **Concept**: Maintain separate replication backlogs for each configured upstream master. On reconnect, query the peer using a vector of offsets to allow delta synchronization even after high write volumes.

### Option C: Lock-Based Sequential Handshakes
- **Concept**: Prevent concurrent bidirectional RDB transfers. Introduce a locking protocol where only one master can load an RDB at a time, ensuring that both transfers complete without early disconnection, followed by a merge step.

---

## 10. Appendix: Test Automation Scripts

The scripts used to reproduce and verify this data loss scenario are located in the repository:

*   **Orchestration Script**: `src/active_active_tests/orchestrator.py`
*   **TCP Proxy Script**: `src/active_active_tests/proxy.py`
*   **Valkey Node A Config**: `src/active_active_tests/node_a.conf`
*   **Valkey Node B Config**: `src/active_active_tests/node_b.conf`

### 10.1 Orchestration Script (`orchestrator.py`)
```python
#!/usr/bin/env python3
import subprocess
import time
import os
import signal
import sys

VALKEY_SERVER_PATH = "../valkey-server"
VALKEY_CLI_PATH = "../valkey-cli"
TEST_DIR = os.path.dirname(os.path.abspath(__file__))
PROXY_PATH = os.path.join(TEST_DIR, "proxy.py")
REPORT_PATH = os.path.join(TEST_DIR, "active_active_test_report.md")

def cleanup_processes():
    print("Cleaning up old valkey and proxy processes...")
    for port in [7000, 7001, 8000, 8001]:
        try:
            output = subprocess.check_output(["lsof", "-t", f"-i:{port}"]).decode().strip()
            for pid in output.split("\n"):
                if pid:
                    print(f"Killing process {pid} on port {port}")
                    os.kill(int(pid), signal.SIGKILL)
        except subprocess.CalledProcessError:
            pass
    for p in [8000, 8001]:
        bf = os.path.join(TEST_DIR, f"block_{p}")
        if os.path.exists(bf):
            os.remove(bf)

def run_cli(port, cmd_args):
    cmd = [VALKEY_CLI_PATH, "-p", str(port)] + [str(x) for x in cmd_args]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return res.returncode, res.stdout, res.stderr

def main():
    cleanup_processes()
    
    # 1. Start Node A and Node B
    print("Starting Node A on Port 7000...")
    node_a_proc = subprocess.Popen(
        [VALKEY_SERVER_PATH, os.path.join(TEST_DIR, "node_a.conf")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    print("Starting Node B on Port 7001...")
    node_b_proc = subprocess.Popen(
        [VALKEY_SERVER_PATH, os.path.join(TEST_DIR, "node_b.conf")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    
    # 2. Start TCP Proxies
    print("Starting Proxy A on Port 8000...")
    proxy_a_proc = subprocess.Popen(
        ["python3", PROXY_PATH, "8000", "127.0.0.1", "7000"],
        stdout=open(os.path.join(TEST_DIR, "proxy_8000.log"), "w"),
        stderr=subprocess.STDOUT
    )
    print("Starting Proxy B on Port 8001...")
    proxy_b_proc = subprocess.Popen(
        ["python3", PROXY_PATH, "8001", "127.0.0.1", "7001"],
        stdout=open(os.path.join(TEST_DIR, "proxy_8001.log"), "w"),
        stderr=subprocess.STDOUT
    )
    
    time.sleep(2)
    
    # 3. Configure replication sequentially to establish stable bidirectional handshake
    print("Wiring replication Node A -> Proxy B...")
    run_cli(7000, ["REPLICAOF", "ADD", "127.0.0.1", "8001"])
    time.sleep(12)
    
    print("Wiring replication Node B -> Proxy A...")
    run_cli(7001, ["REPLICAOF", "ADD", "127.0.0.1", "8000"])
    time.sleep(12)
    
    _, info_a, _ = run_cli(7000, ["INFO", "replication"])
    _, info_b, _ = run_cli(7001, ["INFO", "replication"])
    print("=== Initial Node A replication status ===")
    print(info_a)
    print("=== Initial Node B replication status ===")
    print(info_b)
    
    # 4. Verify baseline replication
    print("Testing baseline replication...")
    run_cli(7000, ["SET", "initkeyA", "valA_healthy"])
    run_cli(7001, ["SET", "initkeyB", "valB_healthy"])
    time.sleep(2)
    
    # 5. Induce Network Partition
    print("Simulating network partition by writing block files...")
    with open(os.path.join(TEST_DIR, "block_8000"), "w") as f:
        f.write("1")
    with open(os.path.join(TEST_DIR, "block_8001"), "w") as f:
        f.write("1")
        
    time.sleep(2)
    
    # 6. Partition writes to overflow 16KB backlog buffers
    print("Writing 150 unique keys to Node A...")
    for i in range(1, 151):
        run_cli(7000, ["SET", f"keyA{i}", f"valA{i}_" + ("a" * 500)])
        
    print("Writing 150 unique keys to Node B...")
    for i in range(1, 151):
        run_cli(7001, ["SET", f"keyB{i}", f"valB{i}_" + ("b" * 500)])
        
    # Wait to ensure repl-timeout triggers and nodes disconnect
    print("Waiting for repl-timeout to trigger and nodes to disconnect (35 seconds)...")
    time.sleep(35)
    
    _, info_a_part, _ = run_cli(7000, ["INFO", "replication"])
    _, info_b_part, _ = run_cli(7001, ["INFO", "replication"])
    print("=== Partition Node A replication status ===")
    print(info_a_part)
    print("=== Partition Node B replication status ===")
    print(info_b_part)
    
    # 7. Heal partition
    print("Healing network partition by removing block files...")
    os.remove(os.path.join(TEST_DIR, "block_8000"))
    os.remove(os.path.join(TEST_DIR, "block_8001"))
    
    # Wait for sync to complete (full sync since backlog overflowed)
    print("Waiting for nodes to reconnect and synchronize (15 seconds)...")
    time.sleep(15)
    
    _, info_a_healed, _ = run_cli(7000, ["INFO", "replication"])
    _, info_b_healed, _ = run_cli(7001, ["INFO", "replication"])
    print("=== Healed Node A replication status ===")
    print(info_a_healed)
    print("=== Healed Node B replication status ===")
    print(info_b_healed)
    
    # 8. Verify data loss
    keys_a = {}
    keys_b = {}
    
    # Read healthy baseline keys
    _, out, _ = run_cli(7000, ["GET", "initkeyA"])
    keys_a["initkeyA"] = out.strip()
    _, out, _ = run_cli(7001, ["GET", "initkeyA"])
    keys_b["initkeyA"] = out.strip()
    
    _, out, _ = run_cli(7000, ["GET", "initkeyB"])
    keys_a["initkeyB"] = out.strip()
    _, out, _ = run_cli(7001, ["GET", "initkeyB"])
    keys_b["initkeyB"] = out.strip()
    
    # Read keyA1-150
    for i in range(1, 151):
        _, out, _ = run_cli(7000, ["GET", f"keyA{i}"])
        keys_a[f"keyA{i}"] = out.strip()
        _, out, _ = run_cli(7001, ["GET", f"keyA{i}"])
        keys_b[f"keyA{i}"] = out.strip()
        
    # Read keyB1-150
    for i in range(1, 151):
        _, out, _ = run_cli(7000, ["GET", f"keyB{i}"])
        keys_a[f"keyB{i}"] = out.strip()
        _, out, _ = run_cli(7001, ["GET", f"keyB{i}"])
        keys_b[f"keyB{i}"] = out.strip()
        
    # Shutdown nodes and proxies
    print("Stopping Valkey nodes...")
    node_a_proc.terminate()
    node_b_proc.terminate()
    proxy_a_proc.terminate()
    proxy_b_proc.terminate()
    
    node_a_proc.wait()
    node_b_proc.wait()
    proxy_a_proc.wait()
    proxy_b_proc.wait()
    
    print("Generating report...")
    generate_report(info_a, info_b, info_a_part, info_b_part, info_a_healed, info_b_healed, keys_a, keys_b)
    print("Done!")

def generate_report(info_a_init, info_b_init, info_a_part, info_b_part, info_a_healed, info_b_healed, keys_a, keys_b):
    # Generates a detailed Markdown report at REPORT_PATH
    pass

if __name__ == "__main__":
    main()
```

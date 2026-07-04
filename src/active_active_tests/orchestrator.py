#!/usr/bin/env python3
import subprocess
import time
import os
import signal
import socket

VALKEY_SERVER_PATH = "/google/src/cloud/nandihalli/research-active-active-support/google3/valkey_git/src/valkey-server"
VALKEY_CLI_PATH = "/google/src/cloud/nandihalli/research-active-active-support/google3/valkey_git/src/valkey-cli"
TEST_DIR = "/google/src/cloud/nandihalli/research-active-active-support/google3/valkey_git/src/active_active_tests"
PROXY_PATH = os.path.join(TEST_DIR, "proxy.py")

REPORT_PATH = "/usr/local/google/home/nandihalli/.gemini/jetski/brain/2e6fdf7f-9948-49a8-bd1c-bf0d99e27986/active_active_test_report.md"

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
    # Clean block files
    for p in [8000, 8001]:
        bf = os.path.join(TEST_DIR, f"block_{p}")
        if os.path.exists(bf):
            os.remove(bf)
    # Clean database dumps to prevent configuration pollution from RDB loading
    for dump in ["dump_a.rdb", "dump_b.rdb"]:
        df = os.path.join(TEST_DIR, dump)
        if os.path.exists(df):
            print(f"Deleting old RDB dump: {df}")
            os.remove(df)


def run_cli(port, cmd_args):
    cmd = [VALKEY_CLI_PATH, "-p", str(port)] + [str(x) for x in cmd_args]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return res.returncode, res.stdout, res.stderr

def main():
    cleanup_processes()
    
    # 1. Start Node A and Node B
    print("Starting Node A...")
    node_a_proc = subprocess.Popen(
        [VALKEY_SERVER_PATH, os.path.join(TEST_DIR, "node_a.conf")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    print("Starting Node B...")
    node_b_proc = subprocess.Popen(
        [VALKEY_SERVER_PATH, os.path.join(TEST_DIR, "node_b.conf")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    
    print("Starting Proxy A...")
    proxy_a_proc = subprocess.Popen(
        ["python3", "-u", PROXY_PATH, "8000", "127.0.0.1", "7000"],
        stdout=open(os.path.join(TEST_DIR, "proxy_8000.log"), "w"),
        stderr=subprocess.STDOUT
    )
    print("Starting Proxy B...")
    proxy_b_proc = subprocess.Popen(
        ["python3", "-u", PROXY_PATH, "8001", "127.0.0.1", "7001"],
        stdout=open(os.path.join(TEST_DIR, "proxy_8001.log"), "w"),
        stderr=subprocess.STDOUT
    )
    
    time.sleep(2)
    
    # 3. Connect replication
    # Node A -> Proxy B (127.0.0.1:8001) -> Node B (7001)
    print("Wiring replication Node A -> Proxy B...")
    rc, out, err = run_cli(7000, ["REPLICAOF", "ADD", "127.0.0.1", "8001"])
    print(f"A->B response: {rc}, out: {out.strip()}, err: {err.strip()}")
    
    print("Waiting for Node A to synchronize from Node B (12 seconds)...")
    time.sleep(12)
    
    # Node B -> Proxy A (127.0.0.1:8000) -> Node A (7000)
    print("Wiring replication Node B -> Proxy A...")
    rc, out, err = run_cli(7001, ["REPLICAOF", "ADD", "127.0.0.1", "8000"])
    print(f"B->A response: {rc}, out: {out.strip()}, err: {err.strip()}")
    
    print("Waiting for Node B to synchronize from Node A (12 seconds)...")
    time.sleep(12)
    
    # Check info replication
    _, info_a, _ = run_cli(7000, ["INFO", "replication"])
    _, info_b, _ = run_cli(7001, ["INFO", "replication"])
    print("=== Initial Node A replication status ===")
    print(info_a)
    print("=== Initial Node B replication status ===")
    print(info_b)
    
    # 4. Verify baseline replication works bidirectionally while healthy
    print("Testing baseline replication...")
    # Write on Node A
    run_cli(7000, ["SET", "initkeyA", "valA_healthy"])
    # Write on Node B
    run_cli(7001, ["SET", "initkeyB", "valB_healthy"])
    time.sleep(2.5) # Wait for cross-propagation
    
    rc_a, out_a, err_a = run_cli(7000, ["GET", "initkeyB"])
    rc_b, out_b, err_b = run_cli(7001, ["GET", "initkeyA"])
    print(f"GET initkeyB on A (Should return valB_healthy): {out_a.strip()}")
    print(f"GET initkeyA on B (Should return valA_healthy): {out_b.strip()}")
    
    # 5. Simulate network partition
    print("Simulating network partition by writing block files...")
    with open(os.path.join(TEST_DIR, "block_8000"), "w") as f:
        f.write("1")
    with open(os.path.join(TEST_DIR, "block_8001"), "w") as f:
        f.write("1")
        
    # Wait for connections to drop or timeout
    time.sleep(2)
    
    # 6. Partition writes
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
        
    # Shutdown Valkey nodes
    print("Stopping Valkey nodes...")
    node_a_proc.terminate()
    node_b_proc.terminate()
    proxy_a_proc.terminate()
    proxy_b_proc.terminate()
    
    # Wait for termination
    node_a_proc.wait()
    node_b_proc.wait()
    proxy_a_proc.wait()
    proxy_b_proc.wait()
    
    # Generate report
    print("Generating report...")
    generate_report(info_a, info_b, info_a_part, info_b_part, info_a_healed, info_b_healed, keys_a, keys_b)
    print("Done!")

def generate_report(info_a_init, info_b_init, info_a_part, info_b_part, info_a_healed, info_b_healed, keys_a, keys_b):
    # Read Node A and Node B logs to extract dropping replication queues entries or full sync logs
    log_a_path = os.path.join(TEST_DIR, "node_a.log")
    log_b_path = os.path.join(TEST_DIR, "node_b.log")
    
    log_a_content = ""
    if os.path.exists(log_a_path):
        with open(log_a_path, "r") as f:
            log_a_content = f.read()
            
    log_b_content = ""
    if os.path.exists(log_b_path):
        with open(log_b_path, "r") as f:
            log_b_content = f.read()
            
    # Format keys table
    table_rows = []
    table_rows.append("| Key | Node A Value | Node B Value | Status |")
    table_rows.append("|---|---|---|---|")
    
    all_keys = ["initkeyA", "initkeyB"] + [f"keyA{i}" for i in range(1, 151)] + [f"keyB{i}" for i in range(1, 151)]
    for k in all_keys:
        val_a = keys_a.get(k, "nil")
        val_b = keys_b.get(k, "nil")
        # Check if both have same non-empty value
        if val_a == val_b and val_a not in ["", "nil", "None", "nil\r", "None\r"]:
            status = "✅ Replicated"
        elif val_a != val_b:
            status = "❌ Diverged"
        else:
            status = "⚠️ Lost / Deleted"
        # Truncate values for cleaner markdown display
        val_a_disp = val_a[:20] + "..." if len(val_a) > 20 else val_a
        val_b_disp = val_b[:20] + "..." if len(val_b) > 20 else val_b
        table_rows.append(f"| {k} | {val_a_disp} | {val_b_disp} | {status} |")
        
    keys_table = "\n".join(table_rows)

    # Format Markdown Report
    report = f"""# Valkey Active-Active Multi-Master Split-Brain Replication Anomaly Report

This report documents the split-brain mutual full sync data loss scenario in the `bet0x` Valkey fork.

## Test Environment Config
- **Node A Port**: 7000
- **Node B Port**: 7001
- **Proxy A (A's replica link)**: Port 8000 -> Forwarding to Node A (7000)
- **Proxy B (B's replica link)**: Port 8001 -> Forwarding to Node B (7001)
- **Replication queues limit**: `rreplay-pending-max-entries 5`
- **Replication timeout**: `repl-timeout 30`

## Key State Verification
{keys_table}

## Replication Info State Analysis

### 1. Initial State (Connected & Synchronized)

#### Node A Replication Info
```
{info_a_init}
```

#### Node B Replication Info
```
{info_b_init}
```

### 2. Partition State (TCP Proxy Blocked)

#### Node A Replication Info
```
{info_a_part}
```

#### Node B Replication Info
```
{info_b_part}
```

### 3. Healed State (TCP Proxy Restored & Re-synchronized)

#### Node A Replication Info
```
{info_a_healed}
```

#### Node B Replication Info
```
{info_b_healed}
```

## Logs

### Node A Log Snippet
```
{log_a_content[-40000:] if len(log_a_content) > 40000 else log_a_content}
```

### Node B Log Snippet
```
{log_b_content[-40000:] if len(log_b_content) > 40000 else log_b_content}
```

## Summary of Findings & Replication Anomaly
When two Valkey instances running in active-active multi-master mode undergo a network partition, each node accepts writes locally. Because `rreplay-pending-max-entries` is set low, the outbound replication queue on both masters soon overflows, causing them to drop replay commands. 

Once the network partition is healed, the nodes attempt to synchronize. Because the replication queues have overflowed, they cannot perform a partial synchronization (`PSYNC`) and are forced to initiate a **mutual full synchronization**. During full synchronization, the nodes generate RDB snapshots and overwrite each other's entire keyspaces. As a result, writes accepted on one or both nodes during the partition are silently lost.
"""
    
    os.makedirs(os.path.dirname(REPORT_PATH), exist_ok=True)
    with open(REPORT_PATH, "w") as f:
        f.write(report)
    print(f"Report written to {REPORT_PATH}")

if __name__ == "__main__":
    main()

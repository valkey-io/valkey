# Valkey Multi-Master Setup (Experimental)

This document explains how to run an **experimental** Multi-Master lab with Valkey.

## 1) Scope

This setup is for validation/testing, not a production rollout guide.

Implemented behavior in this branch includes:
1. Multi-master replication with RW peers (`active-replica yes` + `multi-master yes`).
2. `RREPLAY` forwarding with anti-loop and dedupe behavior.
3. MVCC/LWW conflict handling.
4. Runtime observability and replay counters in `INFO replication`.

## 2) Build is required

You must compile first.

```bash
cd valkey
make -j"$(nproc)"
```

Before continuing, confirm these binaries are available:
- `valkey-server`
- `valkey-cli`

## 3) Local topology (3 masters + optional read replicas)

- `Master A` RW: `127.0.0.1:6379`
- `Master B` RW: `127.0.0.1:6380`
- `Master C` RW: `127.0.0.1:6383`
- `Replica A1` read-only: `127.0.0.1:6381` (follows A, optional)
- `Replica B1` read-only: `127.0.0.1:6382` (follows B, optional)
- `Replica C1` read-only: `127.0.0.1:6384` (follows C, optional)

Create lab dirs:

```bash
mkdir -p /tmp/valkey-mm/{a,b,c,r1,r2,r3}
```

## 4) Minimal configs

### Master A (`/tmp/valkey-mm/a/valkey.conf`)

```conf
port 6379
bind 127.0.0.1
protected-mode no
dir /tmp/valkey-mm/a
dbfilename dump.rdb
appendonly no
replica-read-only no
active-replica yes
multi-master yes
multi-master-no-forward no
```

### Master B (`/tmp/valkey-mm/b/valkey.conf`)

```conf
port 6380
bind 127.0.0.1
protected-mode no
dir /tmp/valkey-mm/b
dbfilename dump.rdb
appendonly no
replica-read-only no
active-replica yes
multi-master yes
multi-master-no-forward no
```

### Master C (`/tmp/valkey-mm/c/valkey.conf`)

```conf
port 6383
bind 127.0.0.1
protected-mode no
dir /tmp/valkey-mm/c
dbfilename dump.rdb
appendonly no
replica-read-only no
active-replica yes
multi-master yes
multi-master-no-forward no
```

### Replica A1 (`/tmp/valkey-mm/r1/valkey.conf`)

```conf
port 6381
bind 127.0.0.1
protected-mode no
dir /tmp/valkey-mm/r1
dbfilename dump.rdb
appendonly no
replica-read-only yes
```

### Replica B1 (`/tmp/valkey-mm/r2/valkey.conf`)

```conf
port 6382
bind 127.0.0.1
protected-mode no
dir /tmp/valkey-mm/r2
dbfilename dump.rdb
appendonly no
replica-read-only yes
```

### Replica C1 (`/tmp/valkey-mm/r3/valkey.conf`)

```conf
port 6384
bind 127.0.0.1
protected-mode no
dir /tmp/valkey-mm/r3
dbfilename dump.rdb
appendonly no
replica-read-only yes
```

## 5) Start nodes

Run one process per terminal:

```bash
valkey-server /tmp/valkey-mm/a/valkey.conf
valkey-server /tmp/valkey-mm/b/valkey.conf
valkey-server /tmp/valkey-mm/c/valkey.conf
valkey-server /tmp/valkey-mm/r1/valkey.conf
valkey-server /tmp/valkey-mm/r2/valkey.conf
valkey-server /tmp/valkey-mm/r3/valkey.conf
```

## 6) Wire replication

Connect masters as full mesh peers:

```bash
valkey-cli -p 6379 REPLICAOF ADD 127.0.0.1 6380
valkey-cli -p 6379 REPLICAOF ADD 127.0.0.1 6383
valkey-cli -p 6380 REPLICAOF ADD 127.0.0.1 6379
valkey-cli -p 6380 REPLICAOF ADD 127.0.0.1 6383
valkey-cli -p 6383 REPLICAOF ADD 127.0.0.1 6379
valkey-cli -p 6383 REPLICAOF ADD 127.0.0.1 6380
```

Connect read replicas:

```bash
valkey-cli -p 6381 REPLICAOF 127.0.0.1 6379
valkey-cli -p 6382 REPLICAOF 127.0.0.1 6380
valkey-cli -p 6384 REPLICAOF 127.0.0.1 6383
```

## 7) Quick checks

Replication state:

```bash
valkey-cli -p 6379 INFO replication
valkey-cli -p 6380 INFO replication
valkey-cli -p 6383 INFO replication
```

Expected fields (among others):
- `active_replica:1`
- `multi_master:1`
- `configured_upstreams`
- `connected_masters`
- `upstream_runtime_entries`
- `active_upstream_runtime_links`
- `upstream_runtime_replay_tx_frames`
- `upstream_runtime_replay_rx_frames`
- `upstream_runtime_replay_ack_frames`
- `upstream_runtime_replay_backlog`

Role output:

```bash
valkey-cli -p 6379 ROLE
valkey-cli -p 6380 ROLE
valkey-cli -p 6383 ROLE
```

Cross-write validation:

```bash
valkey-cli -p 6379 SET mm:key from-A
valkey-cli -p 6380 GET mm:key

valkey-cli -p 6380 SET mm:key from-B
valkey-cli -p 6379 GET mm:key

valkey-cli -p 6383 SET mm:key from-C
valkey-cli -p 6379 GET mm:key
```

Replica validation:

```bash
valkey-cli -p 6381 GET mm:key
valkey-cli -p 6382 GET mm:key
valkey-cli -p 6384 GET mm:key
```

## 8) Useful ops

```bash
valkey-cli -p 6379 REPLICAOF ADD 127.0.0.1 6380
valkey-cli -p 6379 REPLICAOF REMOVE 127.0.0.1 6380
valkey-cli -p 6379 REPLICAOF NO ONE
valkey-cli -p 6379 CONFIG SET multi-master-no-forward yes
valkey-cli -p 6379 CONFIG SET multi-master-no-forward no
```

## 9) Notes

1. There is no single global leader in MM mode; both masters are RW.
2. Replication is asynchronous.
3. During network churn/partitions, convergence depends on MVCC/LWW + replay dedupe windows.
4. Treat this guide as an experimental validation setup.

PS: Are you guys getting weekends?

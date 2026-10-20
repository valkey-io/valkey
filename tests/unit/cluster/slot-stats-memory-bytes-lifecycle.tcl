# Integration tests for CLUSTER SLOT-STATS memory-bytes: the counter must follow
# the data, not slot ownership or the DB that is flushed. Covers replica full
# sync, diskless load with swapdb, failover, and FLUSHDB with several databases.

source tests/unit/cluster/slot-stats-memory-bytes-helpers.tcl

# Writes a mix of strings (with and without expire) and a stream to the slot of
# {FOO} on node `n`.
proc write_memory_bytes_dataset {n} {
    R $n SET "{FOO}s1" [string repeat a 20]
    R $n SET "{FOO}s2" [string repeat b 100] EX 1000
    R $n SET "{FOO}s3" 12345
    for {set i 0} {$i < 100} {incr i} {
        R $n XADD "{FOO}x" "*" f [string repeat x 30]
    }
}

# -----------------------------------------------------------------------------
# Replication and failover.
# -----------------------------------------------------------------------------

start_cluster 1 1 {tags {external:skip cluster}} {
    set primary [srv 0 client]
    set replica [srv -1 client]
    set key_slot [R 0 cluster keyslot "{FOO}"]
    R 1 READONLY

    test "memory-bytes: replica matches primary after full sync" {
        wait_for_cluster_state ok
        write_memory_bytes_dataset 0
        wait_for_ofs_sync $primary $replica
        assert_slot_bytes_match_memory_usage $key_slot 0
        assert_slot_bytes_match_memory_usage $key_slot 1
        assert_equal [get_slot_bytes $key_slot 0] [get_slot_bytes $key_slot 1]
    }

    # With swapdb, the replica loads into a temp DB and then drops the old
    # dataset. The old dataset must not stay in the counter. The replica is not
    # compared with the primary here: after an RDB load the same data can take a
    # different number of allocated bytes (for example with ASan, where the
    # allocator reports the exact requested size).
    test "memory-bytes: replica does not keep old bytes after a swapdb diskless load" {
        R 1 CONFIG SET repl-diskless-load swapdb
        R 0 CONFIG SET repl-diskless-sync yes
        R 0 CONFIG SET repl-diskless-sync-delay 0
        set syncs [s 0 sync_full]

        # A new replication ID forces a full sync on reconnect.
        R 0 DEBUG CHANGE-REPL-ID
        R 1 CLIENT KILL TYPE MASTER
        wait_for_condition 100 100 {
            [s 0 sync_full] > $syncs && [s -1 master_link_status] eq {up}
        } else {
            fail "replica did not full sync"
        }
        wait_for_ofs_sync $primary $replica
        assert_slot_bytes_match_memory_usage $key_slot 1
    } {} {needs:debug}

    # A failover runs clusterDelSlot / clusterAddSlot on the promoted replica
    # while it keeps all the keys.
    test "memory-bytes: promoted replica keeps memory-bytes after failover" {
        set expected [get_slot_bytes $key_slot 1]
        assert {$expected > 0}
        R 1 CLUSTER FAILOVER
        wait_for_condition 1000 50 {
            [s -1 role] eq {master} && [s 0 role] eq {slave}
        } else {
            fail "failover did not happen"
        }
        assert_equal $expected [get_slot_bytes $key_slot 1]
        assert_slot_bytes_match_memory_usage $key_slot 1

        # Deleting the keys on the new primary must bring the slot back to 0.
        R 1 DEL "{FOO}s1" "{FOO}s2" "{FOO}s3" "{FOO}x"
        assert_equal 0 [get_slot_bytes $key_slot 1]
    }
}

# -----------------------------------------------------------------------------
# Several databases. The slot total is the sum over all databases, and FLUSHDB
# only removes the bytes of the flushed database.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster} overrides {cluster-databases 16}} {
    set key_slot [R 0 cluster keyslot "{FOO}"]

    test "memory-bytes: slot total is the sum over all databases" {
        R 0 FLUSHALL
        R 0 SELECT 0
        R 0 SET "{FOO}k" [string repeat a 50]
        set db0_bytes [R 0 MEMORY USAGE "{FOO}k" SAMPLES 0]
        R 0 SELECT 1
        R 0 SET "{FOO}k" [string repeat b 200]
        set db1_bytes [R 0 MEMORY USAGE "{FOO}k" SAMPLES 0]
        assert_equal [expr {$db0_bytes + $db1_bytes}] [get_slot_bytes $key_slot]

        R 0 FLUSHDB
        assert_equal $db0_bytes [get_slot_bytes $key_slot]

        R 0 SELECT 0
        R 0 FLUSHDB ASYNC
        assert_equal 0 [get_slot_bytes $key_slot]
    }

    # Returns the sum of MEMORY USAGE over `keys` in databases 0 and 1.
    proc usage_db01 {keys} {
        set total 0
        foreach db {0 1} {
            R 0 SELECT $db
            foreach k $keys {
                if {[R 0 EXISTS $k]} {incr total [R 0 MEMORY USAGE $k SAMPLES 0]}
            }
        }
        R 0 SELECT 0
        return $total
    }

    # MOVE looks up the same key name in two databases in one command. The
    # snapshot entries must be per database, or the source slot is counted twice.
    # A second key stays in db 0, so the source hashtable is not freed.
    test "memory-bytes: MOVE to another database" {
        R 0 FLUSHALL
        R 0 SELECT 0
        R 0 SET "{FOO}stay" [string repeat a 100]
        R 0 SET "{FOO}k" [string repeat b 100]
        assert_equal 1 [R 0 MOVE "{FOO}k" 1]
        assert_equal [usage_db01 {"{FOO}stay" "{FOO}k"}] [get_slot_bytes $key_slot]
    }

    test "memory-bytes: MOVE with an expire" {
        R 0 FLUSHALL
        R 0 SELECT 0
        R 0 SET "{FOO}stay" [string repeat a 100]
        R 0 SET "{FOO}k" [string repeat b 100] EX 1000
        assert_equal 1 [R 0 MOVE "{FOO}k" 1]
        assert_equal [usage_db01 {"{FOO}stay" "{FOO}k"}] [get_slot_bytes $key_slot]
    }

    test "memory-bytes: MOVE REPLACE over an existing key" {
        R 0 FLUSHALL
        R 0 SELECT 1
        R 0 SET "{FOO}k" [string repeat c 30]
        R 0 SET "{FOO}stay1" [string repeat d 100]
        R 0 SELECT 0
        R 0 SET "{FOO}stay" [string repeat a 100]
        R 0 SET "{FOO}k" [string repeat b 300]
        assert_equal 1 [R 0 MOVE "{FOO}k" 1 REPLACE]
        assert_equal [usage_db01 {"{FOO}stay" "{FOO}stay1" "{FOO}k"}] [get_slot_bytes $key_slot]
    }
}

# -----------------------------------------------------------------------------
# AOF load. Commands are replayed without call(), so the AOF loop drops the
# snapshots itself.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster} overrides {appendonly yes appendfsync always}} {
    set key_slot [R 0 cluster keyslot "{FOO}"]

    test "memory-bytes: matches MEMORY USAGE after DEBUG LOADAOF" {
        write_memory_bytes_dataset 0
        R 0 APPEND "{FOO}s1" [string repeat c 300]
        R 0 SET "{FOO}s1" x NX
        R 0 HDEL "{FOO}missing" f
        R 0 DEBUG LOADAOF
        assert_slot_bytes_match_memory_usage $key_slot
        assert {[get_slot_bytes $key_slot] > 0}
    } {} {needs:debug}
}

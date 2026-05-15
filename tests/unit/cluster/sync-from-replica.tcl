# Tests for the sync-from-replica feature (issue #2767).
#
# Topology: start_cluster 2 2 -> 4 nodes total.
#   Node 0 = primary (shard 0, owns slots 0-8191)
#   Node 1 = primary (shard 1, owns slots 8192-16383)
#   Node 2 = replica of primary 0
#   Node 3 = replica of primary 1
#
# Test pattern: detach node 3 from primary 1, then CLUSTER REPLICATE to
# primary 0. With repl-prefer-sync-from-replica enabled, node 3 should get its
# RDB from the existing sibling (node 2) instead of primary 0.

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Retrieve an INFO field from cluster node idx.
proc get_info {idx field} {
    getInfoProperty [R $idx info all] $field
}

# Return the cluster node-id that a given instance replicates, per CLUSTER NODES.
proc get_cluster_primary_id {idx} {
    set nodes [R $idx cluster nodes]
    foreach line [split $nodes "\n"] {
        if {[string match "*myself*" $line]} {
            set fields [split $line " "]
            set primary_id [lindex $fields 3]
            if {$primary_id eq "-"} { return "" }
            return $primary_id
        }
    }
    return ""
}

# Wait until an instance is a replica of a specific node-id and has role=slave.
proc wait_replica_of {idx target_node_id {maxtries 200} {delay 100}} {
    wait_for_condition $maxtries $delay {
        [get_cluster_primary_id $idx] eq $target_node_id &&
        [get_info $idx role] eq {slave}
    } else {
        fail "Node $idx did not become replica of $target_node_id in time"
    }
}

# Wait until an instance finishes sync and any transient sibling handoff clears.
proc wait_node_synced {idx {maxtries 300} {delay 100}} {
    wait_for_condition $maxtries $delay {
        [get_info $idx master_link_status] eq {up} &&
        ([get_info $idx sync_from_replica_in_progress] eq "" ||
         [get_info $idx sync_from_replica_in_progress] == 0)
    } else {
        fail "Node $idx did not finish sync in time"
    }
}

# Check if the sync-from-replica feature is fully implemented by looking for
# the INFO field it exposes. Returns 1 if implemented, 0 if not.
proc is_sync_from_replica_implemented {} {
    # Test on node 3 (replica) since the feature is used by replicas.
    set code [catch {R 3 config set repl-prefer-sync-from-replica yes} err]
    if {$code != 0} { return 0 }
    set info [R 3 info replication]
    set val [getInfoProperty $info "sync_from_replica_in_progress"]
    if {$val eq ""} {
        catch {R 3 config set repl-prefer-sync-from-replica no}
        return 0
    }
    # Leave feature enabled; callers expect it on after this check.
    return 1
}

proc wait_sync_from_replica_started {idx {maxtries 100} {delay 100}} {
    wait_for_condition $maxtries $delay {
        [get_info $idx sync_from_replica_in_progress] == 1
    } else {
        fail "Node $idx did not start sync-from-replica in time"
    }
}

proc wait_sync_from_replica_done {idx {maxtries 200} {delay 100}} {
    wait_for_condition $maxtries $delay {
        [get_info $idx sync_from_replica_in_progress] == 0
    } else {
        fail "Node $idx did not finish sync-from-replica in time"
    }
}

# Detach node 3 from its current primary and wait for it to become a primary.
proc detach_node3 {} {
    catch {R 3 cluster replicate no one}
    wait_for_condition 100 100 {
        [string match "*master*" [dict get [get_myself 3] flags]]
    } else {
        fail "Node 3 did not transition to primary after detach"
    }
}

# Return the "myself" line from CLUSTER NODES as a dict.
proc get_myself {idx} {
    set nodes [R $idx cluster nodes]
    foreach line [split $nodes "\n"] {
        if {[string match "*myself*" $line]} {
            set fields [split [string trim $line] " "]
            return [dict create id [lindex $fields 0] flags [lindex $fields 2] role [lindex $fields 3]]
        }
    }
    return {}
}

# ---------------------------------------------------------------------------
# All tests in a single start_cluster block to avoid inter-block cleanup issues.
# ---------------------------------------------------------------------------

start_cluster 2 2 {tags {external:skip cluster} overrides {cluster-node-timeout 3000}} {

    # ===== CONFIG DISABLED =====

    test "CONFIG DISABLED - Normal full sync when feature is off" {
        wait_for_cluster_state ok

        for {set i 0} {$i < 100} {incr i} {
            R 0 set "{tag0}cfg:$i" "val:$i"
        }
        wait_node_synced 2

        set p0_sync_full_before [get_info 0 sync_full]
        set s2_sync_full_before [get_info 2 sync_full]

        # Detach node 3 from primary 1.
        detach_node3

        # Feature disabled: CLUSTER REPLICATE should use normal replication.
        set primary0_id [R 0 cluster myid]
        R 3 cluster replicate $primary0_id

        wait_replica_of 3 $primary0_id
        wait_node_synced 3

        # Primary 0 should have done BGSAVE for normal full sync.
        assert {[get_info 0 sync_full] > $p0_sync_full_before}

        # Sibling 2 should not have done BGSAVE.
        assert_equal $s2_sync_full_before [get_info 2 sync_full] \
            "Sibling sync_full should not change -- feature disabled"

        # Data should be correct on node 3.
        R 3 readonly
        for {set i 0} {$i < 100} {incr i} {
            assert_equal "val:$i" [R 3 get "{tag0}cfg:$i"]
        }
    }

    # ===== HAPPY PATH (requires feature config) =====

    test "HAPPY PATH - Sync from sibling replica avoids BGSAVE on primary" {
        # Write test data to primary 0.
        for {set i 0} {$i < 500} {incr i} {
            R 0 set "{tag0}key:$i" "value:$i"
        }

        # Wait for sibling (node 2, replica of primary 0) to be fully synced.
        wait_node_synced 2

        # Record sync_full on primary 0 and sibling 2 before the new node joins.
        set p0_sync_full_before [get_info 0 sync_full]
        set s2_sync_full_before [get_info 2 sync_full]

        # Detach node 3 so it can re-join fresh.
        detach_node3

        # Enable the feature on the joining node.
        if {![is_sync_from_replica_implemented]} {
            skip "repl-prefer-sync-from-replica config is unavailable"
        }
        R 3 config set repl-prefer-sync-from-replica yes

        # Node 3 joins as replica of primary 0.
        set primary0_id [R 0 cluster myid]
        R 3 cluster replicate $primary0_id

        # Wait for node 3 to finish syncing.
        wait_replica_of 3 $primary0_id
        wait_node_synced 3

        # Primary 0 should not have done a new BGSAVE.
        assert_equal $p0_sync_full_before [get_info 0 sync_full] \
            "Primary sync_full should not increase -- no BGSAVE on primary"

        # Sibling 2 should have done a BGSAVE.
        assert_equal [expr $s2_sync_full_before + 1] [get_info 2 sync_full] \
            "Sibling sync_full should increase by 1 -- BGSAVE on sibling"

        # Node 3 should have correct data.
        R 3 readonly
        for {set i 0} {$i < 500} {incr i} {
            assert_equal "value:$i" [R 3 get "{tag0}key:$i"]
        }

        # Cluster topology: node 3 shows as replica of primary 0.
        assert_equal $primary0_id [get_cluster_primary_id 3]
    }

    test "HAPPY PATH - Ongoing replication works after sync-from-replica" {
        if {![is_sync_from_replica_implemented]} {
            skip "repl-prefer-sync-from-replica config is unavailable"
        }

        for {set i 500} {$i < 600} {incr i} {
            R 0 set "{tag0}key:$i" "value:$i"
        }

        set offset [get_info 0 master_repl_offset]
        wait_for_condition 100 100 {
            [get_info 3 master_repl_offset] >= $offset
        } else {
            fail "Node 3 did not catch up to primary after sync"
        }

        R 3 readonly
        for {set i 500} {$i < 600} {incr i} {
            assert_equal "value:$i" [R 3 get "{tag0}key:$i"]
        }
    }

    test "HAPPY PATH - After sync, node 3 replicates from primary 0, not sibling" {
        if {![is_sync_from_replica_implemented]} {
            skip "repl-prefer-sync-from-replica config is unavailable"
        }

        # Verify via cluster topology that node 3's primary is node 0,
        # not the sibling (node 2). Port-based checks are unreliable
        # across TLS/non-TLS modes.
        set primary0_id [R 0 cluster myid]
        assert_equal $primary0_id [get_cluster_primary_id 3]
    }

    # ===== DATA CONSISTENCY (requires feature config) =====

    test "DATA CONSISTENCY - All data types survive sync-from-replica" {
        # Write various data types to primary 0.
        R 0 set "{tag0}string_key" "hello"
        R 0 hset "{tag0}hash_key" field1 val1 field2 val2
        R 0 lpush "{tag0}list_key" a b c
        R 0 sadd "{tag0}set_key" m1 m2 m3
        R 0 zadd "{tag0}zset_key" 1 a 2 b 3 c
        R 0 xadd "{tag0}stream_key" "*" field value
        R 0 set "{tag0}expiry_key" "expires" px 300000
        R 0 incr "{tag0}counter_key"
        R 0 incr "{tag0}counter_key"
        R 0 incr "{tag0}counter_key"

        wait_node_synced 2

        detach_node3

        if {![is_sync_from_replica_implemented]} {
            skip "repl-prefer-sync-from-replica config is unavailable"
        }

        set primary0_id [R 0 cluster myid]
        R 3 cluster replicate $primary0_id

        wait_replica_of 3 $primary0_id
        wait_node_synced 3

        R 3 readonly

        assert_equal "hello" [R 3 get "{tag0}string_key"]
        assert_equal "val1" [R 3 hget "{tag0}hash_key" field1]
        assert_equal "val2" [R 3 hget "{tag0}hash_key" field2]
        assert_equal "c" [R 3 lindex "{tag0}list_key" 0]
        assert_equal "a" [R 3 lindex "{tag0}list_key" 2]
        assert_equal 3 [R 3 llen "{tag0}list_key"]
        assert_equal 3 [R 3 scard "{tag0}set_key"]
        assert {[R 3 sismember "{tag0}set_key" m1] == 1}
        assert_equal 3 [R 3 zcard "{tag0}zset_key"]
        assert_equal "a" [lindex [R 3 zrange "{tag0}zset_key" 0 0] 0]
        assert {[R 3 xlen "{tag0}stream_key"] >= 1}
        assert {[R 3 pttl "{tag0}expiry_key"] > 0}
        assert_equal 3 [R 3 get "{tag0}counter_key"] \
            "Counter should be exactly 3 -- no double-apply from sync"
    }

    # ===== TOPOLOGY CORRECTNESS (requires feature config) =====

    test "TOPOLOGY - Gossip shows new node as replica of P, never of S" {
        # Previous test left node 3 as replica of primary 0. Check gossip.
        if {![is_sync_from_replica_implemented]} {
            skip "repl-prefer-sync-from-replica config is unavailable"
        }

        set primary0_id [R 0 cluster myid]
        set sibling2_id [R 2 cluster myid]
        set node3_id [R 3 cluster myid]

        after 2000 ;# allow gossip to propagate

        foreach observer {0 1 2 3} {
            set nodes [R $observer cluster nodes]
            foreach line [split $nodes "\n"] {
                if {[string match "*$node3_id*" $line]} {
                    set fields [split $line " "]
                    set master_field [lindex $fields 3]
                    assert_equal $primary0_id $master_field \
                        "Observer $observer sees node 3 as replica of wrong node"
                    assert {$master_field ne $sibling2_id}
                    break
                }
            }
        }
    }

    test "TOPOLOGY - DBSIZE matches between primary and new replica" {
        if {![is_sync_from_replica_implemented]} {
            skip "repl-prefer-sync-from-replica config is unavailable"
        }
        assert_equal [R 0 dbsize] [R 3 dbsize]
    }

    # ===== SECOND SYNC CYCLE (catches C3: stale repl_rdb_channel_state) =====

    test "SECOND SYNC CYCLE - repeated sync-from-replica works correctly" {
        if {![is_sync_from_replica_implemented]} {
            skip "repl-prefer-sync-from-replica config is unavailable"
        }

        # At this point node 3 is a replica of primary 0 from a previous test.
        # Write fresh data so we can distinguish this cycle's state.
        for {set i 0} {$i < 200} {incr i} {
            R 0 set "{tag0}cycle2:$i" "second:$i"
        }

        # Wait for node 3 to have the data (it's currently synced to P0).
        set offset [get_info 0 master_repl_offset]
        wait_for_condition 100 100 {
            [get_info 3 master_repl_offset] >= $offset
        } else {
            fail "Node 3 did not catch up before second cycle"
        }

        # Record sync_full counters before second cycle.
        set p0_sync_full_before [get_info 0 sync_full]
        set s2_sync_full_before [get_info 2 sync_full]

        # Detach node 3 and re-attach to primary 0 with sync-from-replica.
        detach_node3
        R 3 config set repl-prefer-sync-from-replica yes

        set primary0_id [R 0 cluster myid]
        R 3 cluster replicate $primary0_id

        wait_replica_of 3 $primary0_id
        wait_node_synced 3

        # Primary 0 should not have done a new BGSAVE in the second cycle.
        assert_equal $p0_sync_full_before [get_info 0 sync_full] \
            "Second cycle: primary sync_full should not increase"

        # Sibling 2 should have done a BGSAVE for the second cycle.
        assert_equal [expr $s2_sync_full_before + 1] [get_info 2 sync_full] \
            "Second cycle: sibling sync_full should increase by 1"

        # Data from both cycles should be present.
        R 3 readonly
        for {set i 0} {$i < 200} {incr i} {
            assert_equal "second:$i" [R 3 get "{tag0}cycle2:$i"]
        }
        # Original data from the happy-path test should also be present.
        assert_equal "value:0" [R 3 get "{tag0}key:0"]
        assert_equal "value:499" [R 3 get "{tag0}key:499"]

        assert_equal [R 0 dbsize] [R 3 dbsize] \
            "Second cycle: DBSIZE should match"
    }

    # ===== WRITES DURING SYNC - delayed sibling stream drain =====

    test "WRITES DURING SYNC - keys written to P during sibling RDB are not lost" {
        if {![is_sync_from_replica_implemented]} {
            skip "repl-prefer-sync-from-replica config is unavailable"
        }

        set old_backlog_size [lindex [R 0 config get repl-backlog-size] 1]
        R 0 config set repl-backlog-size 1kb

        # Slow down sibling's BGSAVE so writes accumulate behind the RDB.
        R 2 config set rdb-key-save-delay 500

        # Record counters before this cycle.
        set p0_sync_full_before [get_info 0 sync_full]

        detach_node3
        R 3 config set repl-prefer-sync-from-replica yes

        set primary0_id [R 0 cluster myid]
        R 3 cluster replicate $primary0_id

        # Wait for sibling to start BGSAVE (confirms N connected to S for RDB).
        wait_for_condition 100 100 {
            [get_info 2 rdb_bgsave_in_progress] == 1
        } else {
            fail "Sibling did not start BGSAVE for writes-during-sync test"
        }

        # Write 1000 keys to primary during the sibling RDB transfer.
        # These writes reach S during the RDB transfer. N must apply them from
        # S before switching back to P, even when P's backlog is tiny.
        for {set i 0} {$i < 1000} {incr i} {
            R 0 set "{tag0}during:$i" "written:$i"
        }
        # Also test non-idempotent commands (INCR) to detect double-apply.
        R 0 set "{tag0}incr_test" 0
        for {set i 0} {$i < 500} {incr i} {
            R 0 incr "{tag0}incr_test"
        }

        # Restore normal BGSAVE speed and wait for sync to complete.
        R 2 config set rdb-key-save-delay 0
        wait_replica_of 3 $primary0_id 500 200
        wait_node_synced 3 500 200
        wait_sync_from_replica_done 3 500 200
        R 0 config set repl-backlog-size $old_backlog_size

        # Primary should not have done BGSAVE.
        assert_equal $p0_sync_full_before [get_info 0 sync_full] \
            "Writes-during-sync: primary sync_full should not increase"

        # All keys written during sync must be present on node 3.
        R 3 readonly
        for {set i 0} {$i < 1000} {incr i} {
            assert_equal "written:$i" [R 3 get "{tag0}during:$i"]
        }

        # INCR counter must be exactly 500 (no double-apply, no loss).
        assert_equal "500" [R 3 get "{tag0}incr_test"] \
            "INCR counter should be exactly 500 -- no double-apply or loss"

        # DBSIZE must match.
        assert_equal [R 0 dbsize] [R 3 dbsize] \
            "Writes-during-sync: DBSIZE should match"
    }

    # ===== CONCURRENT WRITES - before, during, and after sync =====

    test "CONCURRENT WRITES - before, during, and after sync all present" {
        if {![is_sync_from_replica_implemented]} {
            skip "repl-prefer-sync-from-replica config is unavailable"
        }

        # Phase A: 100 keys before starting the sync.
        for {set i 0} {$i < 100} {incr i} {
            R 0 set "{tag0}cw:before:$i" "before:$i"
        }
        wait_node_synced 2

        # Detach, enable, slow BGSAVE on sibling.
        detach_node3
        R 3 config set repl-prefer-sync-from-replica yes
        R 2 config set rdb-key-save-delay 500

        set primary0_id [R 0 cluster myid]
        R 3 cluster replicate $primary0_id

        # Phase B: 200 keys during the sync.
        for {set i 0} {$i < 200} {incr i} {
            R 0 set "{tag0}cw:during:$i" "during:$i"
        }
        # Non-idempotent: INCR 50 times + LPUSH 5 elements.
        R 0 set "{tag0}cw:counter" 0
        for {set i 0} {$i < 50} {incr i} {
            R 0 incr "{tag0}cw:counter"
        }
        R 0 lpush "{tag0}cw:list" a b c d e

        # Let sync complete.
        R 2 config set rdb-key-save-delay 0
        wait_replica_of 3 $primary0_id
        wait_node_synced 3

        # Phase C: 50 keys after sync for ongoing replication check.
        for {set i 0} {$i < 50} {incr i} {
            R 0 set "{tag0}cw:after:$i" "after:$i"
        }
        set offset [get_info 0 master_repl_offset]
        wait_for_condition 100 100 {
            [get_info 3 master_repl_offset] >= $offset
        } else {
            fail "Node 3 did not catch up for post-sync writes"
        }

        # Verify all phases.
        R 3 readonly
        for {set i 0} {$i < 100} {incr i} {
            assert_equal "before:$i" [R 3 get "{tag0}cw:before:$i"]
        }
        for {set i 0} {$i < 200} {incr i} {
            assert_equal "during:$i" [R 3 get "{tag0}cw:during:$i"]
        }
        for {set i 0} {$i < 50} {incr i} {
            assert_equal "after:$i" [R 3 get "{tag0}cw:after:$i"]
        }

        # INCR must be exactly 50 (no double-apply from buffer drain).
        assert_equal 50 [R 3 get "{tag0}cw:counter"] \
            "Counter should be 50; duplicate commands would inflate this"

        # List must have 5 elements in LPUSH order (e d c b a).
        assert_equal 5 [R 3 llen "{tag0}cw:list"]
        assert_equal "e" [R 3 lindex "{tag0}cw:list" 0]
        assert_equal "a" [R 3 lindex "{tag0}cw:list" 4]

        assert_equal [R 0 dbsize] [R 3 dbsize]
    }

    # ===== RESIDUAL STATE - normal sync after sync-from-replica =====

    test "RESIDUAL STATE - normal sync works after sync-from-replica completed" {
        if {![is_sync_from_replica_implemented]} {
            skip "repl-prefer-sync-from-replica config is unavailable"
        }

        # Verify node 3 remains healthy after the previous sync-from-replica test.
        assert_equal "up" [get_info 3 master_link_status]

        # Disable the feature.
        R 3 config set repl-prefer-sync-from-replica no

        set p0_sync_full_before [get_info 0 sync_full]
        set s2_sync_full_before [get_info 2 sync_full]

        # Detach and re-attach WITHOUT the feature.
        detach_node3

        for {set i 0} {$i < 100} {incr i} {
            R 0 set "{tag0}rs:normal:$i" "normal:$i"
        }

        set primary0_id [R 0 cluster myid]
        R 3 cluster replicate $primary0_id
        wait_replica_of 3 $primary0_id
        wait_node_synced 3

        # Primary 0 should have done BGSAVE for normal sync.
        assert {[get_info 0 sync_full] > $p0_sync_full_before}

        # Sibling should not have done BGSAVE.
        assert_equal $s2_sync_full_before [get_info 2 sync_full]

        # Data correct.
        R 3 readonly
        for {set i 0} {$i < 100} {incr i} {
            assert_equal "normal:$i" [R 3 get "{tag0}rs:normal:$i"]
        }

        # Guard flag must be cleared (no residual state contamination).
        set in_progress [getInfoProperty [R 3 info replication] sync_from_replica_in_progress]
        assert {$in_progress eq "" || $in_progress eq "0"}

        assert_equal [R 0 dbsize] [R 3 dbsize]
    }

    # ===== REATTACH CYCLE (catches stale state after sync-from-replica) =====

    test "REATTACH CYCLE - detach and reattach to different primary after sync-from-replica" {
        if {![is_sync_from_replica_implemented]} {
            skip "repl-prefer-sync-from-replica config is unavailable"
        }

        for {set i 0} {$i < 200} {incr i} {
            R 0 set "{tag0}reattach:$i" "val:$i"
        }
        wait_node_synced 2

        # Sync from sibling to primary 0.
        detach_node3
        R 3 config set repl-prefer-sync-from-replica yes
        set primary0_id [R 0 cluster myid]
        R 3 cluster replicate $primary0_id
        wait_replica_of 3 $primary0_id
        wait_node_synced 3

        R 3 readonly
        assert_equal "val:199" [R 3 get "{tag0}reattach:199"]

        # Now detach node 3 and reattach to primary 1 (different shard).
        # Reattaching to a different primary verifies that transient state was cleared.
        detach_node3
        set primary1_id [R 1 cluster myid]
        R 3 cluster replicate $primary1_id
        wait_replica_of 3 $primary1_id
        wait_node_synced 3

        # Write to primary 1 and verify node 3 gets it.
        R 1 set "{aaa}reattach_p1" "from_p1"
        set offset [get_info 1 master_repl_offset]
        wait_for_condition 100 100 {
            [get_info 3 master_repl_offset] >= $offset
        } else {
            fail "Node 3 did not replicate from primary 1 after reattach"
        }

        R 3 readonly
        assert_equal "from_p1" [R 3 get "{aaa}reattach_p1"]
    }

    test "SYNC STATE - guard flag is visible during sibling sync and clears after switch" {
        if {![is_sync_from_replica_implemented]} {
            skip "sync-from-replica not implemented"
        }

        detach_node3
        R 3 config set repl-prefer-sync-from-replica yes
        R 2 config set rdb-key-save-delay 500

        set primary0_id [R 0 cluster myid]
        R 3 cluster replicate $primary0_id

        wait_sync_from_replica_started 3
        R 2 config set rdb-key-save-delay 0
        R 0 set "{tag0}sync-state-marker" "done"
        wait_replica_of 3 $primary0_id
        wait_node_synced 3
        wait_sync_from_replica_done 3

        assert_equal 0 [get_info 3 sync_from_replica_in_progress] \
            "Guard flag should be cleared after sync completes"
    }

    test "IO THREADS - sibling stream catch-up switches back to primary" {
        if {!$::io_threads} {
            skip "requires --io-threads"
        }
        if {![is_sync_from_replica_implemented]} {
            skip "sync-from-replica not implemented"
        }

        detach_node3
        R 3 config set repl-prefer-sync-from-replica yes
        R 2 config set rdb-key-save-delay 500

        set primary0_id [R 0 cluster myid]
        R 3 cluster replicate $primary0_id
        wait_sync_from_replica_started 3

        for {set i 0} {$i < 100} {incr i} {
            R 0 set "{tag0}io-thread-catchup:$i" "value:$i"
        }
        R 2 config set rdb-key-save-delay 0

        wait_replica_of 3 $primary0_id
        wait_node_synced 3
        wait_sync_from_replica_done 3

        assert_equal [srv 0 port] [get_info 3 master_port]
        assert_equal 0 [get_info 3 sync_from_replica_in_progress]
        R 3 readonly
        assert_equal "value:99" [R 3 get "{tag0}io-thread-catchup:99"]
    }

    # ------------------------------------------------------------------
    # CLUSTER FAILOVER BLOCKED - explicit failover rejected during sync
    # ------------------------------------------------------------------
    test "FAILOVER BLOCKED - CLUSTER FAILOVER rejected during sibling sync" {
        if {![is_sync_from_replica_implemented]} {
            skip "sync-from-replica not implemented"
        }

        detach_node3
        R 3 config set repl-prefer-sync-from-replica yes
        R 2 config set rdb-key-save-delay 500

        set primary0_id [R 0 cluster myid]
        R 3 cluster replicate $primary0_id
        wait_sync_from_replica_started 3

        set code [catch {R 3 cluster failover force} err]
        assert {$code == 1}
        assert_match {*syncing from sibling*} $err

        R 2 config set rdb-key-save-delay 0
        R 0 set "{tag0}failover-block-marker" "done"
        wait_replica_of 3 $primary0_id
        wait_node_synced 3
        wait_sync_from_replica_done 3
    }

} ;# start_cluster

# Exercise atomic migration while the source cannot learn committed ownership.
source tests/support/cluster_raft.tcl

start_cluster 3 0 {tags {external:skip cluster} overrides {cluster-protocol raft cluster-node-timeout 60000 cluster-manual-failover-timeout 500}} {
    raft_promote_start_cluster_voters 3

    test "Raft: acknowledged writes survive delayed slot ownership updates" {
        assert_equal leader [CI 0 cluster_raft_role]
        set source_id [R 2 CLUSTER MYID]
        set target_id [R 0 CLUSTER MYID]
        set key "{6ZJ}:handoff"
        assert_equal 16383 [R 2 CLUSTER KEYSLOT $key]
        assert_equal OK [R 2 SET $key before]

        # Hold the target before it proposes SLOT_CHANGE, after the source
        # has paused writes and granted the handoff.
        R 0 DEBUG SLOTMIGRATION PREVENT-FAILOVER 1
        test_defer {R 0 DEBUG SLOTMIGRATION PREVENT-FAILOVER 0}
        R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $target_id
        wait_for_condition 100 10 {
            [dict exists [set migration [lindex [R 0 CLUSTER GETSLOTMIGRATIONS] 0]] state] &&
            [dict get $migration state] eq "failover-granted"
        } else {
            fail "Target did not receive authorization for the handoff (migration: $migration)"
        }

        # Isolate only the source's cluster bus. The target and third voter
        # retain a quorum; the source's client and migration links remain live.
        foreach idx {0 1 2} {
            R $idx DEBUG DISABLE-CLUSTER-RECONNECTION 1
            test_defer [list R $idx DEBUG DISABLE-CLUSTER-RECONNECTION 0]
        }
        foreach idx {0 1} {
            R 2 DEBUG CLUSTERLINK KILL all [R $idx CLUSTER MYID]
            R $idx DEBUG CLUSTERLINK KILL all $source_id
        }
        R 0 DEBUG SLOTMIGRATION PREVENT-FAILOVER 0
        wait_for_condition 100 10 {
            [dict exists [set migration [lindex [R 0 CLUSTER GETSLOTMIGRATIONS] 0]] state] &&
            [dict get $migration state] eq "success"
        } else {
            fail "The quorum did not commit the slot handoff (migration: $migration)"
        }
        set committed [CI 0 cluster_raft_commit_index]
        assert {$committed > [CI 2 cluster_raft_last_applied]}
        assert_equal before [R 0 GET $key]

        # Exceed the 2000 ms pause extension granted by REQUEST-FAILOVER,
        # without stopping the source process or its client connections.
        after 2500
        set writer [valkey_deferring_client -2]
        test_defer [list $writer close]
        $writer CLIENT ID
        set writer_id [$writer read]
        $writer SET $key after
        wait_for_condition 100 10 {
            [string match "*cmd=set*" [R 2 CLIENT LIST ID $writer_id]]
        } else {
            fail "Source did not process the handoff-window write"
        }

        # Let the source apply the committed handoff before checking the
        # write's final outcome. A blocked write may now return MOVED.
        foreach idx {0 1 2} {R $idx DEBUG DISABLE-CLUSTER-RECONNECTION 0}
        wait_for_condition 100 50 {
            [CI 2 cluster_raft_last_applied] >= $committed
        } else {
            fail "Source did not apply the committed slot handoff"
        }
        if {![catch {$writer read} reply]} {
            assert_equal OK $reply
            assert_equal after [R 0 GET $key] "Source acknowledged a write lost during slot handoff"
        }
    }
}

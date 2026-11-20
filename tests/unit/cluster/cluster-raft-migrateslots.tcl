# Source must not ACK a write that applying SLOT_CHANGE will delete.
# The grant-window tests pause at FAILOVER-GRANTED, wait past the 2s export
# timeout, SET, then resume Raft. They only differ in how SLOT_CHANGE is stalled.
# A third test checks that failing before grant does not leave Raft paused.
source tests/support/cluster_raft.tcl

proc raft_migration_state {idx} {
    set m [lindex [R $idx CLUSTER GETSLOTMIGRATIONS] 0]
    if {[dict exists $m state]} { return [dict get $m state] }
    return ""
}

proc raft_begin_handoff {src dst slot key} {
    assert_equal OK [R $src SET $key before]
    R $dst DEBUG SLOTMIGRATION PREVENT-FAILOVER 1
    test_defer [list R $dst DEBUG SLOTMIGRATION PREVENT-FAILOVER 0]
    R $src CLUSTER MIGRATESLOTS SLOTSRANGE $slot $slot NODE [R $dst CLUSTER MYID]
    wait_for_condition 100 10 {
        [raft_migration_state $dst] eq "failover-granted"
    } else {
        fail "Target did not get failover-granted ([raft_migration_state $dst])"
    }
}

proc raft_handoff_write {src key} {
    after 2500
    set writer [valkey_deferring_client [expr {-$src}]]
    test_defer [list $writer close]
    $writer CLIENT ID
    set writer_id [$writer read]
    $writer SET $key after
    wait_for_condition 100 10 {
        [string match "*cmd=set*" [R $src CLIENT LIST ID $writer_id]]
    } else {
        fail "Source did not process the handoff-window write"
    }
    return $writer
}

proc raft_assert_write_not_lost {writer dst key} {
    if {![catch {$writer read} reply]} {
        assert_equal OK $reply
        assert_equal after [R $dst GET $key] "Source acknowledged a write lost during slot handoff"
    }
}

start_cluster 3 0 {tags {external:skip cluster} overrides {cluster-protocol raft cluster-node-timeout 60000 cluster-manual-failover-timeout 500}} {
    raft_promote_start_cluster_voters 3
    assert_equal leader [CI 0 cluster_raft_role]

    test "Raft: pre-grant export failure does not stick a write pause" {
        # Hold the source at REQUEST-PAUSE until the target is frozen, then
        # let it pause. The target cannot REQUEST-FAILOVER while asleep, so
        # cancel happens in FAILOVER_PAUSED, where SLOT_CHANGE cannot follow.
        set src 2
        set dst 0
        set slot 16383
        set key "{6ZJ}:handoff"
        assert_equal $slot [R $src CLUSTER KEYSLOT $key]
        assert_equal OK [R $src SET $key before]
        R $src DEBUG SLOTMIGRATION PREVENT-PAUSE 1
        test_defer {R $src DEBUG SLOTMIGRATION PREVENT-PAUSE 0}
        set sleeper [valkey_deferring_client [expr {-$dst}]]
        test_defer [list $sleeper close]
        R $src CLUSTER MIGRATESLOTS SLOTSRANGE $slot $slot NODE [R $dst CLUSTER MYID]
        wait_for_condition 100 10 {
            [raft_migration_state $dst] eq "waiting-for-paused"
        } else {
            fail "Target did not request pause ([raft_migration_state $dst])"
        }
        $sleeper DEBUG SLEEP 2
        R $src DEBUG SLOTMIGRATION PREVENT-PAUSE 0
        wait_for_condition 100 10 {
            [raft_migration_state $src] eq "failover-paused"
        } else {
            fail "Source did not pause ([raft_migration_state $src])"
        }
        assert_equal failover-paused [raft_migration_state $src]
        R $src CLUSTER CANCELSLOTMIGRATIONS
        wait_for_condition 100 10 {
            [raft_migration_state $src] in {failed cancelled}
        } else {
            fail "Source did not cancel ([raft_migration_state $src])"
        }
        $sleeper read

        set src_id [R $src CLUSTER MYID]
        for {set i 0} {$i < 3} {incr i} {
            R $i DEBUG DISABLE-CLUSTER-RECONNECTION 1
            test_defer [list R $i DEBUG DISABLE-CLUSTER-RECONNECTION 0]
        }
        for {set i 0} {$i < 3} {incr i} {
            if {$i == $src} continue
            R $src DEBUG CLUSTERLINK KILL all [R $i CLUSTER MYID]
            R $i DEBUG CLUSTERLINK KILL all $src_id
        }
        wait_for_condition 50 20 {
            [s -$src paused_reason] eq "none"
        } else {
            fail "Raft re-paused writes after a pre-grant failure ([s -$src paused_reason])"
        }
        assert_equal OK [R $src SET $key after]
    }
}

start_cluster 4 0 {tags {external:skip cluster} overrides {cluster-protocol raft cluster-node-timeout 60000 cluster-manual-failover-timeout 500}} {
    raft_promote_start_cluster_voters 4
    assert_equal leader [CI 0 cluster_raft_role]

    test "Raft: partitioned source does not ACK writes lost to SLOT_CHANGE" {
        # Follower source: quorum commits without this node applying.
        set src 3
        set dst 0
        set slot 16383
        set key "{6ZJ}:handoff"
        assert_equal $slot [R $src CLUSTER KEYSLOT $key]
        raft_begin_handoff $src $dst $slot $key

        set src_id [R $src CLUSTER MYID]
        for {set i 0} {$i < 4} {incr i} {
            R $i DEBUG DISABLE-CLUSTER-RECONNECTION 1
            test_defer [list R $i DEBUG DISABLE-CLUSTER-RECONNECTION 0]
        }
        for {set i 0} {$i < 4} {incr i} {
            if {$i == $src} continue
            R $src DEBUG CLUSTERLINK KILL all [R $i CLUSTER MYID]
            R $i DEBUG CLUSTERLINK KILL all $src_id
        }
        R $dst DEBUG SLOTMIGRATION PREVENT-FAILOVER 0
        wait_for_condition 100 10 {
            [raft_migration_state $dst] eq "success"
        } else {
            fail "Quorum did not commit the handoff ([raft_migration_state $dst])"
        }
        set committed [CI $dst cluster_raft_commit_index]
        assert {$committed > [CI $src cluster_raft_last_applied]}
        assert_equal before [R $dst GET $key]

        set writer [raft_handoff_write $src $key]
        for {set i 0} {$i < 4} {incr i} {R $i DEBUG DISABLE-CLUSTER-RECONNECTION 0}
        wait_for_condition 100 50 {
            [CI $src cluster_raft_last_applied] >= $committed
        } else {
            fail "Source did not apply the committed slot handoff"
        }
        raft_assert_write_not_lost $writer $dst $key
    }

    test "Raft: leader source does not ACK writes while SLOT_CHANGE is uncommitted" {
        # Leader source: entry is in the local log, but sleeping voters withhold ACK.
        set src 0
        set dst 2
        set slot 0
        set key "06S"
        assert_equal $slot [R $src CLUSTER KEYSLOT $key]
        raft_begin_handoff $src $dst $slot $key

        set sleepers {}
        foreach i {1 3} {
            set c [valkey_deferring_client [expr {-$i}]]
            test_defer [list $c close]
            $c DEBUG SLEEP 6
            lappend sleepers $c
        }
        after 200
        set log_before [CI $src cluster_raft_log_entries]
        set commit_before [CI $src cluster_raft_commit_index]
        R $dst DEBUG SLOTMIGRATION PREVENT-FAILOVER 0
        wait_for_condition 100 10 {
            [CI $src cluster_raft_log_entries] > $log_before &&
            [CI $src cluster_raft_commit_index] == $commit_before
        } else {
            fail "Leader did not keep SLOT_CHANGE uncommitted"
        }

        set writer [raft_handoff_write $src $key]
        assert_equal $commit_before [CI $src cluster_raft_commit_index]
        foreach c $sleepers {$c read}
        wait_for_condition 100 50 {
            [CI $src cluster_raft_last_applied] > $commit_before
        } else {
            fail "Leader did not apply the slot handoff after followers woke"
        }
        raft_assert_write_not_lost $writer $dst $key
    }
}

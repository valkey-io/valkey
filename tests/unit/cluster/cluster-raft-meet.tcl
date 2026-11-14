# Test CLUSTER MEET scenarios for the Raft cluster protocol.

tags {external:skip cluster singledb} {

start_multiple_servers 2 {overrides {cluster-enabled yes cluster-protocol raft}} {
    test "Raft MEET: two singletons" {
        set r0 [srv 0 client]

        $r0 CLUSTER MEET [srv -1 host] [srv -1 port]
        wait_for_raft_cluster_size 2
    }
}

start_multiple_servers 5 {overrides {cluster-enabled yes cluster-protocol raft}} {
    test "Raft MEET: star formation - first node meets each node" {
        set r0 [srv 0 client]

        for {set i 1} {$i < 5} {incr i} {
            $r0 CLUSTER MEET [srv -$i host] [srv -$i port]
        }
        wait_for_raft_cluster_size 5
    }
}

start_multiple_servers 5 {overrides {cluster-enabled yes cluster-protocol raft}} {
    test "Raft MEET: reverse star formation - each node meets the first node" {
        for {set i 1} {$i < 5} {incr i} {
            [srv -$i client] CLUSTER MEET [srv 0 host] [srv 0 port]
        }
        wait_for_raft_cluster_size 5
    }
}

start_multiple_servers 5 {overrides {cluster-enabled yes cluster-protocol raft}} {
    test "Raft MEET: chain formation - each node meets the next" {
        set r0 [srv 0 client]

        for {set i 0} {$i < 4} {incr i} {
            [srv -$i client] CLUSTER MEET [srv -[expr {$i+1}] host] [srv -[expr {$i+1}] port]
        }
        wait_for_raft_cluster_size 5
    }
}

start_multiple_servers 2 {overrides {cluster-enabled yes cluster-protocol raft}} {
    test "Raft MEET: addslots after meet" {
        set r0 [srv 0 client]

        $r0 CLUSTER MEET [srv -1 host] [srv -1 port]
        wait_for_raft_cluster_size 2

        $r0 CLUSTER ADDSLOTSRANGE 0 16383

        wait_for_condition 50 100 {
            [CI 0 cluster_slots_assigned] == 16384 &&
            [CI 1 cluster_slots_assigned] == 16384
        } else {
            fail "Slots: [CI 0 cluster_slots_assigned] [CI 1 cluster_slots_assigned]"
        }

        assert_equal ok [CI 0 cluster_state]
        assert_equal ok [CI 1 cluster_state]
    }
}

start_multiple_servers 4 {overrides {cluster-enabled yes cluster-protocol raft}} {
    test "Raft MEET: merging two clusters is rejected" {
        # Form two separate 2-node clusters.
        [srv 0 client] CLUSTER MEET [srv -1 host] [srv -1 port]
        [srv -2 client] CLUSTER MEET [srv -3 host] [srv -3 port]

        wait_for_raft_cluster_size 2

        # Attempting to merge them should fail.
        catch {[srv 0 client] CLUSTER MEET [srv -2 host] [srv -2 port]} err
        assert_match "ERR Cannot merge*" $err
    }
}

} ;# tags

# Test CLUSTER MEET scenarios for the Raft cluster protocol.

source tests/support/cluster_raft.tcl

tags {external:skip cluster singledb} {

start_multiple_servers 2 {overrides {cluster-enabled yes cluster-protocol raft}} {
    test "Raft MEET: two singletons" {
        set r0 [srv 0 client]
        set r1 [srv -1 client]

        $r0 CLUSTER MEET [srv -1 host] [srv -1 port]
        raft_add_voter $r0 [$r1 CLUSTER MYID]

        wait_for_condition 50 100 {
            [get_cluster_info_field $r0 cluster_size] == 2 &&
            [get_cluster_info_field $r1 cluster_size] == 2
        } else {
            fail "Cluster size: [get_cluster_info_field $r0 cluster_size] [get_cluster_info_field $r1 cluster_size]"
        }
    }
}

start_multiple_servers 5 {overrides {cluster-enabled yes cluster-protocol raft}} {
    test "Raft MEET: star formation - first node meets each node" {
        set r0 [srv 0 client]

        for {set i 1} {$i < 5} {incr i} {
            $r0 CLUSTER MEET [srv -$i host] [srv -$i port]
        }
        for {set i 1} {$i < 5} {incr i} {
            raft_add_voter $r0 [[srv -$i client] CLUSTER MYID]
        }

        wait_for_condition 100 200 {
            [get_cluster_info_field $r0 cluster_size] == 5 &&
            [get_cluster_info_field [srv -1 client] cluster_size] == 5 &&
            [get_cluster_info_field [srv -4 client] cluster_size] == 5
        } else {
            fail "Sizes: [get_cluster_info_field $r0 cluster_size] [get_cluster_info_field [srv -1 client] cluster_size] [get_cluster_info_field [srv -4 client] cluster_size]"
        }
    }
}

start_multiple_servers 5 {overrides {cluster-enabled yes cluster-protocol raft}} {
    test "Raft MEET: reverse star formation - each node meets the first node" {
        for {set i 1} {$i < 5} {incr i} {
            [srv -$i client] CLUSTER MEET [srv 0 host] [srv 0 port]
        }
        # The first initiator becomes leader; promote all joined learners there.
        set leader [srv -1 client]
        foreach idx {0 -1 -2 -3 -4} {
            raft_add_voter $leader [[srv $idx client] CLUSTER MYID]
        }

        wait_for_condition 100 200 {
            [get_cluster_info_field [srv 0 client] cluster_size] == 5 &&
            [get_cluster_info_field [srv -1 client] cluster_size] == 5 &&
            [get_cluster_info_field [srv -4 client] cluster_size] == 5
        } else {
            fail "Sizes: [get_cluster_info_field [srv 0 client] cluster_size] [get_cluster_info_field [srv -1 client] cluster_size] [get_cluster_info_field [srv -4 client] cluster_size]"
        }
    }
}

start_multiple_servers 5 {overrides {cluster-enabled yes cluster-protocol raft}} {
    test "Raft MEET: chain formation - each node meets the next" {
        set r0 [srv 0 client]

        for {set i 0} {$i < 4} {incr i} {
            [srv -$i client] CLUSTER MEET [srv -[expr {$i+1}] host] [srv -[expr {$i+1}] port]
        }
        for {set i 1} {$i < 5} {incr i} {
            raft_add_voter $r0 [[srv -$i client] CLUSTER MYID]
        }

        wait_for_condition 100 200 {
            [get_cluster_info_field $r0 cluster_size] == 5 &&
            [get_cluster_info_field [srv -1 client] cluster_size] == 5 &&
            [get_cluster_info_field [srv -4 client] cluster_size] == 5
        } else {
            fail "Sizes: [get_cluster_info_field $r0 cluster_size] [get_cluster_info_field [srv -1 client] cluster_size] [get_cluster_info_field [srv -4 client] cluster_size]"
        }
    }
}

start_multiple_servers 2 {overrides {cluster-enabled yes cluster-protocol raft}} {
    test "Raft MEET: addslots after meet" {
        set r0 [srv 0 client]
        set r1 [srv -1 client]

        $r0 CLUSTER MEET [srv -1 host] [srv -1 port]
        raft_add_voter $r0 [$r1 CLUSTER MYID]
        wait_for_condition 50 100 {
            [get_cluster_info_field $r0 cluster_size] == 2
        } else {
            fail "Cluster did not form"
        }

        $r0 CLUSTER ADDSLOTSRANGE 0 16383

        wait_for_condition 50 100 {
            [get_cluster_info_field $r0 cluster_slots_assigned] == 16384 &&
            [get_cluster_info_field $r1 cluster_slots_assigned] == 16384
        } else {
            fail "Slots: [get_cluster_info_field $r0 cluster_slots_assigned] [get_cluster_info_field $r1 cluster_slots_assigned]"
        }

        assert_equal ok [get_cluster_info_field $r0 cluster_state]
        assert_equal ok [get_cluster_info_field $r1 cluster_state]
    }
}

start_multiple_servers 4 {overrides {cluster-enabled yes cluster-protocol raft}} {
    test "Raft MEET: merging two clusters is rejected" {
        # Form two separate 2-node clusters.
        [srv 0 client] CLUSTER MEET [srv -1 host] [srv -1 port]
        raft_add_voter [srv 0 client] [[srv -1 client] CLUSTER MYID]
        [srv -2 client] CLUSTER MEET [srv -3 host] [srv -3 port]
        raft_add_voter [srv -2 client] [[srv -3 client] CLUSTER MYID]

        wait_for_condition 50 100 {
            [get_cluster_info_field [srv 0 client] cluster_size] == 2 &&
            [get_cluster_info_field [srv -2 client] cluster_size] == 2
        } else {
            fail "Two clusters did not form"
        }

        # Attempting to merge them should fail.
        catch {[srv 0 client] CLUSTER MEET [srv -2 host] [srv -2 port]} err
        assert_match "ERR Cannot merge*" $err
    }
}

} ;# tags

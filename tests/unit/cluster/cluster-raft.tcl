# Test higher-level Raft cluster behavior that does not require direct
# wire-protocol interaction from Tcl.

tags {external:skip cluster singledb} {

test "Raft: leader steps down after losing quorum freshness" {
    start_multiple_servers 3 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
        [srv 0 client] CLUSTER MEET [srv -1 host] [srv -1 port]
        [srv 0 client] CLUSTER MEET [srv -2 host] [srv -2 port]

        wait_for_condition 50 100 {
            [CI 0 cluster_size] == 3 &&
            [CI 1 cluster_size] == 3 &&
            [CI 2 cluster_size] == 3
        } else {
            fail "Cluster did not form: sizes=[CI 0 cluster_size],[CI 1 cluster_size],[CI 2 cluster_size]"
        }

        assert_equal [CI 0 cluster_raft_role] "leader"
        set leader_idx 0

        set paused [list]
        foreach idx {0 1 2} {
            if {$idx == $leader_idx} continue
            pause_process [srv [expr {-$idx}] pid]
            lappend paused $idx
        }

        wait_for_condition 100 50 {
            [CI $leader_idx cluster_raft_role] eq "follower"
        } else {
            foreach idx $paused {
                resume_process [srv [expr {-$idx}] pid]
            }
            fail "Leader did not step down after losing quorum freshness: role=[CI $leader_idx cluster_raft_role] leader=[CI $leader_idx cluster_raft_leader]"
        }

        foreach idx $paused {
            resume_process [srv [expr {-$idx}] pid]
        }
    }
}

test "Raft: CLUSTER FORGET transfers leadership when target is leader" {
    start_multiple_servers 3 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
        [srv 0 client] CLUSTER MEET [srv -1 host] [srv -1 port]
        [srv 0 client] CLUSTER MEET [srv -2 host] [srv -2 port]

        wait_for_condition 50 100 {
            [CI 0 cluster_size] == 3 &&
            [CI 1 cluster_size] == 3 &&
            [CI 2 cluster_size] == 3
        } else {
            fail "Cluster did not form: sizes=[CI 0 cluster_size],[CI 1 cluster_size],[CI 2 cluster_size]"
        }

        set leader_id [CI 0 cluster_raft_leader]
        set leader_idx -1
        set followers [list]
        foreach idx {0 1 2} {
            if {[R $idx CLUSTER MYID] eq $leader_id} {
                set leader_idx $idx
            } else {
                lappend followers $idx
            }
        }

        assert {$leader_idx >= 0}
        assert_equal 2 [llength $followers]

        set forgetter_idx [lindex $followers 0]
        set survivor_idx [lindex $followers 1]
        assert_equal "OK" [R $forgetter_idx CLUSTER FORGET $leader_id]

        wait_for_condition 100 50 {
            [CI $forgetter_idx cluster_raft_leader] ne "" &&
            [CI $forgetter_idx cluster_raft_leader] eq [CI $survivor_idx cluster_raft_leader] &&
            [CI $forgetter_idx cluster_raft_leader] ne $leader_id
        } else {
            fail "Leader was not transferred before forgetting: forgetter_leader=[CI $forgetter_idx cluster_raft_leader] survivor_leader=[CI $survivor_idx cluster_raft_leader]"
        }
    }
}

} ;# tags

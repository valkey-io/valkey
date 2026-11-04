# Test higher-level Raft cluster behavior that does not require direct
# wire-protocol interaction from Tcl.

source tests/support/cluster_raft.tcl

tags {external:skip cluster singledb} {

test "Raft: leader steps down after losing quorum freshness" {
    start_multiple_servers 3 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
        set r0 [srv 0 client]
        $r0 CLUSTER MEET [srv -1 host] [srv -1 port]
        $r0 CLUSTER MEET [srv -2 host] [srv -2 port]
        raft_add_voter $r0 [[srv -1 client] CLUSTER MYID]
        raft_add_voter $r0 [[srv -2 client] CLUSTER MYID]

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

} ;# tags

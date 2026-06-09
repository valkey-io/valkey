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

        set leader_idx -999
        foreach idx {0 1 2} {
            if {[CI $idx cluster_raft_role] eq "leader"} {
                set leader_idx $idx
                break
            }
        }
        assert {$leader_idx != -999}

        set paused [list]
        foreach idx {0 1 2} {
            if {$idx == $leader_idx} continue
            pause_process [srv [expr {-$idx}] pid]
            lappend paused $idx
        }

        set stepped_down 0
        set leader_role ""
        for {set i 0} {$i < 150} {incr i} {
            set leader_role [CI $leader_idx cluster_raft_role]
            if {[string equal $leader_role "follower"]} {
                set stepped_down 1
                break
            }
            after 50
        }
        if {!$stepped_down} {
            foreach idx $paused {
                resume_process [srv [expr {-$idx}] pid]
            }
            fail "Leader did not step down after losing quorum freshness: role=$leader_role leader=[CI $leader_idx cluster_raft_leader]"
        }

        foreach idx $paused {
            resume_process [srv [expr {-$idx}] pid]
        }
    }
}

} ;# tags

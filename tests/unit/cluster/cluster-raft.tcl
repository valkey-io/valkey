# Test higher-level Raft cluster behavior that does not require direct
# wire-protocol interaction from Tcl.

source tests/support/cluster_raft.tcl

# Form a cluster led by node 0, with nodes 1..num_voters-1 as voters and the
# rest as learners.
proc raft_form_cluster {num_voters num_nodes} {
    for {set i 1} {$i < $num_nodes} {incr i} {
        R 0 CLUSTER MEET [srv -$i host] [srv -$i port]
    }
    for {set i 1} {$i < $num_voters} {incr i} {
        raft_add_voter [srv 0 client] [R $i CLUSTER MYID]
    }
    wait_for_condition 100 100 {
        [raft_cluster_formed $num_voters $num_nodes]
    } else {
        fail "Cluster did not form with $num_voters voters and [expr {$num_nodes - $num_voters}] learners"
    }
}

proc raft_cluster_formed {num_voters num_nodes} {
    if {[CI 0 cluster_raft_role] ne "leader"} { return 0 }
    for {set i 0} {$i < $num_nodes} {incr i} {
        if {[CI $i cluster_size] != $num_voters} { return 0 }
        if {$i >= $num_voters && [CI $i cluster_raft_role] ne "learner"} { return 0 }
    }
    return 1
}

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

test "Raft: CLUSTER FORGET transfers leadership when target is leader" {
    start_multiple_servers 3 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
        raft_form_cluster 2 3
        set leader_id [R 0 CLUSTER MYID]
        set target_id [R 1 CLUSTER MYID]

        assert_equal "OK" [R 2 CLUSTER FORGET $leader_id]

        wait_for_condition 100 50 {
            [CI 1 cluster_raft_leader] eq $target_id &&
            [CI 2 cluster_raft_leader] eq $target_id &&
            [CI 1 cluster_size] == 1 &&
            [CI 2 cluster_size] == 1
        } else {
            fail "Leadership was not transferred before forgetting the leader"
        }
    }
}

test "Raft: leader transfer waits for the target to catch up" {
    # Long timeout so the leader keeps its link to the paused target
    start_multiple_servers 3 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 5000}} {
        raft_form_cluster 2 3
        set leader_id [R 0 CLUSTER MYID]
        set target_id [R 1 CLUSTER MYID]
        set loglines [count_log_lines 0]

        # The leader appends an entry the paused target doesn't get
        pause_process [srv -1 pid]
        R 0 CONFIG SET cluster-announce-hostname lagging-target.example.com
        set rd [valkey_deferring_client -2]
        $rd CLUSTER FORGET $leader_id

        wait_for_log_messages 0 [list "*waiting for $target_id to catch up*"] $loglines 50 100
        assert_equal [CI 0 cluster_raft_role] "leader"

        resume_process [srv -1 pid]
        assert_equal "OK" [$rd read]
        $rd close

        wait_for_condition 100 50 {
            [CI 1 cluster_raft_leader] eq $target_id &&
            [CI 2 cluster_raft_leader] eq $target_id &&
            [CI 1 cluster_size] == 1 &&
            [CI 2 cluster_size] == 1
        } else {
            fail "Leadership was not transferred to the caught-up voter"
        }
    }
}

test "Raft: CLUSTER FORGET rejects leader transfer without another voter" {
    start_multiple_servers 2 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
        raft_form_cluster 1 2
        assert_error "*no eligible voting member available for leadership transfer*" {
            R 1 CLUSTER FORGET [R 0 CLUSTER MYID]
        }
    }
}

test "Raft: CLUSTER FORGET of a leader with slots fails without a leader transfer" {
    start_multiple_servers 3 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
        raft_form_cluster 2 3
        set leader_id [R 0 CLUSTER MYID]
        R 0 CLUSTER ADDSLOTS 0
        wait_for_condition 50 100 {
            [llength [R 2 CLUSTER SLOTS]] == 1
        } else {
            fail "Slot assignment was not propagated"
        }
        set term [CI 0 cluster_raft_current_term]

        assert_error "*proposal rejected by raft leader*" {R 2 CLUSTER FORGET $leader_id}
        assert_equal $leader_id [CI 2 cluster_raft_leader]
        assert_equal $term [CI 0 cluster_raft_current_term]
    }
}

} ;# tags

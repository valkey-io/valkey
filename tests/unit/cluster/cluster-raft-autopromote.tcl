# Automatic learner promotion up to RAFT_TARGET_VOTERS (5).

source tests/support/cluster_raft.tcl

# Server index in 1..n-1. Skip the bootstrap leader so demotion and FORGET do not pick it.
proc raft_find_learner {leader n} {
    for {set i 1} {$i < $n} {incr i} {
        set id [R $i CLUSTER MYID]
        if {[cluster_has_flag [cluster_get_node_by_id $leader $id] learner]} {
            return $i
        }
    }
    return -1
}

proc raft_find_voter {leader n {exclude -1}} {
    for {set i 1} {$i < $n} {incr i} {
        if {$i == $exclude} continue
        set id [R $i CLUSTER MYID]
        if {![cluster_has_flag [cluster_get_node_by_id $leader $id] learner]} {
            return $i
        }
    }
    return -1
}

tags {external:skip cluster singledb} {

start_cluster 2 0 {overrides {cluster-protocol raft cluster-node-timeout 10000}} {
    start_server {overrides {cluster-enabled yes cluster-protocol raft cluster-voter no}} {
        test "Raft autopromote: a voter policy change during admission takes effect" {
            # R 1 is the leader. Pause its only follower to keep R 0's NODE_JOIN
            # uncommitted while the joiner changes its policy.
            pause_process [srv -2 pid]
            set meet [valkey_deferring_client -1]
            $meet CLUSTER MEET [srv 0 host] [srv 0 port]
            wait_for_condition 100 50 {
                [raft_inflight_count [Rn 1] NODE_JOIN] == 1
            } else {fail "Leader did not append NODE_JOIN"}
            assert_equal OK [raft_set_voter 0 auto]
            resume_process [srv -2 pid]
            assert_equal OK [$meet read]
            $meet close

            wait_for_raft_cluster_size 3
            assert_equal follower [CI 0 cluster_raft_role]
        }
    }
}

start_cluster 5 0 {overrides {cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: 1 voter + 4 learners converge to 5 and do not overshoot" {
        after 2000
        assert_equal 5 [CI 0 cluster_size]
        assert_equal 0 [raft_inflight_count [srv 0 client] ADD_VOTER]
    }
}

start_cluster 6 0 {overrides {cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: yes above target stays; extra auto voters are demoted" {
        set remaining [raft_find_learner 0 6]
        assert {$remaining > 0}
        set remaining_id [R $remaining CLUSTER MYID]
        raft_add_voter 0 $remaining
        wait_for_condition 200 100 {
            [CI 0 cluster_size] == 5 &&
            ![cluster_has_flag [cluster_get_node_by_id 0 $remaining_id] learner]
        } else {
            fail "size=[CI 0 cluster_size] remaining learner=[cluster_has_flag [cluster_get_node_by_id 0 $remaining_id] learner]"
        }
    }
}

start_cluster 7 0 {overrides {cluster-protocol raft cluster-node-timeout 1200}} {
    foreach policy {auto no} {
        test "Raft autopromote: cluster-voter $policy waits for a quorum of remaining voters" {
            for {set i 0} {$i < 7} {incr i} {raft_set_voter $i yes}
            wait_for_raft_cluster_size 7
            set failed_ids {}
            foreach i {4 5 6} {
                lappend failed_ids [R $i CLUSTER MYID]
                pause_process [srv -$i pid]
            }
            wait_for_condition 100 50 {
                [cluster_all_see_flag {0} $failed_ids fail]
            } else {fail "Leader did not mark the three paused voters as failed"}

            set loglines [count_log_lines 0]
            raft_set_voter 1 $policy
            wait_for_log_messages 0 {"*Applied NODE_INFO*"} $loglines 100 50
            # Give the controller several cron ticks to act on the policy.
            after 300
            assert_equal OK [R 0 CLUSTER DELSLOTS 0]
            assert_equal 7 [CI 0 cluster_size]

            # One recovery leaves four responsive voters even after the demotion.
            resume_process [srv -4 pid]
            wait_for_raft_cluster_size 6
            assert_equal learner [CI 1 cluster_raft_role]
            assert_equal OK [R 0 CLUSTER ADDSLOTS 0]
        }
    }
}

start_cluster 5 0 {overrides {cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: DELVOTER below target is applied, then the learner is promoted" {
        set target [raft_find_voter 0 5]
        assert {$target > 0}
        set target_id [R $target CLUSTER MYID]
        raft_set_voter $target no
        wait_for_condition 100 50 {
            [cluster_has_flag [cluster_get_node_by_id 0 $target_id] learner]
        } else {
            fail "demotion was not applied: size=[CI 0 cluster_size]"
        }
        raft_set_voter $target auto
        wait_for_condition 200 100 {
            [CI 0 cluster_size] == 5 &&
            ![cluster_has_flag [cluster_get_node_by_id 0 $target_id] learner]
        } else {
            fail "size=[CI 0 cluster_size]"
        }
    }
}

start_cluster 6 0 {overrides {cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: DELVOTER at/above target is allowed" {
        set remaining [raft_find_learner 0 6]
        assert {$remaining > 0}
        raft_add_voter 0 $remaining
        set target [raft_find_voter 0 6 $remaining]
        assert {$target > 0}
        raft_set_voter $target no
        wait_for_raft_cluster_size 5
    }
}

start_cluster 6 0 {overrides {cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: paused voter does not cause promotion beyond target" {
        set remaining [raft_find_learner 0 6]
        set pause_i [raft_find_voter 0 6]
        assert {$remaining > 0 && $pause_i > 0}
        set remaining_id [R $remaining CLUSTER MYID]
        pause_process [srv -$pause_i pid]
        after 2000
        assert_equal 5 [CI 0 cluster_size]
        assert {[cluster_has_flag [cluster_get_node_by_id 0 $remaining_id] learner]}
        resume_process [srv -$pause_i pid]
    }
}

start_cluster 6 0 {overrides {cluster-protocol raft cluster-node-timeout 500}} {
    test "Raft autopromote: leader change: yes stays a voter after auto demotes back to 5" {
        set remaining [raft_find_learner 0 6]
        assert {$remaining > 0}

        set old_term [CI 0 cluster_raft_current_term]
        pause_process [srv 0 pid]
        wait_for_condition 200 100 {
            [CI 1 cluster_raft_current_term] > $old_term &&
            ([CI 1 cluster_raft_role] eq "leader" ||
             [CI 2 cluster_raft_role] eq "leader" ||
             [CI 3 cluster_raft_role] eq "leader" ||
             [CI 4 cluster_raft_role] eq "leader" ||
             [CI 5 cluster_raft_role] eq "leader")
        } else {
            resume_process [srv 0 pid]
            fail "no new leader elected after pause"
        }

        set new_leader_idx 0
        for {set i 1} {$i < 6} {incr i} {
            if {[CI $i cluster_raft_role] eq "leader"} {
                set new_leader_idx $i
                break
            }
        }
        assert {$new_leader_idx > 0}

        set remaining_id [R $remaining CLUSTER MYID]
        raft_add_voter $new_leader_idx $remaining
        resume_process [srv 0 pid]
        wait_for_raft_cluster_size 5
        assert {![cluster_has_flag [cluster_get_node_by_id $new_leader_idx $remaining_id] learner]}
    }
}

start_cluster 6 0 {overrides {cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: forgetting a voter promotes another learner" {
        set learner [raft_find_learner 0 6]
        set target [raft_find_voter 0 6]
        assert {$learner > 0 && $target > 0}
        set learner_id [R $learner CLUSTER MYID]
        set target_id [R $target CLUSTER MYID]
        # start_cluster assigns slots; NODE_FORGET rejects a primary that still
        # owns any. Flush before pause so the target can propose the DELSLOTS.
        assert_equal OK [R $target CLUSTER FLUSHSLOTS]
        pause_process [srv -$target pid]
        assert_equal OK [R 0 CLUSTER FORGET $target_id]
        wait_for_condition 200 100 {
            [CI 0 cluster_size] == 5 &&
            [cluster_get_node_by_id 0 $target_id] eq {} &&
            ![cluster_has_flag [cluster_get_node_by_id 0 $learner_id] learner]
        } else {
            resume_process [srv -$target pid]
            fail "size=[CI 0 cluster_size]"
        }
        resume_process [srv -$target pid]
    }
}

start_cluster 3 0 {overrides {cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: FORGET can shrink the voter set to one node" {
        assert_equal OK [R 2 CLUSTER FLUSHSLOTS]
        assert_equal OK [R 0 CLUSTER FORGET [R 2 CLUSTER MYID]]
        pause_process [srv -2 pid]
        wait_for_condition 100 100 {
            [CI 0 cluster_size] == 2
        } else {
            resume_process [srv -2 pid]
            fail "size=[CI 0 cluster_size]"
        }
        assert_equal OK [R 1 CLUSTER FLUSHSLOTS]
        assert_equal OK [R 0 CLUSTER FORGET [R 1 CLUSTER MYID]]
        pause_process [srv -1 pid]
        wait_for_condition 100 100 {
            [CI 0 cluster_size] == 1
        } else {
            resume_process [srv -1 pid]
            resume_process [srv -2 pid]
            fail "size=[CI 0 cluster_size]"
        }
        after 1000
        assert_equal 1 [CI 0 cluster_size]
        resume_process [srv -1 pid]
        resume_process [srv -2 pid]
    }
}

} ;# tags

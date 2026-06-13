# Test pre-vote behavior in real raft clusters.

proc raft_wait_for_any_prevote_log {srv_a from_a srv_b from_b maxtries delay} {
    for {set i 0} {$i < $maxtries} {incr i} {
        set logs_a [exec tail -n +[expr {$from_a + 1}] < [srv $srv_a stdout]]
        set logs_b [exec tail -n +[expr {$from_b + 1}] < [srv $srv_b stdout]]
        if {[string match "*Starting Raft pre-vote*" $logs_a]} {
            return
        }
        if {[string match "*Starting Raft pre-vote*" $logs_b]} {
            return
        }
        after $delay
    }
    fail "no survivor started Raft pre-vote"
}

tags {external:skip cluster singledb} {

start_cluster 3 0 {overrides {cluster-protocol raft cluster-node-timeout 500}} {
    test "Raft Cluster: leader failure, successful pre-vote and election" {
        assert_equal "leader" [CI 0 cluster_raft_role]

        set old_term [CI 0 cluster_raft_current_term]
        set old_leader_id [CI 0 cluster_raft_leader]
        set loglines1 [count_log_lines -1]
        set loglines2 [count_log_lines -2]

        pause_process [srv 0 pid]

        raft_wait_for_any_prevote_log -1 $loglines1 -2 $loglines2 200 100

        wait_for_condition 200 100 {
            [CI 1 cluster_raft_current_term] > $old_term &&
            [CI 2 cluster_raft_current_term] > $old_term &&
            ([CI 1 cluster_raft_role] eq "leader" ||
             [CI 2 cluster_raft_role] eq "leader")
        } else {
            fail "no new leader elected after leader shutdown (term1=[CI 1 cluster_raft_current_term] term2=[CI 2 cluster_raft_current_term] role1=[CI 1 cluster_raft_role] role2=[CI 2 cluster_raft_role])"
        }

        if {[CI 1 cluster_raft_role] eq "leader"} {
            set new_leader_idx 1
        } else {
            set new_leader_idx 2
        }
        set new_leader_id [CI $new_leader_idx cluster_raft_leader]
        assert {$new_leader_id ne $old_leader_id}
        assert {[CI $new_leader_idx cluster_raft_current_term] > $old_term}
    }
}

} ;# tags

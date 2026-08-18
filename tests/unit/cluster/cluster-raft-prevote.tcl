# Test pre-vote behavior in real raft clusters.

tags {external:skip cluster singledb} {

start_cluster 3 0 {overrides {cluster-protocol raft cluster-node-timeout 500}} {
    test "Raft Cluster: leader failure, successful pre-vote and election" {
        wait_for_condition 50 100 {
            [CI 0 cluster_size] == 3 &&
            [CI 1 cluster_size] == 3 &&
            [CI 2 cluster_size] == 3
        } else {
            fail "Cluster did not form: sizes=[CI 0 cluster_size],[CI 1 cluster_size],[CI 2 cluster_size]"
        }

        assert_equal "leader" [CI 0 cluster_raft_role]

        set old_term [CI 0 cluster_raft_current_term]
        set old_leader_id [CI 0 cluster_raft_leader]

        pause_process [srv 0 pid]

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
            set new_leader_log_idx -1
        } else {
            set new_leader_idx 2
            set new_leader_log_idx -2
        }
        set new_leader_id [CI $new_leader_idx cluster_raft_leader]
        assert {$new_leader_id ne $old_leader_id}
        assert {[CI $new_leader_idx cluster_raft_current_term] > $old_term}
        assert {[count_log_message $new_leader_log_idx "Starting Raft pre-vote"] > 0}
    }
}

} ;# tags

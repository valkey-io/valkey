# Shared helpers for Raft cluster integration tests.

proc raft_add_voter {leader node_id} {
    set leader_client [Rn $leader]
    # Wait until NODE_JOIN finished: learner, or already a voting member.
    wait_for_condition 100 100 {
        [expr {[llength [cluster_get_node_by_id $leader $node_id]] > 0 &&
               ([cluster_has_flag [cluster_get_node_by_id $leader $node_id] learner] ||
                ![cluster_has_flag [cluster_get_node_by_id $leader $node_id] handshake])}]
    } else {
        fail "Node $node_id did not finish joining on leader $leader"
    }
    set err ""
    # The auto-promoter may have already promoted this learner, making a
    # subsequent ADDVOTER fail with "already a voter". Treat either
    # "promoted" or "ADDVOTER succeeded" as success (avoids a TOCTOU race).
    wait_for_condition 200 100 {
        [expr {![cluster_has_flag [cluster_get_node_by_id $leader $node_id] learner] ||
               ![catch {$leader_client CLUSTER ADDVOTER $node_id} err]}]
    } else {
        fail "Could not promote $node_id to voter: $err"
    }
}

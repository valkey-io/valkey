# Test higher-level Raft cluster behavior that does not require direct
# wire-protocol interaction from Tcl.

tags {external:skip cluster singledb} {

start_cluster 3 0 {overrides {cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft: leader steps down after losing quorum freshness" {
        assert_equal [CI 0 cluster_raft_role] "leader"

        pause_process [srv -1 pid]
        pause_process [srv -2 pid]

        wait_for_condition 100 50 {
            [CI 0 cluster_raft_role] eq "follower"
        } else {
            resume_process [srv -1 pid]
            resume_process [srv -2 pid]
            fail "Leader did not step down after losing quorum freshness: role=[CI 0 cluster_raft_role] leader=[CI 0 cluster_raft_leader]"
        }

        resume_process [srv -1 pid]
        resume_process [srv -2 pid]
    }
}

} ;# tags

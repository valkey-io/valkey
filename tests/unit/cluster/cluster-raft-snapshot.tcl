# Test Raft log snapshot (InstallSnapshot) behavior.

tags {external:skip cluster singledb} {

start_cluster 3 0 {overrides {cluster-protocol raft cluster-node-timeout 3000}} {

    test "Raft snapshot: follower catches up via snapshot after leader trims log" {
        assert_equal "leader" [CI 0 cluster_raft_role]

        # Pause a follower before generating entries so its next_index
        # falls behind the leader's trim point.
        pause_process [srv -1 pid]

        # Generate enough committed entries to trigger log trimming (>128).
        for {set j 0} {$j < 140} {incr j} {
            R 0 config set cluster-announce-hostname "host-$j.com"
        }

        # Wait for leader to trim its log.
        wait_for_condition 50 100 {
            [CI 0 cluster_raft_log_entries] < 128
        } else {
            fail "leader did not trim log (entries=[CI 0 cluster_raft_log_entries])"
        }
        set leader_applied [CI 0 cluster_raft_last_applied]

        resume_process [srv -1 pid]

        # Follower should catch up via snapshot.
        wait_for_condition 50 200 {
            [CI 1 cluster_raft_last_applied] == [CI 0 cluster_raft_last_applied]
        } else {
            fail "follower did not catch up (follower=[CI 1 cluster_raft_last_applied] leader=[CI 0 cluster_raft_last_applied])"
        }

        # Verify the follower caught up to the leader's applied index.
        assert {[CI 1 cluster_raft_last_applied] >= $leader_applied}
    }
}

start_cluster 3 0 {overrides {cluster-protocol raft cluster-node-timeout 3000}} {

    test "Raft snapshot: new node joins leader with compacted log (via restart)" {
        assert_equal "leader" [CI 0 cluster_raft_role]
        set leader_port [srv 0 port]

        # Wait for cluster to converge.
        wait_for_condition 50 100 {
            [CI 2 cluster_raft_last_applied] == [CI 0 cluster_raft_last_applied]
        } else {
            fail "cluster not converged"
        }

        # Restart all nodes one at a time. This preserves quorum and avoids
        # a slow full election. After restart, each node's in-memory log is
        # empty (all committed entries are in the snapshot/nodes.conf).
        for {set i 0} {$i < 3} {incr i} {
            restart_server [expr {-1*$i}] true false
            # Wait for the restarted node to rejoin.
            wait_for_condition 50 100 {
                [CI $i cluster_raft_last_applied] > 0
            } else {
                fail "node $i did not recover after restart"
            }
        }

        # Add a new node. The leader has no log entries, so it must
        # send InstallSnapshot to bring the new node up to speed.
        start_server {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 3000}} {
            R 0 CLUSTER MEET 127.0.0.1 $leader_port

            # Wait for the new node to join (receives snapshot with all 4 nodes).
            wait_for_condition 200 200 {
                [CI 0 cluster_known_nodes] == 4
            } else {
                puts "New node CLUSTER NODES: [R 0 CLUSTER NODES]"
                puts "New node CLUSTER INFO: [R 0 CLUSTER INFO]"
                fail "new node did not join (known_nodes=[CI 0 cluster_known_nodes])"
            }

            # Verify snapshot was used and the new node's index is correct.
            assert {[count_log_message 0 "Installed snapshot"] > 0}
            assert {[CI 0 cluster_raft_last_applied] > 0}
        }
    }
}

start_cluster 3 0 {overrides {cluster-protocol raft cluster-node-timeout 3000}} {

    test "Raft snapshot: new node joins when leader has trimmed log" {
        assert_equal "leader" [CI 0 cluster_raft_role]
        set leader_port [srv 0 port]

        # Generate enough entries to trigger trimming.
        for {set j 0} {$j < 140} {incr j} {
            R 0 config set cluster-announce-hostname "host-$j.com"
        }

        wait_for_condition 50 100 {
            [CI 0 cluster_raft_log_entries] < 128
        } else {
            fail "leader did not trim log"
        }

        # Start a new server and have it MEET the leader. The new node
        # steps down to joiner on WELCOME and receives InstallSnapshot.
        assert_equal "leader" [CI 0 cluster_raft_role]
        set leader_applied [CI 0 cluster_raft_last_applied]
        start_server {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 3000}} {
            # Here, R 0 is the new server and the others have shifted.
            R 0 CLUSTER MEET 127.0.0.1 $leader_port

            wait_for_condition 100 200 {
                [CI 0 cluster_known_nodes] == 4
            } else {
                fail "new node did not join (known_nodes=[CI 0 cluster_known_nodes])"
            }

            # Verify snapshot was used and the new node caught up.
            assert {[count_log_message 0 "Installed snapshot"] > 0}
            assert {[count_log_message 0 "Promoted from joiner to follower"] > 0}
            assert {[CI 0 cluster_raft_last_applied] >= $leader_applied}
        }
    }
}

} ;# tags

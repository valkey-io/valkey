# Test raft failure detection paths.
# Validates that the correct detector is used based on shard type:
# - Single-node shard: AE_ACK-based detector (centralized, on the raft leader)
# - Multi-node shard: replication-stream-based detector (decentralized)
#
# Uses 3 masters + 2 replicas: masters 0,1 have replicas (multi-node shards),
# master 2 has no replica (single-node shard).

start_cluster 3 2 {tags {external:skip cluster cluster-raft:only}} {

    test "Cluster should start ok" {
        wait_for_cluster_state ok
    }

    test "Identify topology: single-node shard and multi-node shard" {
        # Find raft leader
        set ::leader_idx -1
        for {set j 0} {$j < 5} {incr j} {
            if {[CI $j cluster_raft_role] eq "leader"} {
                set ::leader_idx $j
                break
            }
        }
        assert {$::leader_idx != -1}

        # Find a single-node primary (no replica) and a multi-node primary
        # (has a replica). Prefer a non-leader for the multi-node primary,
        # but accept the leader if it's the only option — the replica-side
        # detector works regardless.
        set ::single_node_primary -1
        set ::multi_node_primary -1
        set ::multi_node_replica -1

        for {set j 0} {$j < 3} {incr j} {
            set myid [dict get [cluster_get_myself $j] id]
            set has_replica 0
            for {set k 3} {$k < 5} {incr k} {
                if {[dict get [cluster_get_myself $k] slaveof] eq $myid} {
                    set has_replica 1
                    if {$::multi_node_primary == -1 || $::multi_node_primary == $::leader_idx} {
                        set ::multi_node_primary $j
                        set ::multi_node_replica $k
                    }
                    break
                }
            }
            if {!$has_replica} {
                set ::single_node_primary $j
            }
        }
        assert {$::single_node_primary != -1}
        assert {$::multi_node_primary != -1}
        assert {$::multi_node_replica != -1}
        puts "Leader=#$::leader_idx, single-node-primary=#$::single_node_primary, multi-node-primary=#$::multi_node_primary, replica=#$::multi_node_replica"
    }

    test "Wait for multi-node replica to sync" {
        wait_for_condition 1000 50 {
            [s [expr -1*$::multi_node_replica] master_link_status] eq {up}
        } else {
            fail "Replica #$::multi_node_replica master link not up"
        }
    }

    test "Single-node shard uses AE_ACK-based failure detection" {
        # The raft leader monitors single-node shards via AE_ACK.
        # Skip if the single-node primary is itself the raft leader.
        if {$::single_node_primary == $::leader_idx} {
            puts "SKIP: single-node primary is the raft leader"
            return
        }

        set loglines [count_log_lines [expr -1*$::leader_idx]]
        set single_id [R $::single_node_primary CLUSTER MYID]

        pause_process [srv [expr -1*$::single_node_primary] pid]

        # Wait for the AE_ACK-based detection log on the raft leader.
        wait_for_log_messages [expr -1*$::leader_idx] \
            {"*not responding (AE_ACK), proposing NODE_FAIL*"} \
            $loglines 2000 50

        # Verify NODE_FAIL is applied — node shows fail flag in CLUSTER NODES.
        wait_for_condition 1000 50 {
            [string match "*$single_id*fail*" [R $::leader_idx CLUSTER NODES]]
        } else {
            fail "NODE_FAIL not applied for single-node shard primary"
        }

        resume_process [srv [expr -1*$::single_node_primary] pid]

        # Wait for recovery — fail flag cleared.
        wait_for_condition 1000 50 {
            ![string match "*$single_id*fail*" [R $::leader_idx CLUSTER NODES]]
        } else {
            fail "Single-node shard primary did not recover"
        }
    }

    test "Multi-node shard uses replication-stream failure detection" {
        # The shard members (primary or replica) detect failure via replication
        # stream, NOT via AE_ACK on the leader. Kill the multi-node primary
        # and verify the repl-stream detection log appears.
        set loglines [count_log_lines [expr -1*$::multi_node_replica]]

        pause_process [srv [expr -1*$::multi_node_primary] pid]

        # Wait for the replication-stream detection log on the replica.
        wait_for_log_messages [expr -1*$::multi_node_replica] \
            {"*Replication stream timeout: proposing NODE_FAIL*"} \
            $loglines 2000 50

        # Verify failover completes.
        wait_for_condition 1000 50 {
            [s [expr -1*$::multi_node_replica] role] eq {master}
        } else {
            fail "Replica #$::multi_node_replica did not fail over"
        }

        resume_process [srv [expr -1*$::multi_node_primary] pid]

        # Wait for old primary to become replica.
        wait_for_condition 1000 50 {
            [s [expr -1*$::multi_node_primary] role] eq {slave}
        } else {
            fail "Old primary #$::multi_node_primary did not become replica"
        }
    }

} ;# start_cluster

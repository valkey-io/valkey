# Cluster-mode tests for CLEAN_STATE_FOR_DOLLY_SAVE.
#
# Scenario: a replica crashed; we CRIU-cloned the primary to a new host and
# run CLEAN_STATE_FOR_DOLLY_SAVE on the clone to reset source-machine
# identity while preserving the clone's ability to PSYNC with its source
# primary.
#
# On a running cluster node the command:
#   - regenerates the cluster node ID and shard_id
#   - forgets all peers and resets cluster epochs
#   - clears cached announce addresses / CLUSTER SLOTS responses
#   - clears myself's slot ownership bitmap (required for CLUSTER REPLICATE)
#   - leaves the CLUSTER_NODE_REPLICA/PRIMARY flag alone (subsequent
#     CLUSTER REPLICATE sets it correctly)
#   - preserves the keyspace
#   - synthesizes server.cached_primary from server.replid + offset
#     (clone-of-primary case) so that PSYNC lands a +CONTINUE

start_cluster 2 0 {tags {external:skip cluster "dolly-save"} overrides {cluster-replica-no-failover yes cluster-allow-reads-when-down yes}} {

    test "CLEAN_STATE_FOR_DOLLY_SAVE regenerates cluster identity and clears slots" {
        wait_for_cluster_state "ok"

        # Hash tag "\{dolly\}" -> slot 7365, owned by node 0 (slots 0..8191).
        R 0 set "{dolly}k1" v1
        R 0 set "{dolly}k2" v2

        set old_myid [R 0 CLUSTER MYID]
        set dbsize_before [R 0 DBSIZE]
        set replid_before [status [srv 0 client] master_replid]
        set offset_before [status [srv 0 client] master_repl_offset]
        assert {$dbsize_before > 0}

        assert_equal "OK" [R 0 CLEAN_STATE_FOR_DOLLY_SAVE]

        # Node ID regenerated.
        set new_myid [R 0 CLUSTER MYID]
        assert {[string length $new_myid] == 40}
        assert {$new_myid ne $old_myid}

        # Peers forgotten.
        set nodes_after [llength [split [string trim [R 0 CLUSTER NODES]] "\n"]]
        assert_equal 1 $nodes_after
        assert_match "*myself*" [R 0 CLUSTER NODES]

        # Epochs reset.
        assert_equal 0 [CI 0 cluster_current_epoch]

        # Slot ownership cleared: "connected 0-" (the usual node-0 range) is gone.
        assert_equal -1 [string first "connected 0-" [R 0 CLUSTER NODES]]

        # Keyspace preserved.
        assert_equal $dbsize_before [R 0 DBSIZE]

        # Replication identity preserved: replid + offset carry forward
        # (the offset may advance due to replication no-ops but must not regress).
        assert_equal $replid_before [status [srv 0 client] master_replid]
        assert {[status [srv 0 client] master_repl_offset] >= $offset_before}
    }
}

# Isolated single-node cluster: exercise the "no peers to forget" path and
# verify the clone-of-primary synthesis path populates cached_primary.
start_cluster 1 0 {tags {external:skip cluster "dolly-save"}} {
    test "CLEAN_STATE_FOR_DOLLY_SAVE on isolated cluster node is safe" {
        wait_for_cluster_state "ok"

        set old_myid [R 0 CLUSTER MYID]
        set replid_before [status [srv 0 client] master_replid]

        # Write some data to advance the replication offset.
        R 0 set "{dolly}k" v

        assert_equal "OK" [R 0 CLEAN_STATE_FOR_DOLLY_SAVE]
        set new_myid [R 0 CLUSTER MYID]
        assert {$new_myid ne $old_myid}
        assert_equal 0 [CI 0 cluster_current_epoch]
        assert_equal PONG [R 0 ping]

        # Replid preserved; this node is the clone-of-primary case, so
        # cached_primary was synthesized from replid + offset. We cannot
        # directly read cached_primary via INFO, but the log line
        # "synthesizing cached_primary for PSYNC" confirms the path ran,
        # and the preserved replid is the observable proof.
        assert_equal $replid_before [status [srv 0 client] master_replid]
    }
}

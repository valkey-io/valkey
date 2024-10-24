start_cluster 2 2 {tags {external:skip cluster}} {

    test "Verify that slot ownership transfer through gossip propagates deletes to replicas" {
        assert {[s -2 role] eq {slave}}
        wait_for_condition 1000 50 {
            [s -2 master_link_status] eq {up}
        } else {
            fail "Instance #2 master link status is not up"
        }

        assert {[s -3 role] eq {slave}}
        wait_for_condition 1000 50 {
            [s -3 master_link_status] eq {up}
        } else {
            fail "Instance #3 master link status is not up"
        }

        # Set a single key that will be used to test deletion
        set key "FOO"
        R 0 SET $key TEST
        set key_slot [R 0 cluster keyslot $key]
        set slot_keys_num [R 0 cluster countkeysinslot $key_slot]
        assert {$slot_keys_num > 0}

        # Wait for replica to have the key
        R 2 readonly
        wait_for_condition 1000 50 {
            [R 2 exists $key] eq "1"
        } else {
            fail "Test key was not replicated"
        }

        assert_equal [R 2 cluster countkeysinslot $key_slot] $slot_keys_num

        # Assert other shards in cluster doesn't have the key
        assert_equal [R 1 cluster countkeysinslot $key_slot] "0"
        assert_equal [R 3 cluster countkeysinslot $key_slot] "0"

        set nodeid [R 1 cluster myid]

        R 1 cluster bumpepoch
        # Move $key_slot to node 1
        assert_equal [R 1 cluster setslot $key_slot node $nodeid] "OK"

        wait_for_cluster_propagation

        # src master will delete keys in the slot
        wait_for_condition 50 100 {
            [R 0 cluster countkeysinslot $key_slot] eq 0
        } else {
            fail "master 'countkeysinslot $key_slot' did not eq 0"
        }

        # src replica will delete keys in the slot
        wait_for_condition 50 100 {
            [R 2 cluster countkeysinslot $key_slot] eq 0
        } else {
            fail "replica 'countkeysinslot $key_slot' did not eq 0"
        }
    }
}

start_cluster 3 1 {tags {external:skip cluster} overrides {shutdown-timeout 100}} {
    test "Primary lost a slot during the shutdown waiting" {
        R 0 set FOO 0

        # Pause the replica.
        pause_process [srv -3 pid]

        # Incr the key and immediately shutdown the primary.
        # The primary waits for the replica to replicate before exiting.
        R 0 incr FOO
        exec kill -SIGTERM [srv 0 pid]
        wait_for_condition 50 100 {
            [s 0 shutdown_in_milliseconds] > 0
        } else {
            fail "Primary not indicating ongoing shutdown."
        }

        # Move the slot to other primary
        R 1 cluster bumpepoch
        R 1 cluster setslot [R 1 cluster keyslot FOO] node [R 1 cluster myid]

        # Waiting for dirty slot update.
        wait_for_log_messages 0 {"*Deleting keys in dirty slot*"} 0 1000 10

        # Resume the replica and make sure primary exits normally instead of crashing.
        resume_process [srv -3 pid]
        wait_for_log_messages 0 {"*Valkey is now ready to exit, bye bye*"} 0 1000 10

        # Make sure that the replica will become the new primary and does not own the key.
        wait_for_condition 1000 50 {
            [s -3 role] eq {master}
        } else {
            fail "The replica was not converted into primary"
        }
        assert_error {ERR no such key} {R 3 debug object foo}
    }
}

start_cluster 3 1 {tags {external:skip cluster}} {
    test "Primary lost a slot during the manual failover pausing" {
        R 0 set FOO 0

        # Set primaries to drop the FAILOVER_AUTH_REQUEST packets, so that
        # primary 0 will pause until the failover times out.
        R 1 debug drop-cluster-packet-filter 5
        R 2 debug drop-cluster-packet-filter 5

        # Replica doing the manual failover.
        R 3 cluster failover

        # Move the slot to other primary
        R 1 cluster bumpepoch
        R 1 cluster setslot [R 1 cluster keyslot FOO] node [R 1 cluster myid]

        # Waiting for dirty slot update.
        wait_for_log_messages 0 {"*Deleting keys in dirty slot*"} 0 1000 10

        # Make sure primary doesn't crash when deleting the keys.
        R 0 ping

        R 1 debug drop-cluster-packet-filter -1
        R 2 debug drop-cluster-packet-filter -1
    }
}

start_cluster 3 1 {tags {external:skip cluster}} {
    test "Primary lost a slot during the client pause command" {
        R 0 set FOO 0

        R 0 client pause 1000000000 write

        # Move the slot to other primary
        R 1 cluster bumpepoch
        R 1 cluster setslot [R 1 cluster keyslot FOO] node [R 1 cluster myid]

        # Waiting for dirty slot update.
        wait_for_log_messages 0 {"*Deleting keys in dirty slot*"} 0 1000 10

        # Make sure primary doesn't crash when deleting the keys.
        R 0 ping

        R 0 client unpause
    }
}

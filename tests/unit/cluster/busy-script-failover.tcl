proc test_busy_script {type} {
    test "Kill the busy script during failover - $type" {
        set rd [valkey_deferring_client 0]
        R 0 config set busy-reply-threshold 10

        # key_977613 belong to slot 0 and belong to R 0.
        R 0 set key_977613 123456

        if {$type == "write"} {
            # This is a while true write script.
            $rd eval {while true do server.call('INCR', KEYS[1]) end} 1 key_977613
        } elseif {$type == "read"} {
            # This is a while true read script, and then an unreachable write command.
            $rd eval {while true do server.call('GET', KEYS[1]) end; server.call('INCR', KEYS[1])} 1 key_977613
        }

        # Make sure R 0 detected the slow script.
        wait_for_log_messages 0 {"*Slow script detected*"} 0 2000 1

        # Trigger a failover.
        R 3 cluster failover

        # Make sure busy script is detected during the failover.
        wait_for_log_messages 0 {"*Slow script detected during the failover*"} 0 1000 50

        # We will end up with an error since the script will eventually be killed.
        # It used to return a `Script attempted to access a non local key in a cluster node`
        # error because node lost its primaryship.
        assert_error {ERR Script killed due to cluster failover*} {$rd read}

        # Wait for failover.
        wait_for_condition 1000 50 {
            [s 0 role] == "slave" &&
            [s -3 role] == "master"
        } else {
            fail "No failover detected"
        }

        # Write a new key and make sure the replica got the new update.
        R 3 set "{key_977613}-foo" bar
        R 0 readonly
        wait_for_condition 1000 50 {
            [R 0 get "{key_977613}-foo"] == {bar}
        } else {
            fail "No failover detected"
        }

        # Make sure full sync and psync are detected.
        if {$type == "write"} {
            verify_log_message 0 "*Partial resynchronization not possible*" 0
        } elseif {$type == "read"} {
            verify_log_message 0 "*Successful partial resynchronization*" 0
        }

        # Make sure that node data is eventually consistent.
        assert_equal [R 0 get key_977613] [R 3 get key_977613]
        assert_equal [R 0 debug digest] [R 3 debug digest]

        $rd close
    }
}

start_cluster 3 1 {tags {external:skip cluster} overrides {cluster-replica-no-failover yes}} {
    test_busy_script "write"
}

start_cluster 3 1 {tags {external:skip cluster} overrides {cluster-replica-no-failover yes}} {
    test_busy_script "read"
}

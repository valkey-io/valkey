proc shutdown_on_how {srv_id how} {
    if {$how == "shutdown"} {
        catch {R $srv_id shutdown nosave}
    } elseif {$how == "sigterm"} {
        exec kill -SIGTERM [s -$srv_id process_id]
    }
}

proc test_main {how} {
    test "auto-failover-on-shutdown will always pick a best replica and send CLUSTER FAILOVER - $how" {
        set primary [srv 0 client]
        set replica1 [srv -3 client]
        set replica1_pid [s -3 process_id]
        set replica2 [srv -6 client]
        set replica2_ip [srv -6 host]
        set replica2_port [srv -6 port]

        # Pause a replica so it has no chance to catch up with the offset.
        pause_process $replica1_pid

        # Primary write some data to increse the offset.
        for {set i 0} {$i < 10} {incr i} {
            $primary incr key_991803
        }

        # Wait the replica2 catch up with the offset
        wait_replica_acked_ofs $primary $replica2_ip $replica2_port

        # Shutdown the primary.
        shutdown_on_how 0 $how

        # Wait for the replica2 to become a primary.
        wait_for_condition 1000 50 {
            [s -6 role] eq {master}
        } else {
            puts "s -6 role: [s -6 role]"
            fail "Failover does not happened"
        }

        # Make sure that the expected logs are printed.
        verify_log_message 0 "*Sending CLUSTER FAILOVER FORCE to replica*" 0
        verify_log_message -6 "*Forced failover primary request accepted*" 0

        resume_process $replica1_pid
    }

    test "Unable to find a replica to perform an auto failover - $how" {
        set primary [srv -6 client]
        set replica1 [srv -3 client]
        set replica1_pid [s -3 process_id]

        pause_process $replica1_pid

        $primary client kill type replica
        shutdown_on_how 6 $how
        wait_for_log_messages -6 {"*Unable to find a replica to perform an auto failover on shutdown*"} 0 1000 10

        resume_process $replica1_pid
    }
}

start_cluster 3 4 {tags {external:skip cluster} overrides {cluster-ping-interval 1000 cluster-node-timeout 5000 shutdown-timeout 0 auto-failover-on-shutdown yes}} {
    test_main "shutdown"
}

start_cluster 3 4 {tags {external:skip cluster} overrides {cluster-ping-interval 1000 cluster-node-timeout 5000 shutdown-timeout 0 auto-failover-on-shutdown yes}} {
    test_main "sigterm"
}

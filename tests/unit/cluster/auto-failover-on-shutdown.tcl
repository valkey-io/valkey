proc shutdown_how {srv_id how} {
    if {$how == "shutdown"} {
        catch {R $srv_id shutdown nosave failover}
    } elseif {$how == "sigterm"} {
        exec kill -SIGTERM [s -$srv_id process_id]
    }
}

# We will start a cluster with 3 primary nodes and 4 replicas, the primary 1 will have 2 replicas.
# We will pause the replica 1, and then shutdown the primary 1, and making replica 2 to become
# the new primary.
proc test_auto_failover {how shutdown_timeout} {
    test "Failover on shutdown hands over primaryship to a fully sync'd replica - $how - shutdown-timeout: $shutdown_timeout" {
        set primary [srv 0 client]
        set replica1 [srv -3 client]
        set replica1_pid [s -3 process_id]
        set replica2 [srv -6 client]
        set replica2_host [srv -6 host]
        set replica2_port [srv -6 port]

        if {$how == "sigterm"} {
            $primary config set shutdown-on-sigterm failover
        }
        $primary config set shutdown-timeout $shutdown_timeout
        $primary config set repl-ping-replica-period 3600

        # Prevent timeout-based failover without making replica2 ineligible.
        for {set id 0} {$id < 7} {incr id} {
            R $id config set cluster-node-timeout 120000
        }

        # Pause a replica so it has no chance to catch up with the offset.
        pause_process $replica1_pid

        # Primary write some data to increase the offset.
        for {set i 0} {$i < 10} {incr i} {
            $primary incr key_991803
        }

        if {$shutdown_timeout == 0} {
            # Wait the replica2 catch up with the offset
            wait_for_ofs_sync $primary $replica2
            wait_replica_acked_ofs $primary $replica2 $replica2_host $replica2_port
        } else {
            # If shutdown-timeout is enable, we expect the primary to pause writing
            # and wait for the replica to catch up with the offset.
        }

        # Shutdown the primary.
        shutdown_how 0 $how

        # Wait for the replica2 to become a primary.
        wait_for_condition 1000 50 {
            [s -6 role] eq {master}
        } else {
            puts "s -6 role: [s -6 role]"
            fail "Failover does not happened"
        }

        # Make sure that the expected logs are printed.
        verify_log_message -6 "*Forced failover primary request accepted*" 0

        resume_process $replica1_pid
    }

    test "Unable to find a replica to perform an auto failover - $how" {
        set primary [srv -6 client]
        set replica1 [srv -3 client]
        set replica1_pid [s -3 process_id]

        pause_process $replica1_pid

        if {$how == "sigterm"} {
            $primary config set shutdown-on-sigterm failover
        }
        $primary client kill type replica
        shutdown_how 6 $how
        wait_for_log_messages -6 {"*Unable to find a replica to perform the auto failover on shutdown*"} 0 1000 10

        resume_process $replica1_pid
    }
}

start_cluster 3 4 {tags {external:skip cluster}} {
    test_auto_failover "shutdown" 0
}

start_cluster 3 4 {tags {external:skip cluster}} {
    test_auto_failover "sigterm" 0
}

start_cluster 3 4 {tags {external:skip cluster}} {
    test_auto_failover "shutdown" 10
}

start_cluster 3 4 {tags {external:skip cluster}} {
    test_auto_failover "sigterm" 10
}

proc test_shutdown_respects_nofailover {how {all_nofailover 0}} {
    test "Shutdown respects nofailover - $how - all nofailover: $all_nofailover" {
        set primary [srv 0 client]
        set preferred [srv -3 client]
        set nofailover [srv -6 client]
        set preferred_id [$preferred cluster myid]
        set nofailover_id [$nofailover cluster myid]

        $primary config set shutdown-timeout 0
        if {$how eq "sigterm"} {
            $primary config set shutdown-on-sigterm failover
        }
        $nofailover config set cluster-replica-no-failover yes
        if {$all_nofailover} {
            $preferred config set cluster-replica-no-failover yes
        }
        wait_for_condition 1000 50 {
            [cluster_has_flag [cluster_get_node_by_id 0 $nofailover_id] nofailover] &&
            (!$all_nofailover ||
             [cluster_has_flag [cluster_get_node_by_id 0 $preferred_id] nofailover])
        } else {
            fail "Primary did not receive the nofailover flags"
        }

        # Reconnect the nofailover replica last so the old selection would pick it.
        wait_replica_acked_ofs $primary $preferred [srv -3 host] [srv -3 port]
        $nofailover client kill type master
        wait_for_condition 1000 50 {
            [s -6 master_link_status] eq "up"
        } else {
            fail "Nofailover replica did not reconnect"
        }
        $primary incr key_991803
        wait_replica_acked_ofs $primary $preferred [srv -3 host] [srv -3 port]
        wait_replica_acked_ofs $primary $nofailover [srv -6 host] [srv -6 port]
        $primary config set repl-ping-replica-period 3600
        wait_for_condition 1000 50 {
            [check_replica_acked_ofs $primary [srv -3 host] [srv -3 port]] &&
            [check_replica_acked_ofs $primary [srv -6 host] [srv -6 port]]
        } else {
            fail "Replicas did not acknowledge the final offset"
        }

        shutdown_how 0 $how
        if {$all_nofailover} {
            wait_for_log_messages 0 {"*Unable to find a replica to perform the auto failover on shutdown*"} 0 1000 10
            wait_for_log_messages 0 {"*Valkey is now ready to exit, bye bye*"} 0 1000 10
            assert_equal 0 [count_log_message 0 "*Perform auto failover to replica*"]
            assert_equal "slave" [s -3 role]
            assert_equal 0 [count_log_message -3 "*Forced failover primary request accepted*"]
        } else {
            wait_for_condition 1000 50 {
                [s -3 role] eq "master"
            } else {
                fail "Expected replica was not promoted"
            }
            verify_log_message -3 "*Forced failover primary request accepted*" 0
        }
        assert_equal "slave" [s -6 role]
        assert_equal 0 [count_log_message -6 "*Forced failover primary request accepted*"]
    }
}

start_cluster 3 4 {tags {external:skip cluster}} {
    test_shutdown_respects_nofailover shutdown
}

start_cluster 3 4 {tags {external:skip cluster}} {
    test_shutdown_respects_nofailover sigterm
}

start_cluster 3 4 {tags {external:skip cluster}} {
    test_shutdown_respects_nofailover shutdown 1
}

start_cluster 3 4 {tags {external:skip cluster}} {
    test_shutdown_respects_nofailover sigterm 1
}

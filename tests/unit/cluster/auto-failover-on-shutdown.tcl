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

        # To avoid failover kick in.
        $replica2 config set cluster-replica-no-failover yes

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

proc test_shutdown_prefers_failover_enabled {how {all_nofailover 0}} {
    test "Shutdown prefers failover-enabled replicas - $how - all nofailover: $all_nofailover" {
        set primary [srv 0 client]
        set preferred [srv -3 client]
        set fallback [srv -6 client]
        set preferred_id [$preferred cluster myid]
        set fallback_id [$fallback cluster myid]
        set promoted -3
        set retained -6

        $primary config set shutdown-timeout 0
        if {$how eq "sigterm"} {
            $primary config set shutdown-on-sigterm failover
        }
        $fallback config set cluster-replica-no-failover yes
        if {$all_nofailover} {
            $preferred config set cluster-replica-no-failover yes
            set promoted -6
            set retained -3
        }
        wait_for_condition 1000 50 {
            [cluster_has_flag [cluster_get_node_by_id 0 $fallback_id] nofailover] &&
            (!$all_nofailover ||
             [cluster_has_flag [cluster_get_node_by_id 0 $preferred_id] nofailover])
        } else {
            fail "Primary did not receive the nofailover flags"
        }

        # Reconnect the nofailover replica last so the old selection would pick it.
        wait_replica_acked_ofs $primary $preferred [srv -3 host] [srv -3 port]
        $fallback client kill type master
        wait_for_condition 1000 50 {
            [s -6 master_link_status] eq "up"
        } else {
            fail "Nofailover replica did not reconnect"
        }
        $primary incr key_991803
        wait_replica_acked_ofs $primary $preferred [srv -3 host] [srv -3 port]
        wait_replica_acked_ofs $primary $fallback [srv -6 host] [srv -6 port]
        $primary config set repl-ping-replica-period 3600
        wait_for_condition 1000 50 {
            [check_replica_acked_ofs $primary [srv -3 host] [srv -3 port]] &&
            [check_replica_acked_ofs $primary [srv -6 host] [srv -6 port]]
        } else {
            fail "Replicas did not acknowledge the final offset"
        }

        shutdown_how 0 $how
        wait_for_condition 1000 50 {
            [s $promoted role] eq "master"
        } else {
            fail "Expected replica was not promoted"
        }
        verify_log_message $promoted "*Forced failover primary request accepted*" 0
        assert_equal "slave" [s $retained role]
        assert_equal 0 [count_log_message $retained "*Forced failover primary request accepted*"]
    }
}

start_cluster 3 4 {tags {external:skip cluster}} {
    test_shutdown_prefers_failover_enabled shutdown
}

start_cluster 3 4 {tags {external:skip cluster}} {
    test_shutdown_prefers_failover_enabled sigterm
}

start_cluster 3 4 {tags {external:skip cluster}} {
    test_shutdown_prefers_failover_enabled shutdown 1
}

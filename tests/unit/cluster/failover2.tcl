# Check the basic monitoring and failover capabilities.

start_cluster 3 4 {tags {external:skip cluster} overrides {cluster-ping-interval 1000 cluster-node-timeout 5000}} {

    test "Cluster is up" {
        wait_for_cluster_state ok
    }

    test "Cluster is writable" {
        cluster_write_test [srv 0 port]
    }

    set paused_pid [srv 0 pid]
    test "Killing one primary node" {
        pause_process $paused_pid
    }

    test "Wait for failover" {
        wait_for_condition 1000 50 {
            [s -3 role] == "master" || [s -6 role] == "master"
        } else {
            fail "No failover detected"
        }
    }

    test "Killing the new primary node" {
        if {[s -3 role] == "master"} {
            set replica_to_be_primary -6
            set paused_pid2 [srv -3 pid]
        } else {
            set replica_to_be_primary -3
            set paused_pid2 [srv -6 pid]
        }
        pause_process $paused_pid2
    }

    test "Cluster should eventually be up again" {
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            if {[process_is_paused [srv -$j pid]]} continue
            wait_for_condition 1000 50 {
                [CI $j cluster_state] eq "ok"
            } else {
                fail "Cluster node $j cluster_state:[CI $j cluster_state]"
            }
        }
    }

    test "wait for new failover" {
        wait_for_condition 1000 50 {
            [s $replica_to_be_primary role] == "master"
        } else {
            fail "No failover detected"
        }
    }

    test "Restarting the previously killed primary nodes" {
        resume_process $paused_pid
        resume_process $paused_pid2
    }

    test "Make sure there is no failover timeout" {
        verify_no_log_message -3 "*Failover attempt expired*" 0
        verify_no_log_message -6 "*Failover attempt expired*" 0
    }
} ;# start_cluster

start_cluster 7 3 {tags {external:skip cluster} overrides {cluster-ping-interval 1000 cluster-node-timeout 5000}} {
    test "Primaries will not time out then they are elected in the same epoch" {
        # Since we have the delay time, so these node may not initiate the
        # election at the same time (same epoch). But if they do, we make
        # sure there is no failover timeout.

        # Killing there primary nodes.
        pause_process [srv 0 pid]
        pause_process [srv -1 pid]
        pause_process [srv -2 pid]

        # Wait for the failover
        wait_for_condition 1000 50 {
            [s -7 role] == "master" &&
            [s -8 role] == "master" &&
            [s -9 role] == "master"
        } else {
            fail "No failover detected"
        }

        # Make sure there is no false epoch 0.
        verify_no_log_message -7 "*Failover election in progress for epoch 0*" 0
        verify_no_log_message -8 "*Failover election in progress for epoch 0*" 0
        verify_no_log_message -9 "*Failover election in progress for epoch 0*" 0

        # Make sure there is no failover timeout.
        verify_no_log_message -7 "*Failover attempt expired*" 0
        verify_no_log_message -8 "*Failover attempt expired*" 0
        verify_no_log_message -9 "*Failover attempt expired*" 0

        # Resuming these primary nodes, speed up the shutdown.
        resume_process [srv 0 pid]
        resume_process [srv -1 pid]
        resume_process [srv -2 pid]
    }
} ;# start_cluster

run_solo {cluster} {
    start_cluster 32 15 {tags {external:skip cluster} overrides {cluster-ping-interval 1000 cluster-node-timeout 15000}} {
        test "Multiple primary nodes are down, rank them based on the failed primary" {
            # Killing these primary nodes.
            for {set j 0} {$j < 15} {incr j} {
                pause_process [srv -$j pid]
            }

            # Make sure that a node starts failover.
            wait_for_condition 1000 100 {
                [s -40 role] == "master"
            } else {
                fail "No failover detected"
            }

            # Wait for the cluster state to become ok.
            for {set j 0} {$j < [llength $::servers]} {incr j} {
                if {[process_is_paused [srv -$j pid]]} continue
                wait_for_condition 1000 100 {
                    [CI $j cluster_state] eq "ok"
                } else {
                    fail "Cluster node $j cluster_state:[CI $j cluster_state]"
                }
            }

            # Resuming these primary nodes, speed up the shutdown.
            for {set j 0} {$j < 15} {incr j} {
                resume_process [srv -$j pid]
            }
        }
    } ;# start_cluster
} ;# run_solo

# Needs to run in the body of
# start_cluster 3 1 {tags {external:skip cluster} overrides {cluster-replica-validity-factor 0}}
proc test_replica_config_epoch_failover {type} {
    test "Replica can update the config epoch when trigger the failover - $type" {
        set CLUSTER_PACKET_TYPE_NONE -1
        set CLUSTER_PACKET_TYPE_ALL -2

        if {$type == "automatic"} {
            R 3 CONFIG SET cluster-replica-no-failover no
        } elseif {$type == "manual"} {
            R 3 CONFIG SET cluster-replica-no-failover yes
        }
        R 3 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_ALL
        R 3 DEBUG CLOSE-CLUSTER-LINK-ON-PACKET-DROP 1

        set R0_nodeid [R 0 cluster myid]

        # R 0 is the first node, so we expect its epoch to be the smallest,
        # so bumpepoch must succeed and it's config epoch will be changed.
        set res [R 0 cluster bumpepoch]
        assert_match {BUMPED *} $res
        set R0_config_epoch [lindex $res 1]

        # Wait for the config epoch to propagate across the cluster.
        wait_for_condition 1000 10 {
            $R0_config_epoch == [dict get [cluster_get_node_by_id 1 $R0_nodeid] config_epoch] &&
            $R0_config_epoch == [dict get [cluster_get_node_by_id 2 $R0_nodeid] config_epoch]
        } else {
            fail "Other primaries does not update config epoch"
        }
        # Make sure that replica do not update config epoch.
        assert_not_equal $R0_config_epoch [dict get [cluster_get_node_by_id 3 $R0_nodeid] config_epoch]

        # Pause the R 0 and wait for the cluster to be down.
        pause_process [srv 0 pid]
        R 3 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_NONE
        R 3 DEBUG CLOSE-CLUSTER-LINK-ON-PACKET-DROP 0
        wait_for_condition 1000 50 {
            [CI 1 cluster_state] == "fail" &&
            [CI 2 cluster_state] == "fail" &&
            [CI 3 cluster_state] == "fail"
        } else {
            fail "Cluster does not fail"
        }

        # Make sure both the automatic and the manual failover will fail in the first time.
        if {$type == "automatic"} {
            wait_for_log_messages -3 {"*Failover attempt expired*"} 0 1000 10
        } elseif {$type == "manual"} {
            R 3 cluster failover force
            wait_for_log_messages -3 {"*Manual failover timed out*"} 0 1000 10
        }

        # Make sure the primaries prints the relevant logs.
        wait_for_log_messages -1 {"*Failover auth denied to* epoch * > reqConfigEpoch*"} 0 1000 10
        wait_for_log_messages -1 {"*has old slots configuration, sending an UPDATE message about*"} 0 1000 10
        wait_for_log_messages -2 {"*Failover auth denied to* epoch * > reqConfigEpoch*"} 0 1000 10
        wait_for_log_messages -2 {"*has old slots configuration, sending an UPDATE message about*"} 0 1000 10

        # Make sure the replica has updated the config epoch.
        wait_for_condition 1000 10 {
            $R0_config_epoch == [dict get [cluster_get_node_by_id 1 $R0_nodeid] config_epoch]
        } else {
            fail "The replica does not update the config epoch"
        }

        if {$type == "manual"} {
            # The second manual failure will succeed because the config epoch
            # has already propagated.
            R 3 cluster failover force
        }

        # Wait for the failover to success.
        wait_for_condition 1000 50 {
            [s -3 role] == "master" &&
            [CI 1 cluster_state] == "ok" &&
            [CI 2 cluster_state] == "ok" &&
            [CI 3 cluster_state] == "ok"
        } else {
            fail "Failover does not happen"
        }

        # Restore the old primary, make sure it can covert
        resume_process [srv 0 pid]
        wait_for_condition 1000 50 {
            [s 0 role] == "slave" &&
            [CI 0 cluster_state] == "ok"
        } else {
            fail "The old primary was not converted into replica"
        }
    }
}

start_cluster 3 1 {tags {external:skip cluster} overrides {cluster-replica-validity-factor 0}} {
    test_replica_config_epoch_failover "automatic"
}

start_cluster 3 1 {tags {external:skip cluster} overrides {cluster-replica-validity-factor 0}} {
    test_replica_config_epoch_failover "manual"
}

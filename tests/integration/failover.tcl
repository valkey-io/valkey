start_server {tags {"failover external:skip"} overrides {save {}}} {
start_server {overrides {save {}}} {
start_server {overrides {save {}}} {
    set node_0 [srv 0 client]
    set node_0_host [srv 0 host]
    set node_0_port [srv 0 port]
    set node_0_pid [srv 0 pid]

    set node_1 [srv -1 client]
    set node_1_host [srv -1 host]
    set node_1_port [srv -1 port]
    set node_1_pid [srv -1 pid]

    set node_2 [srv -2 client]
    set node_2_host [srv -2 host]
    set node_2_port [srv -2 port]
    set node_2_pid [srv -2 pid]

    proc assert_digests_match {n1 n2 n3} {
        assert_equal [$n1 debug digest] [$n2 debug digest]
        assert_equal [$n2 debug digest] [$n3 debug digest]
    }

    test {failover command fails without connected replica} {
        catch { $node_0 failover to $node_1_host $node_1_port } err
        if {! [string match "ERR*" $err]} {
            fail "failover command succeeded when replica not connected"
        }
    }

    test {setup replication for following tests} {
        $node_1 replicaof $node_0_host $node_0_port
        $node_2 replicaof $node_0_host $node_0_port
        wait_for_sync $node_1
        wait_for_sync $node_2
    }

    test {failover command fails with invalid host} {
        catch { $node_0 failover to invalidhost $node_1_port } err
        assert_match "ERR*" $err
    }

    test {failover command fails with invalid port} {
        catch { $node_0 failover to $node_1_host invalidport } err
        assert_match "ERR*" $err
    }

    test {failover command fails with just force and timeout} {
        catch { $node_0 FAILOVER FORCE TIMEOUT 100} err
        assert_match "ERR*" $err
    }

    test {failover command fails when sent to a replica} {
        catch { $node_1 failover to $node_1_host $node_1_port } err
        assert_match "ERR*" $err
    }

    test {failover command fails with force without timeout} {
        catch { $node_0 failover to $node_1_host $node_1_port FORCE } err
        assert_match "ERR*" $err
    }

    test {failover command to specific replica works} {
        set initial_psyncs [s -1 sync_partial_ok]
        set initial_syncs [s -1 sync_full]

        # Generate a delta between primary and replica
        set load_handler [start_write_load $node_0_host $node_0_port 5]
        pause_process [srv -1 pid]
        wait_for_condition 50 100 {
            [s 0 total_commands_processed] > 100
        } else {
            fail "Node 0 did not accept writes"
        }
        resume_process [srv -1 pid]

        # Execute the failover
        $node_0 failover to $node_1_host $node_1_port

        # Wait for failover to end
        wait_for_condition 50 100 {
            [s 0 master_failover_state] == "no-failover"
        } else {
            fail "Failover from node 0 to node 1 did not finish"
        }

        # stop the write load and make sure no more commands processed
        stop_write_load $load_handler
        wait_load_handlers_disconnected

        $node_2 replicaof $node_1_host $node_1_port
        wait_for_sync $node_0
        wait_for_sync $node_2

        assert_match *slave* [$node_0 role]
        assert_match *master* [$node_1 role]
        assert_match *slave* [$node_2 role]

        # We should accept psyncs from both nodes
        assert_equal [expr [s -1 sync_partial_ok] - $initial_psyncs] 2
        assert_equal [expr [s -1 sync_full] - $initial_psyncs] 0
        assert_digests_match $node_0 $node_1 $node_2
    }

    test {failover command to any replica works} {
        set initial_psyncs [s -2 sync_partial_ok]
        set initial_syncs [s -2 sync_full]

        wait_for_ofs_sync $node_1 $node_2
        # We stop node 0 to and make sure node 2 is selected
        pause_process $node_0_pid
        $node_1 set CASE 1
        $node_1 FAILOVER

        # Wait for failover to end
        wait_for_condition 50 100 {
            [s -1 master_failover_state] == "no-failover"
        } else {
            fail "Failover from node 1 to node 2 did not finish"
        }
        resume_process $node_0_pid
        $node_0 replicaof $node_2_host $node_2_port

        wait_for_sync $node_0
        wait_for_sync $node_1

        assert_match *slave* [$node_0 role]
        assert_match *slave* [$node_1 role]
        assert_match *master* [$node_2 role]

        # We should accept Psyncs from both nodes
        assert_equal [expr [s -2 sync_partial_ok] - $initial_psyncs] 2
        assert_equal [expr [s -1 sync_full] - $initial_psyncs] 0
        assert_digests_match $node_0 $node_1 $node_2
    }

    test {failover to a replica with force works} {
        set initial_psyncs [s 0 sync_partial_ok]
        set initial_syncs [s 0 sync_full]

        pause_process $node_0_pid
        # node 0 will never acknowledge this write
        $node_2 set case 2
        $node_2 failover to $node_0_host $node_0_port TIMEOUT 100 FORCE

        # Wait for node 0 to give up on sync attempt and start failover
        wait_for_condition 50 100 {
            [s -2 master_failover_state] == "failover-in-progress"
        } else {
            fail "Failover from node 2 to node 0 did not timeout"
        }

        # Quick check that everyone is a replica, we never want a 
        # state where there are two masters.
        assert_match *slave* [$node_1 role]
        assert_match *slave* [$node_2 role]

        resume_process $node_0_pid

        # Wait for failover to end
        wait_for_condition 50 100 {
            [s -2 master_failover_state] == "no-failover"
        } else {
            fail "Failover from node 2 to node 0 did not finish"
        }
        $node_1 replicaof $node_0_host $node_0_port

        wait_for_sync $node_1
        wait_for_sync $node_2

        assert_match *master* [$node_0 role]
        assert_match *slave* [$node_1 role]
        assert_match *slave* [$node_2 role]

        assert_equal [count_log_message -2 "time out exceeded, failing over."] 1

        # We should accept both psyncs, although this is the condition we might not
        # since we didn't catch up.
        assert_equal [expr [s 0 sync_partial_ok] - $initial_psyncs] 2
        assert_equal [expr [s 0 sync_full] - $initial_syncs] 0
        assert_digests_match $node_0 $node_1 $node_2
    }

    test {failover with timeout aborts if replica never catches up} {
        set initial_psyncs [s 0 sync_partial_ok]
        set initial_syncs [s 0 sync_full]

        # Stop replica so it never catches up
        pause_process [srv -1 pid]
        $node_0 SET CASE 1
        
        $node_0 failover to [srv -1 host] [srv -1 port] TIMEOUT 500
        # Wait for failover to end
        wait_for_condition 50 20 {
            [s 0 master_failover_state] == "no-failover"
        } else {
            fail "Failover from node_0 to replica did not finish"
        }

        resume_process [srv -1 pid]

        # We need to make sure the nodes actually sync back up
        wait_for_ofs_sync $node_0 $node_1
        wait_for_ofs_sync $node_0 $node_2

        assert_match *master* [$node_0 role]
        assert_match *slave* [$node_1 role]
        assert_match *slave* [$node_2 role]

        # Since we never caught up, there should be no syncs
        assert_equal [expr [s 0 sync_partial_ok] - $initial_psyncs] 0
        assert_equal [expr [s 0 sync_full] - $initial_syncs] 0
        assert_digests_match $node_0 $node_1 $node_2
    }

    test {failovers can be aborted} {
        set initial_psyncs [s 0 sync_partial_ok]
        set initial_syncs [s 0 sync_full]
    
        # Stop replica so it never catches up
        pause_process [srv -1 pid]
        $node_0 SET CASE 2
        
        $node_0 failover to [srv -1 host] [srv -1 port] TIMEOUT 60000
        assert_match [s 0 master_failover_state] "waiting-for-sync"

        # Sanity check that read commands are still accepted
        $node_0 GET CASE

        $node_0 failover abort
        assert_match [s 0 master_failover_state] "no-failover"

        resume_process [srv -1 pid]

        # Just make sure everything is still synced
        wait_for_ofs_sync $node_0 $node_1
        wait_for_ofs_sync $node_0 $node_2

        assert_match *master* [$node_0 role]
        assert_match *slave* [$node_1 role]
        assert_match *slave* [$node_2 role]

        # Since we never caught up, there should be no syncs
        assert_equal [expr [s 0 sync_partial_ok] - $initial_psyncs] 0
        assert_equal [expr [s 0 sync_full] - $initial_syncs] 0
        assert_digests_match $node_0 $node_1 $node_2
    }

    test {failover aborts if target rejects sync request} {
        set initial_psyncs [s 0 sync_partial_ok]
        set initial_syncs [s 0 sync_full]

        # We block psync, so the failover will fail
        $node_1 acl setuser default -psync

        # We pause the target long enough to send a write command
        # during the pause. This write will not be interrupted.
        pause_process [srv -1 pid]
        set rd [redis_deferring_client]
        $rd SET FOO BAR
        $node_0 failover to $node_1_host $node_1_port
        resume_process [srv -1 pid]

        # Wait for failover to end
        wait_for_condition 50 100 {
            [s 0 master_failover_state] == "no-failover"
        } else {
            fail "Failover from node_0 to replica did not finish"
        }

        assert_equal [$rd read] "OK"
        $rd close

        # restore access to psync
        $node_1 acl setuser default +psync

        # We need to make sure the nodes actually sync back up
        wait_for_sync $node_1
        wait_for_sync $node_2

        assert_match *master* [$node_0 role]
        assert_match *slave* [$node_1 role]
        assert_match *slave* [$node_2 role]

        # We will cycle all of our replicas here and force a psync.
        assert_equal [expr [s 0 sync_partial_ok] - $initial_psyncs] 2
        assert_equal [expr [s 0 sync_full] - $initial_syncs] 0

        assert_equal [count_log_message 0 "Failover target rejected psync request"] 1
        assert_digests_match $node_0 $node_1 $node_2
    }
}
}
}

# Get a specific node by ID by parsing the CLUSTER NODES output
# of the instance Number 'instance_id'
proc get_node_by_id {instance_id node_id} {
    set nodes [get_cluster_nodes $instance_id]
    foreach n $nodes {
        if {[dict get $n id] eq $node_id} {return $n}
    }
    return {}
}

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

        set R0_nodeid [R 0 cluster myid]

        # R 0 is the first node, so we expect its epoch to be the smallest,
        # so bumpepoch must succeed and it's config epoch will be changed.
        set res [R 0 cluster bumpepoch]
        assert_match {BUMPED *} $res
        set R0_config_epoch [lindex $res 1]

        # Wait for the config epoch to propagate across the cluster.
        wait_for_condition 1000 10 {
            $R0_config_epoch == [dict get [get_node_by_id 1 $R0_nodeid] config_epoch] &&
            $R0_config_epoch == [dict get [get_node_by_id 2 $R0_nodeid] config_epoch]
        } else {
            fail "Other primaries does not update config epoch"
        }
        # Make sure that replica do not update config epoch.
        assert_not_equal $R0_config_epoch [dict get [get_node_by_id 3 $R0_nodeid] config_epoch]

        # Pause the R 0 and wait for the cluster to be down.
        pause_process [srv 0 pid]
        R 3 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_NONE
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
            $R0_config_epoch == [dict get [get_node_by_id 1 $R0_nodeid] config_epoch]
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

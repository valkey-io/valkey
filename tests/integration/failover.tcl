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
        verify_replica_online $node_0 0 50
        verify_replica_online $node_0 1 50
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

        set rd_blocking [valkey_deferring_client -2]

        $rd_blocking BRPOP FOO_LIST 0

        wait_for_condition 50 100 {
            [s -2 blocked_clients] == 1
        } else {
            fail "rd_blocking client should be blocked"
        }

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

        # rd_blocking client should still be blocked in failover-in-progress
        assert_equal [s -2 blocked_clients] 1

        resume_process $node_0_pid

        # Wait for failover to end
        wait_for_condition 50 100 {
            [s -2 master_failover_state] == "no-failover"
        } else {
            fail "Failover from node 2 to node 0 did not finish"
        }

        assert_error "UNBLOCKED force unblock from blocking operation, instance state changed (master -> replica?)" {$rd_blocking read}

        assert_equal [s -2 blocked_clients] 0

        $rd_blocking close

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

        # We send a BRPOP command and pause the target long enough
        # to send an LPUSH and another BRPOP command during the pause.
        # This push and the two pops (which are blocked for different
        # reasons, respectively) will not be interrupted by the aborted
        # failover.
        pause_process $node_1_pid
        $node_0 SET FOO BAR
        set rd_lpush [valkey_deferring_client]
        set rd_brpop_before [valkey_deferring_client]
        set rd_brpop_after [valkey_deferring_client]

        $rd_brpop_before BRPOP FOO_LIST 0
        wait_for_condition 50 100 {
            [s 0 blocked_clients] == 1
        } else {
            fail "rd_brpop_before should be blocked"
        }

        $node_0 failover to $node_1_host $node_1_port
        wait_for_condition 50 100 {
            [s 0 master_failover_state] == "waiting-for-sync"
        } else {
            fail "failover did not start in time"
        } debug {
            puts [s 0 master_failover_state]
            puts [$node_0 info replication]
            fail "failover did not start in time"
        }

        $rd_brpop_after BRPOP FOO_LIST 0
        $rd_lpush LPUSH FOO_LIST BAR1 BAR2

        wait_for_condition 100 100 {
            [s 0 blocked_clients] == 3
        } else {
            fail "All deferring clients should be blocked"
        }

        assert_equal "waiting-for-sync" [s 0 master_failover_state]

        resume_process $node_1_pid

        # Wait for failover to end
        wait_for_condition 50 100 {
            [s 0 master_failover_state] == "no-failover"
        } else {
            fail "Failover from node_0 to replica did not finish"
        }

        assert_equal "2" [$rd_lpush read]
        assert_equal "FOO_LIST BAR1" [$rd_brpop_before read]
        assert_equal "FOO_LIST BAR2" [$rd_brpop_after read]
        $rd_lpush close
        $rd_brpop_before close
        $rd_brpop_after close

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

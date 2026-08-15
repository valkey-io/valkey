# Primitive tests on cluster-enabled server with modules

source tests/support/cli.tcl

# cluster creation is complicated with TLS, and the current tests don't really need that coverage
tags {tls:skip external:skip cluster modules} {

set testmodule [file normalize tests/modules/cluster.so]
set modules [list loadmodule $testmodule]
start_cluster 3 0 [list config_lines $modules] {
    set node1 [srv 0 client]
    set node2 [srv -1 client]
    set node3 [srv -2 client]

    test "Cluster module send message API - VM_SendClusterMessage" {
        R 0 CONFIG RESETSTAT
        R 1 CONFIG RESETSTAT
        R 2 CONFIG RESETSTAT

        assert_equal OK [$node1 test.pingall]
        assert_equal 2 [CI 0 cluster_stats_messages_module_sent]
        wait_for_condition 50 100 {
            [CI 1 cluster_stats_messages_module_received] eq 1 &&
            [CI 2 cluster_stats_messages_module_received] eq 1 &&
            [CI 1 cluster_stats_module_bytes_received] > 0 &&
            [CI 2 cluster_stats_module_bytes_received] > 0
        } else {
            fail "node 2 or node 3 didn't receive cluster module message"
        }
        set sent_module_bytes [CI 0 cluster_stats_module_bytes_sent]
        set received_module_bytes [expr {[CI 1 cluster_stats_module_bytes_received] + [CI 2 cluster_stats_module_bytes_received]}]
        assert {$sent_module_bytes > 0}
        assert_equal $sent_module_bytes $received_module_bytes
        verify_log_message -1 "*DING (type 1) RECEIVED*Hey*" 0
        verify_log_message -2 "*DING (type 1) RECEIVED*Hey*" 0
    }

    test "Cluster module receive message API - VM_RegisterClusterMessageReceiver" {
        wait_for_condition 50 100 {
            [CI 0 cluster_stats_messages_module_received] eq 2 &&
            [CI 0 cluster_stats_module_bytes_received] > 0
        } else {
            fail "node 1 didn't receive DONG messages"
        }
        set received_module_bytes [CI 0 cluster_stats_module_bytes_received]
        set sent_module_bytes [expr {[CI 1 cluster_stats_module_bytes_sent] + [CI 2 cluster_stats_module_bytes_sent]}]
        assert {$received_module_bytes > 0}
        assert_equal $received_module_bytes $sent_module_bytes
        wait_for_condition 50 100 {
            [count_log_message 0 "* <cluster> DONG (type 2) RECEIVED*"] eq 2
        } else {
            fail "node 1 didn't log DONG message twice"
        }
    }
}

set testmodule_nokey [file normalize tests/modules/blockonbackground.so]
set testmodule_blockedclient [file normalize tests/modules/blockedclient.so]
set testmodule [file normalize tests/modules/blockonkeys.so]
set testmodule_auth [file normalize tests/modules/auth.so]

set modules [list loadmodule $testmodule loadmodule $testmodule_nokey loadmodule $testmodule_blockedclient loadmodule $testmodule_auth]
start_cluster 3 0 [list config_lines $modules] {

    set node1 [srv 0 client]
    set node2 [srv -1 client]
    set node3 [srv -2 client]
    set node3_pid [srv -2 pid]

    test "Run blocking command (blocked on key) on cluster node3" {
        # key9184688 is mapped to slot 10923 (first slot of node 3)
        set node3_rd [valkey_deferring_client -2]
        $node3_rd fsl.bpop key9184688 0
        $node3_rd flush
        wait_for_condition 50 100 {
            [s -2 blocked_clients] eq {1}
        } else {
            fail "Client executing blocking command (blocked on key) not blocked"
        }
    }

    test "Run blocking command (no keys) on cluster node2" {
        set node2_rd [valkey_deferring_client -1]
        $node2_rd block.block 0
        $node2_rd flush

        wait_for_condition 50 100 {
            [s -1 blocked_clients] eq {1}
        } else {
            fail "Client executing blocking command (no keys) not blocked"
        }
    }


    test "Perform a Resharding" {
        exec $::VALKEY_CLI_BIN --cluster-yes --cluster reshard 127.0.0.1:[srv -2 port] \
                           --cluster-to [$node1 cluster myid] \
                           --cluster-from [$node3 cluster myid] \
                           --cluster-slots 1
    }

    test "Verify command (no keys) is unaffected after resharding" {
        # verify there are blocked clients on node2
        assert_equal [s -1 blocked_clients]  {1}

        #release client 
        $node2 block.release 0
    }

    test "Verify command (blocked on key) got unblocked after resharding" {
        # this (read) will wait for the node3 to realize the new topology
        assert_error {*MOVED*} {$node3_rd read}

        # verify there are no blocked clients
        assert_equal [s 0 blocked_clients]  {0}
        assert_equal [s -1 blocked_clients]  {0}
        assert_equal [s -2 blocked_clients]  {0}
    }

    test "Wait for cluster to be stable" {
        wait_for_condition 1000 50 {
            [catch {exec $::VALKEY_CLI_BIN --cluster check 127.0.0.1:[srv 0 port]}] == 0 &&
            [catch {exec $::VALKEY_CLI_BIN --cluster check 127.0.0.1:[srv -1 port]}] == 0 &&
            [catch {exec $::VALKEY_CLI_BIN --cluster check 127.0.0.1:[srv -2 port]}] == 0 &&
            [CI 0 cluster_state] eq {ok} &&
            [CI 1 cluster_state] eq {ok} &&
            [CI 2 cluster_state] eq {ok}
        } else {
            fail "Cluster doesn't stabilize"
        }
    }

    test "Sanity test push cmd after resharding" {
        assert_error {*MOVED*} {$node3 fsl.push key9184688 1}

        set node1_rd [valkey_deferring_client 0]
        $node1_rd fsl.bpop key9184688 0
        $node1_rd flush

        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            puts "Client not blocked"
            puts "read from blocked client: [$node1_rd read]"
            fail "Client not blocked"
        }

        $node1 fsl.push key9184688 2
        assert_equal {2} [$node1_rd read]
    }

    $node1_rd close
    $node2_rd close
    $node3_rd close

    test "Run blocking command (blocked on key) again on cluster node1" {
        $node1 del key9184688
        # key9184688 is mapped to slot 10923 which has been moved to node1
        set node1_rd [valkey_deferring_client 0]
        $node1_rd fsl.bpop key9184688 0
        $node1_rd flush

        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Client executing blocking command (blocked on key) again not blocked"
        }
    }

    test "Run blocking command (no keys) again on cluster node2" {
        set node2_rd [valkey_deferring_client -1]

        $node2_rd block.block 0
        $node2_rd flush

        wait_for_condition 50 100 {
            [s -1 blocked_clients] eq {1}
        } else {
            fail "Client executing blocking command (no keys) again not blocked"
        }
    }

    test "Kill a cluster node and wait for fail state" {
        # kill node3 in cluster
        pause_process $node3_pid

        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {fail} &&
            [CI 1 cluster_state] eq {fail}
        } else {
            fail "Cluster doesn't fail"
        }
    }

    test "Verify command (blocked on key) got unblocked after cluster failure" {
        assert_error {*CLUSTERDOWN*} {$node1_rd read}
    }

    test "Verify command (with no keys) is not unblocked after cluster failure" {
        assert_no_match {*CLUSTERDOWN*} {$node2_rd read}
        # verify there are blocked clients
        assert_equal [s -1 blocked_clients]  {1}
    }

    test "Verify command RM_Call is rejected when cluster is down" {
        assert_error "ERR Can not execute a command 'set' while the cluster is down" {$node1 do_rm_call set x 1}
    }

    test "Verify Module Auth Succeeds when cluster is down" {
        r acl setuser foo >pwd on ~* &* +@all
        assert_error "*CLUSTERDOWN*" {r set x 1}
        # Non Blocking Module Auth
        assert_equal {OK} [r testmoduleone.rm_register_auth_cb]
        assert_equal {OK} [r AUTH foo allow]
        # Blocking Module Auth
        assert_equal {OK} [r testmoduleone.rm_register_blocking_auth_cb]
        assert_equal {OK} [r AUTH foo block_allow]
    }

    resume_process $node3_pid
    $node1_rd close
    $node2_rd close
}

set testmodule_keyspace_events [file normalize tests/modules/keyspace_events.so]
set testmodule_postnotifications "[file normalize tests/modules/postnotifications.so] with_key_events"
set modules [list loadmodule $testmodule_keyspace_events loadmodule $testmodule_postnotifications]
start_cluster 2 2 [list config_lines $modules] {

    set master1 [srv 0 client]
    set master2 [srv -1 client]
    set replica1 [srv -2 client]
    set replica2 [srv -3 client]

    test "Verify keys deletion and notification effects happened on cluster slots change are replicated inside multi exec" {
        $master2 set count_dels_{4oi} 1
        $master2 del count_dels_{4oi}
        assert_equal 1 [$master2 keyspace.get_dels]
        assert_equal 1 [$replica2 keyspace.get_dels]
        $master2 set count_dels_{4oi} 1

        set repl [attach_to_replication_stream_on_connection -3]

        $master1 cluster bumpepoch
        $master1 cluster setslot 16382 node [$master1 cluster myid]

        wait_for_cluster_propagation
        wait_for_condition 50 100 {
            [$master2 keyspace.get_dels] eq 2
        } else {
            fail "master did not delete the key"
        }
        wait_for_condition 50 100 {
            [$replica2 keyspace.get_dels] eq 2
        } else {
            fail "replica did not increase del counter"
        }

        # the {lpush before_deleted count_dels_{4oi}} is a post notification job registered when 'count_dels_{4oi}' was removed
        assert_replication_stream $repl {
            {multi}
            {unlink count_dels_{4oi}}
            {keyspace.incr_dels}
            {lpush before_deleted count_dels_{4oi}}
            {exec}
        }
        close_replication_stream $repl
    }
}

set testmodule [file normalize tests/modules/basics.so]
set modules [list loadmodule $testmodule]
start_cluster 3 0 [list config_lines $modules] {
    set node1 [srv 0 client]
    set node2 [srv -1 client]
    set node3 [srv -2 client]

    test "Verify RM_Call inside module load function on cluster mode" {
        assert_equal {PONG} [$node1 PING]
        assert_equal {PONG} [$node2 PING]
        assert_equal {PONG} [$node3 PING]
    }
}

set testmodule [file normalize tests/modules/cluster.so]
set modules [list loadmodule $testmodule]
start_cluster 3 0 [list config_lines $modules] {
    set node1 [srv 0 client]
    set node2 [srv -1 client]
    set node3 [srv -2 client]

    test "VM_CALL with cluster slots" {
        assert_equal [lsort [$node1 cluster slots]] [lsort [$node1 test.cluster_slots]]
        assert_equal [lsort [$node2 cluster slots]] [lsort [$node2 test.cluster_slots]]
        assert_equal [lsort [$node3 cluster slots]] [lsort [$node3 test.cluster_slots]]
    }

    test "VM_CALL with cluster shards" {
        assert_equal [lsort [$node1 cluster shards]] [lsort [$node1 test.cluster_shards]]
        assert_equal [lsort [$node2 cluster shards]] [lsort [$node2 test.cluster_shards]]
        assert_equal [lsort [$node3 cluster shards]] [lsort [$node3 test.cluster_shards]]
    }

    test "VM_CALL CLUSTER SLOTS from Module Timer" {
        assert_equal {OK} [$node1 test.start_cluster_timer]
        assert_equal {OK} [$node2 test.start_cluster_timer]
        assert_equal {OK} [$node3 test.start_cluster_timer]

        wait_for_condition 50 100 {
            [count_log_message 0 "* <cluster> Timer: CLUSTER SLOTS success*"] >= 1
        } else {
            fail "Timer did not execute CLUSTER SLOTS or server crashed"
        }
    }

    test "VM_RegisterClusterMessageReceiver - unregister head and re-register does not crash" {
        # Register the receivers on node1
        assert_equal OK [$node1 test.register_receiver]

        # Unregister it (the head of the list)
        assert_equal OK [$node1 test.unregister_receiver]

        # Re-register - on the buggy code this traverses freed memory and crashes
        assert_equal OK [$node1 test.register_receiver]

        # Send from node2 so node1 receives it via the re-registered receiver
        R 0 CONFIG RESETSTAT
        assert_equal OK [$node2 test.send_msg_uaf]

        wait_for_condition 50 100 {
            [CI 0 cluster_stats_messages_module_received] >= 2
        } else {
            fail "node1 didn't receive cluster module message after re-registration"
        }
        verify_log_message 0 "*DING (type 3) RECEIVED*TestUAF*" 0
        verify_log_message 0 "*DING (type 255) RECEIVED*TestMAX*" 0
    }

    test "VM_RegisterClusterMessageReceiver - dangling callback after MODULE UNLOAD" {
        set loglines [count_log_lines 0]

        # Register the receivers on node1
        assert_equal OK [$node1 test.register_receiver]

        # Unload the module on node1
        assert_equal OK [$node1 MODULE UNLOAD cluster]

        # Another node sends a packet; node1 receives it and, on the buggy code,
        # would invoke the dangling callback and crash. After the fix the entry
        # is gone, so the packet is simply ignored.
        R 0 CONFIG RESETSTAT
        assert_equal OK [$node2 test.send_msg_uaf]

        # Verify node1 is still alive (the receiving node must not have crashed).
        wait_for_condition 50 100 {
            [CI 0 cluster_stats_messages_module_received] >= 2
        } else {
            fail "node1 didn't receive cluster module message"
        }
        verify_no_log_message 0 "*DING (type 3) RECEIVED*TestUAF*" $loglines
        verify_no_log_message 0 "*DING (type 255) RECEIVED*TestMAX*" $loglines
        assert_equal PONG [$node1 PING]
    }
}

start_cluster 2 0 {overrides {cluster-node-timeout 1000}} {
    test "MODULE LOAD is blocked during atomic slot migration" {
        set testmodule [file normalize tests/modules/basics.so]
        set node0_id [R 0 CLUSTER MYID]
        set node1_id [R 1 CLUSTER MYID]

        # Verify module can be loaded normally before migration
        assert_equal {OK} [R 0 MODULE LOAD $testmodule]
        assert_equal {OK} [R 0 MODULE UNLOAD test]

        # Prevent migration from completing so we can test during migration
        R 0 DEBUG SLOTMIGRATION PREVENT-PAUSE 1
        R 1 DEBUG SLOTMIGRATION PREVENT-PAUSE 1

        # Start atomic slot migration from node 0 to node 1
        assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 100 100 NODE $node1_id]

        # Wait for migration to be in progress
        wait_for_condition 100 100 {
            [llength [R 0 CLUSTER GETSLOTMIGRATIONS]] > 0
        } else {
            fail "Migration did not start"
        }

        # Attempt to load module during migration should fail on exporting node
        catch {R 0 MODULE LOAD $testmodule} err
        assert_match "*Error loading module: cannot load module during slot migration*" $err

        # Attempt to load module during migration should fail on importing node
        catch {R 1 MODULE LOAD $testmodule} err
        assert_match "*Error loading module: cannot load module during slot migration*" $err

        # Cancel migration
        R 0 CLUSTER CANCELSLOTMIGRATIONS

        # Wait for migration to be cancelled
        wait_for_condition 100 100 {
            [llength [R 1 CLUSTER GETSLOTMIGRATIONS]] == 0 ||
            [dict get [lindex [R 1 CLUSTER GETSLOTMIGRATIONS] 0] state] eq "failed"
        } else {
            fail "Migration did not cancel"
        }

        # Re-enable pausing
        R 0 DEBUG SLOTMIGRATION PREVENT-PAUSE 0
        R 1 DEBUG SLOTMIGRATION PREVENT-PAUSE 0

        # Verify module can be loaded after migration completes
        assert_equal {OK} [R 0 MODULE LOAD $testmodule]
        assert_equal {OK} [R 0 MODULE UNLOAD test]
    }

    test "Module VM_OpenKey cross-slot undeclared key access" {
        set clustermodule [file normalize tests/modules/cluster.so]
        assert_equal {OK} [R 0 MODULE LOAD $clustermodule]

        # Find keys with distinct hash slots that all belong to node 0 (slots 0-5460):
        # {bar} = 5061, {baz} = 4813, {alpha} = 865, {k2} = 449, {k3} = 4576
        set key_declared "{bar}declared"
        set key_target_read "{baz}target_read"
        set key_target_write "{alpha}target_write"
        set key_target_expire "{k2}target_expire"
        set key_target_wrongtype "{k3}target_wrongtype"

        set slot_declared [R 0 CLUSTER KEYSLOT $key_declared]
        set slot_target_read [R 0 CLUSTER KEYSLOT $key_target_read]
        set slot_target_write [R 0 CLUSTER KEYSLOT $key_target_write]
        set slot_target_expire [R 0 CLUSTER KEYSLOT $key_target_expire]
        set slot_target_wrongtype [R 0 CLUSTER KEYSLOT $key_target_wrongtype]

        # Explicitly verify all hash slots are genuinely distinct
        assert {$slot_declared != $slot_target_read}
        assert {$slot_declared != $slot_target_write}
        assert {$slot_declared != $slot_target_expire}
        assert {$slot_declared != $slot_target_wrongtype}
        assert {$slot_target_read != $slot_target_write}

        # Derive actual local slot range owned by node 0 dynamically
        set node0_id [R 0 CLUSTER MYID]
        set local_start -1
        set local_end -1
        foreach slot_range [R 0 cluster slots] {
            set primary_info [lindex $slot_range 2]
            set primary_id [lindex $primary_info 2]
            if {$primary_id eq $node0_id} {
                set local_start [lindex $slot_range 0]
                set local_end [lindex $slot_range 1]
                break
            }
        }
        assert {$local_start >= 0 && $local_end >= $local_start}

        # Explicitly verify all hash slots belong to node 0's actual slot range
        assert {$slot_declared >= $local_start && $slot_declared <= $local_end}
        assert {$slot_target_read >= $local_start && $slot_target_read <= $local_end}
        assert {$slot_target_write >= $local_start && $slot_target_write <= $local_end}
        assert {$slot_target_expire >= $local_start && $slot_target_expire <= $local_end}
        assert {$slot_target_wrongtype >= $local_start && $slot_target_wrongtype <= $local_end}

        # 1. READ: Declared key in slot 5061, undeclared target in slot 4813
        R 0 SET $key_target_read "read_val"
        assert_equal "read_val" [R 0 test.openkey_cross_slot $key_declared $key_target_read read]

        # 2. WRITE: Command declared key in slot 5061, writes undeclared key in slot 865
        assert_equal "OK" [R 0 test.openkey_cross_slot $key_declared $key_target_write write "written_val"]
        assert_equal "written_val" [R 0 GET $key_target_write]

        # 3. EXPIRY: Command declared key in slot 5061, sets TTL on undeclared key in slot 449
        R 0 SET $key_target_expire "expiring_val"
        assert_equal "OK" [R 0 test.openkey_cross_slot $key_declared $key_target_expire expire 100000]
        set ttl [R 0 TTL $key_target_expire]
        assert {$ttl > 0 && $ttl <= 100}

        # 4. EXPIRY ERROR HANDLING: missing key and invalid TTL (< -1)
        assert_error "*ERR SetExpire failed*" {R 0 test.openkey_cross_slot $key_declared "{k2}nonexistent" expire 100000}
        assert_error "*ERR SetExpire failed*" {R 0 test.openkey_cross_slot $key_declared $key_target_expire expire -10}

        # 5. EXPIRY CANCELLATION: VALKEYMODULE_NO_EXPIRE (-1) cancels expiration (PERSIST)
        assert_equal "OK" [R 0 test.openkey_cross_slot $key_declared $key_target_expire expire -1]
        assert_equal -1 [R 0 TTL $key_target_expire]

        # 6. WRONG-TYPE READ: string DMA on a list key returns WRONGTYPE
        R 0 LPUSH $key_target_wrongtype "list_elem"
        assert_error "*WRONGTYPE*" {R 0 test.openkey_cross_slot $key_declared $key_target_wrongtype read}

        assert_equal {OK} [R 0 MODULE UNLOAD cluster]
    }

}

} ;# end tag

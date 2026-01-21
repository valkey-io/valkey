# Allocate slot 0 to the last primary and evenly distribute the remaining
# slots to the remaining primaries.
proc my_slot_allocation {masters replicas} {
    set avg [expr double(16384) / [expr $masters-1]]
    set slot_start 1
    for {set j 0} {$j < $masters-1} {incr j} {
        set slot_end [expr int(ceil(($j + 1) * $avg) - 1)]
        R $j cluster addslotsrange $slot_start $slot_end
        set slot_start [expr $slot_end + 1]
    }
    R [expr $masters-1] cluster addslots 0
}

proc get_my_primary_peer {srv_idx} {
    set role_response [R $srv_idx role]
    set primary_ip [lindex $role_response 1]
    set primary_port [lindex $role_response 2]
    set primary_peer "$primary_ip:$primary_port"
    return $primary_peer
}

proc test_migrated_replica {type} {
    test "Migrated replica reports zero repl offset and rank, and fails to win election - $type" {
        # Write some data to primary 0, slot 1, make a small repl_offset.
        for {set i 0} {$i < 1024} {incr i} {
            R 0 incr key_991803
        }
        assert_equal {1024} [R 0 get key_991803]

        # Write some data to primary 3, slot 0, make a big repl_offset.
        for {set i 0} {$i < 10240} {incr i} {
            R 3 incr key_977613
        }
        assert_equal {10240} [R 3 get key_977613]

        # 10s, make sure primary 0 will hang in the save.
        R 0 config set rdb-key-save-delay 100000000

        # Move the slot 0 from primary 3 to primary 0
        set addr "[srv 0 host]:[srv 0 port]"
        set myid [R 3 CLUSTER MYID]
        set code [catch {
            exec $::VALKEY_CLI_BIN {*}[valkeycli_tls_config "./tests"] --cluster rebalance $addr --cluster-weight $myid=0
        } result]
        if {$code != 0} {
            fail "valkey-cli --cluster rebalance returns non-zero exit code, output below:\n$result"
        }

        # Validate that shard 3's primary and replica can convert to replicas after
        # they lose the last slot.
        R 3 config set cluster-replica-validity-factor 0
        R 7 config set cluster-replica-validity-factor 0
        R 3 config set cluster-allow-replica-migration yes
        R 7 config set cluster-allow-replica-migration yes

        if {$type == "shutdown"} {
            # Shutdown primary 0.
            catch {R 0 shutdown nosave}
        } elseif {$type == "sigstop"} {
            # Pause primary 0.
            set primary0_pid [s 0 process_id]
            pause_process $primary0_pid
        }

        # Wait for the replica to become a primary, and make sure
        # the other primary become a replica.
        wait_for_condition 1000 50 {
            [s -4 role] eq {master} &&
            [s -3 role] eq {slave} &&
            [s -7 role] eq {slave}
        } else {
            puts "s -4 role: [s -4 role]"
            puts "s -3 role: [s -3 role]"
            puts "s -7 role: [s -7 role]"
            fail "Failover does not happened"
        }

        # The node may not be able to initiate an election in time due to
        # problems with cluster communication. If an election is initiated,
        # we make sure the offset of server 3 / 7 is 0.
        if {[count_log_message -3 "Start of election"] != 0} {
            verify_log_message -3 "*Start of election*offset 0*" 0
        }
        if {[count_log_message -7 "Start of election"] != 0} {
            verify_log_message -7 "*Start of election*offset 0*" 0
        }

        # Make sure the right replica gets the higher rank.
        verify_log_message -4 "*Start of election*rank #0*" 0

        # Wait for the cluster to be ok.
        wait_for_condition 1000 50 {
            [R 3 cluster slots] eq [R 4 cluster slots] &&
            [R 4 cluster slots] eq [R 7 cluster slots] &&
            [CI 3 cluster_state] eq "ok" &&
            [CI 4 cluster_state] eq "ok" &&
            [CI 7 cluster_state] eq "ok"
        } else {
            puts "R 3: [R 3 cluster info]"
            puts "R 4: [R 4 cluster info]"
            puts "R 7: [R 7 cluster info]"
            fail "Cluster is down"
        }

        # Make sure the key exists and is consistent.
        R 3 readonly
        R 7 readonly
        wait_for_condition 1000 50 {
            [R 3 get key_991803] == 1024 && [R 3 get key_977613] == 10240 &&
            [R 4 get key_991803] == 1024 && [R 4 get key_977613] == 10240 &&
            [R 7 get key_991803] == 1024 && [R 7 get key_977613] == 10240
        } else {
            puts "R 3: [R 3 keys *]"
            puts "R 4: [R 4 keys *]"
            puts "R 7: [R 7 keys *]"
            fail "Key not consistent"
        }

        if {$type == "sigstop"} {
            resume_process $primary0_pid

            # Wait for the old primary to go online and become a replica.
            wait_for_condition 1000 50 {
                [s 0 role] eq {slave}
            } else {
                fail "The old primary was not converted into replica"
            }
        }
    }
} ;# proc

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999}} {
    test_migrated_replica "shutdown"
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999}} {
    test_migrated_replica "sigstop"
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

proc test_nonempty_replica {type} {
    test "New non-empty replica reports zero repl offset and rank, and fails to win election - $type" {
        # Write some data to primary 0, slot 1, make a small repl_offset.
        for {set i 0} {$i < 1024} {incr i} {
            R 0 incr key_991803
        }
        assert_equal {1024} [R 0 get key_991803]

        # Write some data to primary 3, slot 0, make a big repl_offset.
        for {set i 0} {$i < 10240} {incr i} {
            R 3 incr key_977613
        }
        assert_equal {10240} [R 3 get key_977613]

        # 10s, make sure primary 0 will hang in the save.
        R 0 config set rdb-key-save-delay 100000000

        # Make server 7 a replica of server 0.
        R 7 config set cluster-replica-validity-factor 0
        R 7 config set cluster-allow-replica-migration yes
        R 7 cluster replicate [R 0 cluster myid]

        if {$type == "shutdown"} {
            # Shutdown primary 0.
            catch {R 0 shutdown nosave}
        } elseif {$type == "sigstop"} {
            # Pause primary 0.
            set primary0_pid [s 0 process_id]
            pause_process $primary0_pid
        }

        # Wait for the replica to become a primary.
        wait_for_condition 1000 50 {
            [s -4 role] eq {master} &&
            [s -7 role] eq {slave}
        } else {
            puts "s -4 role: [s -4 role]"
            puts "s -7 role: [s -7 role]"
            fail "Failover does not happened"
        }

        verify_log_message -4 "*Start of election*rank #0*" 0

        # The node may not be able to initiate an election in time due to
        # problems with cluster communication. If an election is initiated,
        # we make sure server 7 gets the lower rank and it's offset is 0.
        if {[count_log_message -7 "Start of election"] != 0} {
            verify_log_message -7 "*Start of election*offset 0*" 0
        }

        # Wait for the cluster to be ok.
        wait_for_condition 1000 50 {
            [R 4 cluster slots] eq [R 7 cluster slots] &&
            [CI 4 cluster_state] eq "ok" &&
            [CI 7 cluster_state] eq "ok"
        } else {
            puts "R 4: [R 4 cluster info]"
            puts "R 7: [R 7 cluster info]"
            fail "Cluster is down"
        }

        # Make sure the key exists and is consistent.
        R 7 readonly
        wait_for_condition 1000 50 {
            [R 4 get key_991803] == 1024 &&
            [R 7 get key_991803] == 1024
        } else {
            puts "R 4: [R 4 get key_991803]"
            puts "R 7: [R 7 get key_991803]"
            fail "Key not consistent"
        }

        if {$type == "sigstop"} {
            resume_process $primary0_pid

            # Wait for the old primary to go online and become a replica.
            wait_for_condition 1000 50 {
                [s 0 role] eq {slave}
            } else {
                fail "The old primary was not converted into replica"
            }
        }
    }
} ;# proc

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999}} {
    test_nonempty_replica "shutdown"
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999}} {
    test_nonempty_replica "sigstop"
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

proc test_sub_replica {type} {
    test "Sub-replica reports zero repl offset and rank, and fails to win election - $type" {
        # Write some data to primary 0, slot 1, make a small repl_offset.
        for {set i 0} {$i < 1024} {incr i} {
            R 0 incr key_991803
        }
        assert_equal {1024} [R 0 get key_991803]

        # Write some data to primary 3, slot 0, make a big repl_offset.
        for {set i 0} {$i < 10240} {incr i} {
            R 3 incr key_977613
        }
        assert_equal {10240} [R 3 get key_977613]

        R 3 config set cluster-replica-validity-factor 0
        R 7 config set cluster-replica-validity-factor 0
        R 3 config set cluster-allow-replica-migration yes
        R 7 config set cluster-allow-replica-migration no

        # 10s, make sure primary 0 will hang in the save.
        R 0 config set rdb-key-save-delay 100000000

        # Move slot 0 from primary 3 to primary 0.
        set addr "[srv 0 host]:[srv 0 port]"
        set myid [R 3 CLUSTER MYID]
        set code [catch {
            exec $::VALKEY_CLI_BIN {*}[valkeycli_tls_config "./tests"] --cluster rebalance $addr --cluster-weight $myid=0
        } result]
        if {$code != 0} {
            fail "valkey-cli --cluster rebalance returns non-zero exit code, output below:\n$result"
        }

        # Make sure server 3 and server 7 becomes a replica of primary 0.
        wait_for_condition 1000 50 {
            [get_my_primary_peer 3] eq $addr &&
            [get_my_primary_peer 7] eq $addr
        } else {
            puts "R 3 role: [R 3 role]"
            puts "R 7 role: [R 7 role]"
            fail "Server 3 and 7 role response has not changed"
        }

        # Make sure server 7 got a sub-replica log.
        verify_log_message -7 "*I'm a sub-replica!*" 0

        if {$type == "shutdown"} {
            # Shutdown primary 0.
            catch {R 0 shutdown nosave}
        } elseif {$type == "sigstop"} {
            # Pause primary 0.
            set primary0_pid [s 0 process_id]
            pause_process $primary0_pid
        }

        # Wait for the replica to become a primary, and make sure
        # the other primary become a replica.
        wait_for_condition 1000 50 {
            [s -4 role] eq {master} &&
            [s -3 role] eq {slave} &&
            [s -7 role] eq {slave}
        } else {
            puts "s -4 role: [s -4 role]"
            puts "s -3 role: [s -3 role]"
            puts "s -7 role: [s -7 role]"
            fail "Failover does not happened"
        }

        # The node may not be able to initiate an election in time due to
        # problems with cluster communication. If an election is initiated,
        # we make sure the offset of server 3 / 7 is 0.
        if {[count_log_message -3 "Start of election"] != 0} {
            verify_log_message -3 "*Start of election*offset 0*" 0
        }
        if {[count_log_message -7 "Start of election"] != 0} {
            verify_log_message -7 "*Start of election*offset 0*" 0
        }

        # Wait for the cluster to be ok.
        wait_for_condition 1000 50 {
            [R 3 cluster slots] eq [R 4 cluster slots] &&
            [R 4 cluster slots] eq [R 7 cluster slots] &&
            [CI 3 cluster_state] eq "ok" &&
            [CI 4 cluster_state] eq "ok" &&
            [CI 7 cluster_state] eq "ok"
        } else {
            puts "R 3: [R 3 cluster info]"
            puts "R 4: [R 4 cluster info]"
            puts "R 7: [R 7 cluster info]"
            fail "Cluster is down"
        }

        # Make sure the key exists and is consistent.
        R 3 readonly
        R 7 readonly
        wait_for_condition 1000 50 {
            [R 3 get key_991803] == 1024 && [R 3 get key_977613] == 10240 &&
            [R 4 get key_991803] == 1024 && [R 4 get key_977613] == 10240 &&
            [R 7 get key_991803] == 1024 && [R 7 get key_977613] == 10240
        } else {
            puts "R 3: [R 3 keys *]"
            puts "R 4: [R 4 keys *]"
            puts "R 7: [R 7 keys *]"
            fail "Key not consistent"
        }

        if {$type == "sigstop"} {
            resume_process $primary0_pid

            # Wait for the old primary to go online and become a replica.
            wait_for_condition 1000 50 {
                [s 0 role] eq {slave}
            } else {
                fail "The old primary was not converted into replica"
            }
        }
    }
}

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999}} {
    test_sub_replica "shutdown"
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999}} {
    test_sub_replica "sigstop"
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

# This test demonstrates a race condition where a replica of a failed primary
# does not correctly follow the new primary in the same shard.
#
# The race condition scenario:
# 1. Node 7 is a replica of node 0 (after slot migration)
# 2. Node 0 fails, node 4 becomes the new primary via failover
# 3. Node 7 learns that node 0 is FAIL, but hasn't yet received the slot
#    configuration update from node 4
# 4. Node 7 should follow node 4 (the new primary in the same shard), but
#    instead it remains a replica of the failed node 0
#
# CURRENT BEHAVIOR (BUG):
# Node 7 stays as replica of failed node 0 instead of following node 4.
# This can lead to node 7 starting an election and becoming an empty primary,
# creating duplicate primaries in the same shard.
#
# EXPECTED BEHAVIOR (after fix):
# Node 7 should detect that its primary (node 0) is failed and that there's
# a new primary (node 4) in the same shard, and reconfigure to follow node 4.
proc test_replica_of_failed_primary_behavior {} {
    test "Replica of failed primary - current behavior" {
        R 3 config set cluster-replica-validity-factor 0
        R 7 config set cluster-replica-validity-factor 0
        R 3 config set cluster-allow-replica-migration yes
        # In the test_sub_replica test case above, the config cluster-allow-replica-migration
        # is turned off and thus R7 could become sub-replica. Here, we show that even with this
        # config turned on, R7 could also become sub-replica.
        R 7 config set cluster-allow-replica-migration yes

        # Write some data to primary 0, slot 1, make a small repl_offset.
        for {set i 0} {$i < 1024} {incr i} {
            R 0 incr key_991803
        }
        assert_equal {1024} [R 0 get key_991803]

        # Write some data to primary 3, slot 0, make a big repl_offset.
        for {set i 0} {$i < 10240} {incr i} {
            R 3 incr key_977613
        }
        assert_equal {10240} [R 3 get key_977613]

        # 10s delay per key, make sure primary 0 will hang in the RDB save.
        R 0 config set rdb-key-save-delay 100000000

        # Move slot 0 from primary 3 to primary 0.
        set addr "[srv 0 host]:[srv 0 port]"
        set src_node_id [R 3 CLUSTER MYID]
        set code [catch {
            exec src/valkey-cli {*}[valkeycli_tls_config "./tests"] --cluster rebalance $addr --cluster-weight $src_node_id=0
        } result]
        if {$code != 0} {
            fail "valkey-cli --cluster rebalance returns non-zero exit code, output below:\n$result"
        }

        # Make sure server 3 and server 7 become replicas of primary 0.
        set R0_id [R 0 CLUSTER MYID]
        wait_for_log_messages -3 [list "*Configuration change detected. Reconfiguring myself as a replica of node $R0_id*"] 0 1000 10
        wait_for_log_messages -7 [list "*Lost my last slot during slot migration. Reconfiguring myself as a replica of $R0_id*"] 0 1000 10

        # Stop primary 0 to start a failover.
        set primary0_pid [s 0 process_id]
        pause_process $primary0_pid

        # Wait for the replica 4 to become a primary.
        set R4_id [R 4 CLUSTER MYID]
        wait_for_log_messages -4 {"*Failover election won: I'm the new primary*"} 0 1000 10
        wait_for_log_messages -3 [list "*Configuration change detected. Reconfiguring myself as a replica of node $R4_id*"] 0 1000 10

        # Wait for node 7 to receive the FAIL message about node 0.
        wait_for_log_messages -7 [list "*FAIL message received from * about $R0_id*"] 0 2000 50

        # Isolate node 7 immediately after it learns node 0 is FAIL but before
        # it processes any slot configuration updates from node 4.
        R 7 DEBUG DROP-CLUSTER-PACKET-FILTER -2

        # Small delay to ensure isolation is in effect.
        after 100

        # Re-enable packet reception on node 7.
        R 7 DEBUG DROP-CLUSTER-PACKET-FILTER -1

        # Wait for node 7 to become a primary (the buggy behavior).
        wait_for_condition 1000 50 {
            [s -7 role] eq {master}
        } else {
            puts "R 7 role: [R 7 role]"
            fail "Node 7 did not become a primary (unexpected)"
        }
        # At this point, we have two primaries in the same shard:
        # - Node 4: the legitimate new primary (won the failover election)
        # - Node 7: an empty primary (buggy behavior)
    }
}

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999}} {
    test_replica_of_failed_primary_behavior
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

proc test_cluster_setslot {type} {
    test "valkey-cli make source node ignores NOREPLICAS error when doing the last CLUSTER SETSLOT - $type" {
        R 3 config set cluster-allow-replica-migration no
        R 7 config set cluster-allow-replica-migration yes

        if {$type == "setslot"} {
            # Make R 7 drop the PING message so that we have a higher
            # chance to trigger the migration from CLUSTER SETSLOT.
            R 7 DEBUG DROP-CLUSTER-PACKET-FILTER 1
        }

        # Move slot 0 from primary 3 to primary 0.
        set addr "[srv 0 host]:[srv 0 port]"
        set myid [R 3 CLUSTER MYID]
        set code [catch {
            exec $::VALKEY_CLI_BIN {*}[valkeycli_tls_config "./tests"] --cluster rebalance $addr --cluster-weight $myid=0
        } result]
        if {$code != 0} {
            fail "valkey-cli --cluster rebalance returns non-zero exit code, output below:\n$result"
        }

        # Wait for R 3 to report that it is an empty primary (cluster-allow-replica-migration no)
        wait_for_log_messages -3 {"*I am now an empty primary*"} 0 1000 50

        if {$type == "setslot"} {
            R 7 DEBUG DROP-CLUSTER-PACKET-FILTER -1
        }

        # Make sure server 3 lost its replica (server 7) and server 7 becomes a replica of primary 0.
        wait_for_condition 1000 50 {
            [s -3 role] eq {master} &&
            [s -3 connected_slaves] eq 0 &&
            [s -7 role] eq {slave} &&
            [get_my_primary_peer 7] eq $addr
        } else {
            puts "R 3 role: [R 3 role]"
            puts "R 7 role: [R 7 role]"
            fail "Server 3 and 7 role response has not changed"
        }
    }
}

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999}} {
    test_cluster_setslot "gossip"
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999}} {
    test_cluster_setslot "setslot"
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

start_cluster 3 0 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999}} {
    test "Empty primary will check and delete the dirty slots" {
        R 2 config set cluster-allow-replica-migration no

        # Write a key to slot 0.
        R 2 incr key_977613

        # Move slot 0 from primary 2 to primary 0.
        R 0 cluster bumpepoch
        R 0 cluster setslot 0 node [R 0 cluster myid]

        # Wait for R 2 to report that it is an empty primary (cluster-allow-replica-migration no)
        wait_for_log_messages -2 {"*I am now an empty primary*"} 0 1000 50

        # Make sure primary 0 will delete the dirty slots.
        verify_log_message -2 "*Deleting keys in dirty slot 0*" 0
        assert_equal [R 2 dbsize] 0
    }
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

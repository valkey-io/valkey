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
        # Write a key to primary 0, slot 1, make a small repl_offset.
        set small_value [string repeat "x" 1024]
        R 0 set key_991803 $small_value
        assert_equal $small_value [R 0 get key_991803]

        # Write a key to primary 3, slot 0, make a big repl_offset.
        set large_value [string repeat "y" 10240]
        R 3 set key_977613 $large_value
        assert_equal $large_value [R 3 get key_977613]

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
            [R 3 get key_991803] eq $small_value && [R 3 get key_977613] eq $large_value &&
            [R 4 get key_991803] eq $small_value && [R 4 get key_977613] eq $large_value &&
            [R 7 get key_991803] eq $small_value && [R 7 get key_977613] eq $large_value
        } else {
            catch {puts "R 3: [R 3 keys *]"}
            catch {puts "R 4: [R 4 keys *]"}
            catch {puts "R 7: [R 7 keys *]"}
            fail "Key not consistent"
        }

        if {$type == "sigstop"} {
            assert_equal 0 [count_log_message -7 "I'm a sub-replica"]

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

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_migrated_replica "shutdown"
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_migrated_replica "sigstop"
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

proc test_nonempty_replica {type} {
    test "New non-empty replica reports zero repl offset and rank, and fails to win election - $type" {
        # Write a key to primary 0, slot 1, make a small repl_offset.
        set small_value [string repeat "x" 1024]
        R 0 set key_991803 $small_value
        assert_equal $small_value [R 0 get key_991803]

        # Write a key to primary 3, slot 0, make a big repl_offset.
        set large_value [string repeat "y" 10240]
        R 3 set key_977613 $large_value
        assert_equal $large_value [R 3 get key_977613]

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
            [R 4 get key_991803] eq $small_value &&
            [R 7 get key_991803] eq $small_value
        } else {
            catch {puts "R 4: [R 4 get key_991803]"}
            catch {puts "R 7: [R 7 get key_991803]"}
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

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_nonempty_replica "shutdown"
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_nonempty_replica "sigstop"
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

proc test_sub_replica {type} {
    test "Sub-replica reports zero repl offset and rank, and fails to win election - $type" {
        # Write a key to primary 0, slot 1, make a small repl_offset.
        set small_value [string repeat "x" 1024]
        R 0 set key_991803 $small_value
        assert_equal $small_value [R 0 get key_991803]

        # Write a key to primary 3, slot 0, make a big repl_offset.
        set large_value [string repeat "y" 10240]
        R 3 set key_977613 $large_value
        assert_equal $large_value [R 3 get key_977613]

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
            [R 3 get key_991803] eq $small_value && [R 3 get key_977613] eq $large_value &&
            [R 4 get key_991803] eq $small_value && [R 4 get key_977613] eq $large_value &&
            [R 7 get key_991803] eq $small_value && [R 7 get key_977613] eq $large_value
        } else {
            catch {puts "R 3: [R 3 keys *]"}
            catch {puts "R 4: [R 4 keys *]"}
            catch {puts "R 7: [R 7 keys *]"}
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

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_sub_replica "shutdown"
} my_slot_allocation cluster_allocate_replicas ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_sub_replica "sigstop"
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

# Replica migration test.
# Check that orphaned primaries are joined by replicas of primaries having
# multiple replicas attached, according to the migration barrier settings.
start_cluster 5 10 {tags {external:skip cluster} overrides {shutdown-timeout 0}} {
    # Killing all the replicas of primary #0 and #1
    pause_process [srv -5 pid]
    pause_process [srv -10 pid]
    pause_process [srv -6 pid]
    pause_process [srv -11 pid]

    # R0 and R1 will trigger replica migration.
    wait_for_condition 1000 50 {
        [expr {[count_log_message -7 "Migrating to orphaned primary"] +
              [count_log_message -8 "Migrating to orphaned primary"] +
              [count_log_message -9 "Migrating to orphaned primary"] +
              [count_log_message -12 "Migrating to orphaned primary"] +
              [count_log_message -13 "Migrating to orphaned primary"] +
              [count_log_message -14 "Migrating to orphaned primary"]}] == 2
    } else {
        fail "Replicas did not trigger replica migration "
    }

    # Primary should have at least one replica"
    for {set id 0} {$id < 5} {incr id} {
        wait_for_condition 1000 50 {
            [llength [lindex [R $id role] 2]] >= 1
        } else {
            fail "Primary #$id has no replicas"
        }
    }

    resume_process [srv -5 pid]
    resume_process [srv -10 pid]
    resume_process [srv -6 pid]
    resume_process [srv -11 pid]
} ;# start_cluster

# Now test the migration to a primary which used to be a replica, after
# a failover.
start_cluster 5 10 {tags {external:skip cluster} overrides {shutdown-timeout 0}} {
    test "Primary #12 should get at least one migrated replica" {
        # Kill replica #7 of primary #2. Only replica left is #12 now"
        pause_process [srv -7 pid]

        # Killing primary node #2, #12 should failover
        set current_epoch [CI 1 cluster_current_epoch]
        pause_process [srv -2 pid]
        wait_for_condition 1000 50 {
            [CI 1 cluster_current_epoch] > $current_epoch
        } else {
            fail "No failover detected"
        }

        wait_for_cluster_state ok
        cluster_write_test [srv -1 port]
        assert {[s -12 role] eq {master}}

        # The remaining instance is now without replicas. Some other replica
        # should migrate to it.
        wait_for_condition 1000 50 {
            [llength [lindex [R 12 role] 2]] >= 1
        } else {
            fail "Primary #12 has no replicas"
        }

        resume_process [srv -7 pid]
        resume_process [srv -2 pid]
    }
} ;# start_cluster

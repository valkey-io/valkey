# A primary that comes back with a stale configuration after a failover must
# rejoin its shard as a replica of the promoted replica, even if the promoted
# replica gave some of the slots it inherited to another shard in the meantime.
#
# Shard A = R0 + R3 (+ R6), shard B = R1 + R4 (+ R7), shard C = R2 + R5 (+ R8).
#
# This needs cluster-allow-replica-migration to be disabled: otherwise the stale
# primary becomes a replica of the node that took its last slot.

# Returns the id of the owner of 'slot' according to the node 'n'.
proc stale_primary_slot_owner {n slot} {
    foreach range [R $n cluster slots] {
        if {$slot >= [lindex $range 0] && $slot <= [lindex $range 1]} {
            return [lindex [lindex $range 2] 2]
        }
    }
    return {}
}

# Returns 1 if the node 'n' knows the node 'node_id' as a primary.
proc stale_primary_sees_primary {n node_id} {
    set node [cluster_get_node_by_id $n $node_id]
    expr {$node ne {} && [cluster_has_flag $node master]}
}

# Shuts down the nodes in 'down', the last of which must be R0, the primary of
# shard A, and waits for R3 to take over. Then makes R3 migrate the highest slot
# of shard A to R1, in shard B. 'alive' lists the nodes that remain up.
proc stale_primary_failover_and_migrate {down alive} {
    set q_id [R 1 cluster myid]
    set r_id [R 3 cluster myid]
    set slot 5461
    assert_equal [stale_primary_slot_owner 0 $slot] [R 0 cluster myid]
    assert_equal [stale_primary_slot_owner 0 [expr {$slot + 1}]] $q_id

    foreach n $down {catch {R $n shutdown nosave}}
    wait_for_condition 1000 50 {
        [s -3 role] eq {master}
    } else {
        fail "R3 did not take over"
    }
    foreach n $alive {
        wait_for_condition 1000 50 {
            [stale_primary_slot_owner $n 0] eq $r_id
        } else {
            fail "R$n did not learn about the failover"
        }
    }

    assert_equal OK [R 1 cluster setslot $slot importing $r_id]
    assert_equal OK [R 3 cluster setslot $slot migrating $q_id]
    assert_equal OK [R 1 cluster setslot $slot node $q_id]
    assert_equal OK [R 3 cluster setslot $slot node $q_id]
    foreach n $alive {
        wait_for_condition 1000 50 {
            [stale_primary_slot_owner $n $slot] eq $q_id
        } else {
            fail "R$n did not learn about the slot migration"
        }
    }
}

start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no cluster-require-full-coverage no}} {
    test "Stale primary rejoins its shard as a replica after a failover and a slot migration" {
        set p_id [R 0 cluster myid]
        set q_id [R 1 cluster myid]
        set r_id [R 3 cluster myid]
        wait_for_condition 1000 50 {
            [s -3 master_link_status] eq {up}
        } else {
            fail "R3 did not sync with R0"
        }
        # Some keys in a slot that stays in shard A.
        assert_lessthan [R 0 cluster keyslot "{b}"] 5461
        for {set j 0} {$j < 100} {incr j} {R 0 set "{b}key:$j" $j}
        wait_for_ofs_sync [srv 0 client] [srv -3 client]

        stale_primary_failover_and_migrate {0} {1 2 3 4 5}

        # R0 restarts with its stale configuration while shard B is paused. R0
        # then learns about the slot changes only through UPDATE messages, which
        # report the owner of the first slot it claims with a stale config: R0
        # first gives slots 0-5460 to R3 but stays a primary, as it still owns
        # slot 5461, and then gives slot 5461 to R1, which isn't in its shard.
        set paused [list [srv -1 pid] [srv -4 pid]]
        foreach pid $paused {pause_process $pid}
        set loglines [count_log_lines 0]
        restart_server 0 true false
        wait_for_log_messages 0 [list "*My last slot was migrated to node $q_id*I am now an empty primary*"] $loglines 1000 50
        foreach pid $paused {resume_process $pid}

        # R0 must then become a replica of R3 and sync with it.
        wait_for_condition 1000 50 {
            [s 0 role] eq {slave} && [s 0 master_link_status] eq {up}
        } else {
            fail "R0 did not become a replica: [cluster_get_myself 0]"
        }
        verify_log_message 0 "*I own no slots and node $r_id*Reconfiguring myself as a replica of it*" $loglines
        foreach n {0 1 2 3 4 5} {
            wait_for_condition 1000 50 {
                [cluster_node_is_replica_of $n $p_id $r_id]
            } else {
                fail "R$n does not see R0 as a replica of R3"
            }
        }
        wait_for_ofs_sync [srv -3 client] [srv 0 client]
        assert_equal [R 0 dbsize] 100
    }
}

start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no cluster-require-full-coverage no}} {
    test "Stale primary with keys stays a primary until its keys are deleted" {
        set p_id [R 0 cluster myid]
        set q_id [R 1 cluster myid]
        set c_id [R 2 cluster myid]
        set r_id [R 3 cluster myid]
        wait_for_condition 1000 50 {
            [s -3 master_link_status] eq {up}
        } else {
            fail "R3 did not sync with R0"
        }

        stale_primary_failover_and_migrate {0} {1 2 3 4 5}

        # Same as above, but R3 is paused too, so that R0 stays a primary.
        set r_pid [srv -3 pid]
        set paused [list [srv -1 pid] [srv -4 pid]]
        foreach pid [concat $paused $r_pid] {pause_process $pid}
        set loglines [count_log_lines 0]
        restart_server 0 true false
        wait_for_log_messages 0 [list "*My last slot was migrated to node $q_id*I am now an empty primary*"] $loglines 1000 50
        foreach pid $paused {resume_process $pid}
        # R2 went into the minority while R1 and R3 were both paused.
        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {ok} && [CI 2 cluster_state] eq {ok}
        } else {
            fail "The cluster did not recover"
        }

        # R0 imports a key of shard C. The key now only exists in R0.
        for {set j 0} {$j < 1000} {incr j} {
            set key "key:$j"
            set key_slot [R 2 cluster keyslot $key]
            if {[stale_primary_slot_owner 2 $key_slot] eq $c_id} break
        }
        R 2 set $key value
        assert_equal OK [R 0 cluster setslot $key_slot importing $c_id]
        assert_equal OK [R 2 cluster setslot $key_slot migrating $p_id]
        assert_equal OK [R 2 migrate [srv 0 host] [srv 0 port] $key 0 5000]
        assert_equal [list [R 0 dbsize] [R 2 dbsize]] {1 0}

        # R0 must not become a replica of R3, as it would drop the key.
        set loglines [count_log_lines 0]
        resume_process $r_pid
        wait_for_log_messages 0 [list "*I own no slots and node $r_id*not reconfiguring myself as a replica of it because I have 1 key*"] $loglines 1000 50
        assert_equal [list [s 0 role] [R 0 dbsize]] {master 1}

        # R0 becomes a replica of R3 once the key is deleted.
        assert_equal OK [R 0 flushall]
        wait_for_condition 1000 50 {
            [s 0 role] eq {slave} && [s 0 master_link_status] eq {up}
        } else {
            fail "R0 did not become a replica: [cluster_get_myself 0]"
        }
        assert_equal [dict get [cluster_get_myself 0] slaveof] $r_id
    }
}

start_cluster 3 6 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no cluster-require-full-coverage no}} {
    test "Stale primary and its stale replica both rejoin their shard as replicas" {
        set p_id [R 0 cluster myid]
        set q_id [R 1 cluster myid]
        set r_id [R 3 cluster myid]
        set s_id [R 6 cluster myid]
        wait_for_condition 1000 50 {
            [s -3 master_link_status] eq {up} && [s -6 master_link_status] eq {up}
        } else {
            fail "The replicas of R0 did not sync"
        }

        # R6 goes down before R0, so that R3 takes over.
        stale_primary_failover_and_migrate {6 0} {1 2 3 4 5 7 8}

        set paused [list [srv -1 pid] [srv -4 pid] [srv -7 pid]]
        foreach pid $paused {pause_process $pid}
        set loglines [count_log_lines 0]
        restart_server -6 true false
        restart_server 0 true false
        wait_for_log_messages 0 [list "*My last slot was migrated to node $q_id*I am now an empty primary*"] $loglines 1000 50
        foreach pid $paused {resume_process $pid}

        # R0 becomes a replica of R3, and R6 follows it.
        foreach n {0 6} {
            wait_for_condition 1000 50 {
                [s [expr {-$n}] role] eq {slave} && [s [expr {-$n}] master_link_status] eq {up} &&
                [dict get [cluster_get_myself $n] slaveof] eq $r_id
            } else {
                fail "R$n did not become a replica of R3: [cluster_get_myself $n]"
            }
        }
        foreach n {1 2 3 4 5 7 8} {
            wait_for_condition 1000 50 {
                [cluster_node_is_replica_of $n $p_id $r_id] && [cluster_node_is_replica_of $n $s_id $r_id]
            } else {
                fail "R$n does not see R0 and R6 as replicas of R3"
            }
        }
    }
}

proc stale_primary_one_slot_allocation {masters replicas} {
    R 0 cluster addslots 0
    R 1 cluster addslotsrange 1 8191
    R 2 cluster addslotsrange 8192 16383
}

start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no cluster-require-full-coverage no}} {
    test "Empty primary with a stale view does not follow its former replica into another shard" {
        set p_id [R 0 cluster myid]
        set q_id [R 1 cluster myid]
        set x_id [R 3 cluster myid]
        wait_for_condition 1000 50 {
            [s -3 master_link_status] eq {up}
        } else {
            fail "R3 did not sync with R0"
        }

        # R0 gives its only slot to R1 and becomes an empty primary.
        assert_equal OK [R 1 cluster setslot 0 importing $p_id]
        assert_equal OK [R 0 cluster setslot 0 migrating $q_id]
        assert_equal OK [R 1 cluster setslot 0 node $q_id]
        assert_equal OK [R 0 cluster setslot 0 node $q_id]
        foreach n {0 1 2 3 4 5} {
            wait_for_condition 1000 50 {
                [stale_primary_slot_owner $n 0] eq $q_id
            } else {
                fail "R$n did not learn about the slot migration"
            }
        }
        set shard_a [R 0 cluster myshardid]

        # While R0 is down, R3 leaves shard A with CLUSTER RESET SOFT and
        # becomes the primary of a new shard, with the greatest config epoch.
        catch {R 0 shutdown nosave}
        assert_equal OK [R 3 cluster reset soft]
        assert_not_equal $shard_a [R 3 cluster myshardid]
        assert_equal OK [R 3 cluster meet [srv -1 host] [srv -1 port]]
        # The replica of R1 must know R3 as a primary too, as R1 replicates
        # CLUSTER SETSLOT to it.
        wait_for_condition 1000 50 {
            [stale_primary_sees_primary 1 $x_id] && [stale_primary_sees_primary 2 $x_id] &&
            [stale_primary_sees_primary 4 $x_id] && [stale_primary_sees_primary 5 $x_id] &&
            [stale_primary_sees_primary 3 $q_id]
        } else {
            fail "R3 did not rejoin the cluster as a primary"
        }
        assert_equal OK [R 3 cluster setslot 1 importing $q_id]
        assert_equal OK [R 1 cluster setslot 1 migrating $x_id]
        assert_equal OK [R 3 cluster setslot 1 node $x_id]
        assert_equal OK [R 1 cluster setslot 1 node $x_id]
        foreach n {1 2 4 5} {
            wait_for_condition 1000 50 {
                [stale_primary_slot_owner $n 1] eq $x_id
            } else {
                fail "R$n did not learn that R3 owns slot 1"
            }
        }

        # R0 restarts with its stale configuration, in which R3 is its replica
        # in shard A. Every packet of R3 that R0 processes tells it that R3 is
        # a primary with slot 1, with a greater config epoch than R0, and in
        # another shard.
        restart_server 0 true false
        wait_for_condition 1000 50 {
            [stale_primary_slot_owner 0 1] eq $x_id
        } else {
            fail "R0 did not learn that R3 owns slot 1"
        }
        assert_equal [s 0 role] {master} "R0 followed R3 into another shard"
        assert_equal [R 0 cluster myshardid] $shard_a
        assert_morethan [dict get [cluster_get_node_by_id 0 $x_id] config_epoch] [dict get [cluster_get_myself 0] config_epoch]
    }
} stale_primary_one_slot_allocation

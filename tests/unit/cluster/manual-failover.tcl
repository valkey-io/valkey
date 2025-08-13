# Check the manual failover
start_cluster 5 5 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test [srv 0 port]
}

test "Instance #5 is a slave" {
    assert {[s -5 role] eq {slave}}
}

test "Instance #5 synced with the master" {
    wait_for_condition 1000 50 {
        [s -5 master_link_status] eq {up}
    } else {
        fail "Instance #5 master link status is not up"
    }
}

set current_epoch [CI 1 cluster_current_epoch]

set numkeys 50000
set numops 10000
set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]
catch {unset content}
array set content {}

test "Send CLUSTER FAILOVER to #5, during load" {
    for {set j 0} {$j < $numops} {incr j} {
        # Write random data to random list.
        set listid [randomInt $numkeys]
        set key "key:$listid"
        set ele [randomValue]
        # We write both with Lua scripts and with plain commands.
        # This way we are able to stress Lua -> server command invocation
        # as well, that has tests to prevent Lua to write into wrong
        # hash slots.
        if {$listid % 2} {
            $cluster rpush $key $ele
        } else {
           $cluster eval {server.call("rpush",KEYS[1],ARGV[1])} 1 $key $ele
        }
        lappend content($key) $ele

        if {($j % 1000) == 0} {
            puts -nonewline W; flush stdout
        }

        if {$j == $numops/2} {R 5 cluster failover}
    }
}

test "Wait for failover" {
    wait_for_condition 1000 50 {
        [CI 1 cluster_current_epoch] > $current_epoch
    } else {
        fail "No failover detected"
    }
    wait_for_cluster_propagation
}

test "Cluster should eventually be up again" {
    wait_for_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test [srv -1 port]
}

test "Instance #5 is now a master" {
    assert {[s -5 role] eq {master}}
}

test "Verify $numkeys keys for consistency with logical content" {
    # Check that the Cluster content matches our logical content.
    foreach {key value} [array get content] {
        assert {[$cluster lrange $key 0 -1] eq $value}
    }
}

test "Instance #0 gets converted into a slave" {
    wait_for_condition 1000 50 {
        [s 0 role] eq {slave}
    } else {
        fail "Old master was not converted into slave"
    }
    wait_for_cluster_propagation
}

} ;# start_cluster

## Check that manual failover does not happen if we can't talk with the master.
start_cluster 5 5 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test [srv 0 port]
}

test "Instance #5 is a slave" {
    assert {[s -5 role] eq {slave}}
}

test "Instance #5 synced with the master" {
    wait_for_condition 1000 50 {
        [s -5 master_link_status] eq {up}
    } else {
        fail "Instance #5 master link status is not up"
    }
}

test "Make instance #0 unreachable without killing it" {
    R 0 deferred 1
    R 0 DEBUG SLEEP 2
}

test "Send CLUSTER FAILOVER to instance #5" {
    R 5 cluster failover
}

test "Instance #5 is still a slave after some time (no failover)" {
    after 1000
    assert {[s -5 role] eq {slave}}
}

test "Wait for instance #0 to return back alive" {
    R 0 deferred 0
    assert {[R 0 read] eq {OK}}
}

} ;# start_cluster

## Check with "force" failover happens anyway.
start_cluster 5 10 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test [srv 0 port]
}

test "Instance #5 is a slave" {
    assert {[s -5 role] eq {slave}}
}

test "Instance #5 synced with the master" {
    wait_for_condition 1000 50 {
        [s -5 master_link_status] eq {up}
    } else {
        fail "Instance #5 master link status is not up"
    }
}

test "Make instance #0 unreachable without killing it" {
    R 0 deferred 1
    R 0 DEBUG SLEEP 2
}

test "Send CLUSTER FAILOVER to instance #5" {
    R 5 cluster failover force
}

test "Instance #5 is a master after some time" {
    wait_for_condition 1000 50 {
        [s -5 role] eq {master}
    } else {
        fail "Instance #5 is not a master after some time regardless of FORCE"
    }
}

test "Wait for instance #0 to return back alive" {
    R 0 deferred 0
    assert {[R 0 read] eq {OK}}
}

} ;# start_cluster

start_cluster 3 1 {tags {external:skip cluster} overrides {cluster-ping-interval 1000 cluster-node-timeout 2000}} {
    test "Manual failover vote is not limited by two times the node timeout - drop the auth ack" {
        set CLUSTER_PACKET_TYPE_FAILOVER_AUTH_ACK 6
        set CLUSTER_PACKET_TYPE_NONE -1

        # Let replica drop FAILOVER_AUTH_ACK so that the election won't
        # get the enough votes and the election will time out.
        R 3 debug drop-cluster-packet-filter $CLUSTER_PACKET_TYPE_FAILOVER_AUTH_ACK

        # The first manual failover will time out.
        R 3 cluster failover
        wait_for_log_messages 0 {"*Manual failover timed out*"} 0 1000 50
        wait_for_log_messages -3 {"*Manual failover timed out*"} 0 1000 50

        # Undo packet drop, so that replica can win the next election.
        R 3 debug drop-cluster-packet-filter $CLUSTER_PACKET_TYPE_NONE

        # Make sure the second manual failover will work.
        R 3 cluster failover
        wait_for_condition 1000 50 {
            [s 0 role] eq {slave} &&
            [s -3 role] eq {master}
        } else {
            fail "The second failover does not happen"
        }
        wait_for_cluster_propagation
    }
} ;# start_cluster

start_cluster 3 1 {tags {external:skip cluster} overrides {cluster-ping-interval 1000 cluster-node-timeout 2000}} {
    test "Manual failover vote is not limited by two times the node timeout - mixed failover" {
        # Make sure the failover is triggered by us.
        R 1 config set cluster-replica-validity-factor 0
        R 3 config set cluster-replica-no-failover yes
        R 3 config set cluster-replica-validity-factor 0

        # Pause the primary.
        pause_process [srv 0 pid]
        wait_for_cluster_state fail

        # R 3 performs an automatic failover and it will work.
        R 3 config set cluster-replica-no-failover no
        wait_for_condition 1000 50 {
            [s -3 role] eq {master}
        } else {
            fail "The first failover does not happen"
        }

        # Resume the primary and wait for it to become a replica.
        resume_process [srv 0 pid]
        wait_for_condition 1000 50 {
            [s 0 role] eq {slave}
        } else {
            fail "Old primary not converted into replica"
        }
        wait_for_cluster_propagation

        # The old primary doing a manual failover and wait for it.
        R 0 cluster failover
        wait_for_condition 1000 50 {
            [s 0 role] eq {master} &&
            [s -3 role] eq {slave}
        } else {
            fail "The second failover does not happen"
        }
        wait_for_cluster_propagation

        # R 3 performs a manual failover and it will work.
        R 3 cluster failover
        wait_for_condition 1000 50 {
            [s 0 role] eq {slave} &&
            [s -3 role] eq {master}
        } else {
            fail "The third falover does not happen"
        }
        wait_for_cluster_propagation
    }
} ;# start_cluster

start_cluster 3 1 {tags {external:skip cluster} overrides {cluster-ping-interval 1000 cluster-node-timeout 2000}} {
    test "Automatic failover vote is not limited by two times the node timeout - mixed failover" {
        R 3 cluster failover
        wait_for_condition 1000 50 {
            [s 0 role] eq {slave} &&
            [s -3 role] eq {master}
        } else {
            fail "The first failover does not happen"
        }
        wait_for_cluster_propagation

        R 0 cluster failover
        wait_for_condition 1000 50 {
            [s 0 role] eq {master} &&
            [s -3 role] eq {slave}
        } else {
            fail "The second failover does not happen"
        }
        wait_for_cluster_propagation

        # Let R 3 trigger the automatic failover
        pause_process [srv 0 pid]
        wait_for_condition 1000 50 {
            [s -3 role] eq {master}
        } else {
            fail "The third failover does not happen"
        }
    }
} ;# start_cluster

start_cluster 3 1 {tags {external:skip cluster} overrides {cluster-ping-interval 1000 cluster-node-timeout 15000}} {
    test "Manual failover will reset the on-going election" {
        set CLUSTER_PACKET_TYPE_FAILOVER_AUTH_REQUEST 5
        set CLUSTER_PACKET_TYPE_NONE -1

        # Let other primaries drop FAILOVER_AUTH_REQUEST so that the election won't
        # get the enough votes and the election will time out.
        R 1 debug drop-cluster-packet-filter $CLUSTER_PACKET_TYPE_FAILOVER_AUTH_REQUEST
        R 2 debug drop-cluster-packet-filter $CLUSTER_PACKET_TYPE_FAILOVER_AUTH_REQUEST

        # Replica doing the manual failover.
        R 3 cluster failover

        # Waiting for primary and replica to confirm manual failover timeout.
        wait_for_log_messages 0 {"*Manual failover timed out*"} 0 1000 50
        wait_for_log_messages -3 {"*Manual failover timed out*"} 0 1000 50
        set loglines1 [count_log_lines 0]
        set loglines2 [count_log_lines -3]

        # Undo packet drop, so that replica can win the next election.
        R 1 debug drop-cluster-packet-filter $CLUSTER_PACKET_TYPE_NONE
        R 2 debug drop-cluster-packet-filter $CLUSTER_PACKET_TYPE_NONE

        # Replica doing the manual failover again.
        R 3 cluster failover

        # Make sure the election is reset.
        wait_for_log_messages -3 {"*Failover election in progress*Resetting the election*"} $loglines2 1000 50

        # Wait for failover.
        wait_for_condition 1000 50 {
            [s -3 role] == "master"
        } else {
            fail "No failover detected"
        }

        # Make sure that the second manual failover does not time out.
        verify_no_log_message 0 "*Manual failover timed out*" $loglines1
        verify_no_log_message -3 "*Manual failover timed out*" $loglines2
    }
} ;# start_cluster

start_cluster 3 1 {tags {external:skip cluster} overrides {cluster-ping-interval 1000 cluster-node-timeout 1000}} {
    test "Broadcast PONG to the cluster when the node role changes" {
        # R0 is a primary and R3 is a replica, we will do multiple cluster failover
        # and then check their role and flags.
        set R0_nodeid [R 0 cluster myid]
        set R3_nodeid [R 3 cluster myid]

        # Make sure we don't send PINGs for a short period of time.
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            R $j debug disable-cluster-random-ping 1
            R $j config set cluster-ping-interval 300000
        }

        R 3 cluster failover
        wait_for_condition 1000 50 {
            [s 0 role] eq {slave} &&
            [s -3 role] eq {master}
        } else {
            fail "Failover does not happened"
        }

        # Get the node information of R0 and R3 in my view from CLUSTER NODES
        # R0 should be a replica and R3 should be a primary in all views.
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            wait_for_condition 1000 50 {
                [check_cluster_node_mark slave $j $R0_nodeid] &&
                [check_cluster_node_mark master $j $R3_nodeid]
            } else {
                puts "R0_nodeid: $R0_nodeid"
                puts "R3_nodeid: $R3_nodeid"
                puts "R $j cluster nodes:"
                puts [R $j cluster nodes]
                fail "Node role does not changed in the first failover"
            }
        }

        R 0 cluster failover
        wait_for_condition 1000 50 {
            [s 0 role] eq {master} &&
            [s -3 role] eq {slave}
        } else {
            fail "The second failover does not happened"
        }

        # Get the node information of R0 and R3 in my view from CLUSTER NODES
        # R0 should be a primary and R3 should be a replica in all views.
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            wait_for_condition 1000 50 {
                [check_cluster_node_mark master $j $R0_nodeid] &&
                [check_cluster_node_mark slave $j $R3_nodeid]
            } else {
                puts "R0_nodeid: $R0_nodeid"
                puts "R3_nodeid: $R3_nodeid"
                puts "R $j cluster nodes:"
                puts [R $j cluster nodes]
                fail "Node role does not changed in the second failover"
            }
        }
    }
} ;# start_cluster

start_cluster 3 1 {tags {external:skip cluster}} {
    # In the R0/R3 shard, R0 is the primary node and R3 is the replica.
    #
    # We trigger a manual failover on R3.
    #
    # When R3 becomes the new primary node, it will broadcast a message to all
    # nodes in the cluster.
    # When R0 receives the message, it becomes the new replica and also will
    # broadcast the message to all nodes in the cluster.
    #
    # Let's assume that R1 and R2 receive the message from R0 (new replica) first
    # and then the message from R3 (new primary) later.
    #
    # The purpose of this test is to verify the behavior of R1 and R2 after receiving
    # the message from R0 (new replica) first. R1 and R2 will update R0 as a replica
    # and R3 as a primary, and transfer all slots of R0 to R3.
    test "The role change and the slot ownership change should be an atomic operation" {
        set R0_nodeid [R 0 cluster myid]
        set R1_nodeid [R 1 cluster myid]
        set R2_nodeid [R 2 cluster myid]
        set R3_nodeid [R 3 cluster myid]

        set R0_shardid [R 0 cluster myshardid]
        set R3_shardid [R 3 cluster myshardid]
        assert_equal $R0_shardid $R3_shardid

        # We also take this opportunity to verify slot migration.
        # Move slot 0 from R0 to R1. Move slot 5462 from R1 to R0.
        R 0 cluster setslot 0 migrating $R1_nodeid
        R 1 cluster setslot 0 importing $R0_nodeid
        R 1 cluster setslot 5462 migrating $R0_nodeid
        R 0 cluster setslot 5462 importing $R1_nodeid
        assert_equal [get_open_slots 0] "\[0->-$R1_nodeid\] \[5462-<-$R1_nodeid\]"
        assert_equal [get_open_slots 1] "\[0-<-$R0_nodeid\] \[5462->-$R0_nodeid\]"
        wait_for_slot_state 3 "\[0->-$R1_nodeid\] \[5462-<-$R1_nodeid\]"

        # Ensure that related nodes do not reconnect.
        R 1 debug disable-cluster-reconnection 1
        R 2 debug disable-cluster-reconnection 1
        R 3 debug disable-cluster-reconnection 1

        # After killing the cluster link, ensure that R1 and R2 do not receive
        # messages from R3 (new primary).
        R 1 debug clusterlink kill all $R3_nodeid
        R 2 debug clusterlink kill all $R3_nodeid
        R 3 debug clusterlink kill all $R1_nodeid
        R 3 debug clusterlink kill all $R2_nodeid

        set loglines1 [count_log_lines -1]
        set loglines2 [count_log_lines -2]

        R 3 cluster failover takeover

        # Check that from the perspectives of R1 and R2, R0 becomes a replica and
        # R3 becomes the new primary.
        wait_for_condition 1000 10 {
            [cluster_has_flag [cluster_get_node_by_id 1 $R0_nodeid] slave] eq 1 &&
            [cluster_has_flag [cluster_get_node_by_id 1 $R3_nodeid] master] eq 1 &&

            [cluster_has_flag [cluster_get_node_by_id 2 $R0_nodeid] slave] eq 1 &&
            [cluster_has_flag [cluster_get_node_by_id 2 $R3_nodeid] master] eq 1
        } else {
            fail "The node is not marked with the correct flag"
        }

        # Check that R0 (replica) does not own any slots and R3 (new primary) owns
        # the slots.
        assert_equal {} [dict get [cluster_get_node_by_id 1 $R0_nodeid] slots]
        assert_equal {} [dict get [cluster_get_node_by_id 2 $R0_nodeid] slots]
        assert_equal {0-5461} [dict get [cluster_get_node_by_id 1 $R3_nodeid] slots]
        assert_equal {0-5461} [dict get [cluster_get_node_by_id 2 $R3_nodeid] slots]

        # Check that in the R1 perspective, both migration-source and migration-target
        # have moved from R0 to R1.
        assert_equal [get_open_slots 0] "\[0->-$R1_nodeid\] \[5462-<-$R1_nodeid\]"
        assert_equal [get_open_slots 1] "\[0-<-$R3_nodeid\] \[5462->-$R3_nodeid\]"
        assert_equal [get_open_slots 3] "\[0->-$R1_nodeid\] \[5462-<-$R1_nodeid\]"

        # A failover occurred in shard, we will only go to this code branch,
        # verify we print the logs.

        # Both importing slots and migrating slots are move to R3.
        set pattern "*Failover occurred in migration source. Update importing source for slot 0 to node $R3_nodeid () in shard $R3_shardid*"
        verify_log_message -1 $pattern $loglines1
        set pattern "*Failover occurred in migration target. Slot 5462 is now being migrated to node $R3_nodeid () in shard $R3_shardid*"
        verify_log_message -1 $pattern $loglines1

        # Both slots are move to R3.
        set R0_slots 5462
        set pattern "*A failover occurred in shard $R3_shardid; node $R0_nodeid () lost $R0_slots slot(s) and failed over to node $R3_nodeid*"
        verify_log_message -1 $pattern $loglines1
        verify_log_message -2 $pattern $loglines2

        # Both importing slots and migrating slots are move to R3.
        set pattern "*A failover occurred in migration source. Update importing source of 1 slot(s) to node $R3_nodeid () in shard $R3_shardid*"
        verify_log_message -1 $pattern $loglines1
        set pattern "*A failover occurred in migration target. Update migrating target of 1 slot(s) to node $R3_nodeid () in shard $R3_shardid*"
        verify_log_message -1 $pattern $loglines1

        R 1 debug disable-cluster-reconnection 0
        R 2 debug disable-cluster-reconnection 0
        R 3 debug disable-cluster-reconnection 0

        wait_for_cluster_propagation
    }
}

start_cluster 3 2 {tags {external:skip cluster}} {
    # In the R0/R3/R4 shard, R0 is the primary node, R3 and R4 are the replicas.
    #
    # We trigger a manual failover on R3.
    #
    # When R3 becomes the new primary node, it will broadcast a message to all
    # nodes in the cluster.
    # When R0 receives the message, it becomes the new replica and also will
    # broadcast the message to all nodes in the cluster.
    #
    # Let's assume that R4 receive the message from R0 (new replica) first
    # and then the message from R3 (new primary) later. In the past it would
    # have created a replication loop, that is R4->R0->R3->R0.
    #
    # The purpose of this test is to verify the behavior of R4 after receiving
    # the message from R0 (new replica) first. R4 will set R3 as the primary node
    # and set R0 as a replica of R3. After it realizes that a sub-replica has appeared
    # in this case R4->R0->R3, it will set itself as a replica of R3.
    test "Node will fix the replicaof when it finds that it is a sub-replica" {
        # We make R4 become a fresh new node.
        isolate_node 4

        set R0_nodeid [R 0 cluster myid]
        set R3_nodeid [R 3 cluster myid]
        set R4_nodeid [R 4 cluster myid]

        # Add R4 and wait for R4 to become a replica of R0.
        R 4 cluster meet [srv 0 host] [srv 0 port]
        wait_for_condition 50 100 {
            [cluster_get_node_by_id 4 $R0_nodeid] != {}
        } else {
            fail "Node R4 never learned about node R0"
        }
        R 4 cluster replicate $R0_nodeid
        wait_for_sync [srv -4 client]

        wait_for_cluster_propagation

        # Ensure that related nodes do not reconnect.
        R 3 debug disable-cluster-reconnection 1
        R 4 debug disable-cluster-reconnection 1

        # After killing the cluster link, ensure that R4 do not receive messages
        # from R3 (new primary), but can receive message from R0 (new replica).
        R 4 debug clusterlink kill all $R3_nodeid
        R 3 debug clusterlink kill all $R4_nodeid

        R 3 cluster failover takeover

        # Wait for the failover to success in R4's view.
        wait_for_condition 1000 10 {
            [cluster_has_flag [cluster_get_node_by_id 4 $R3_nodeid] master] eq 1 &&
            [dict get [cluster_get_node_by_id 4 $R3_nodeid] slaveof] eq "-" &&

            [cluster_has_flag [cluster_get_node_by_id 4 $R0_nodeid] slave] eq 1 &&
            [dict get [cluster_get_node_by_id 4 $R0_nodeid] slaveof] eq $R3_nodeid &&

            [cluster_has_flag [cluster_get_node_by_id 4 $R4_nodeid] slave] eq 1 &&
            [dict get [cluster_get_node_by_id 4 $R4_nodeid] slaveof] eq $R3_nodeid
        } else {
            puts "R 4 cluster nodes:"
            puts [R 4 cluster nodes]
            fail "The node is not marked with the correct flag"
        }

        # Make sure R4 indeed detect the sub-replica and fixed the replicaof.
        set pattern "*I'm a sub-replica! Reconfiguring myself as a replica of $R3_nodeid from $R0_nodeid*"
        verify_log_message -4 $pattern 0

        R 3 debug disable-cluster-reconnection 0
        R 4 debug disable-cluster-reconnection 0

        wait_for_cluster_propagation

        # Finally, assert one more times that each replicaof is correct.
        assert_equal [dict get [cluster_get_node_by_id 4 $R3_nodeid] slaveof] "-"
        assert_equal [dict get [cluster_get_node_by_id 4 $R0_nodeid] slaveof] $R3_nodeid
        assert_equal [dict get [cluster_get_node_by_id 4 $R4_nodeid] slaveof] $R3_nodeid
    }
}

# Disable this test case due to #2441.
if {false} {
start_cluster 3 2 {tags {external:skip cluster}} {
    # This test consists of two phases.
    # The first phase, we will create a scenario where two primary are on the same shard. See #2279 for more details.
    # The second phase, we will test the behavior of the node when packets arrive out of order. See #2301 for more details.
    #
    # The first phase.
    # In the R0/R3/R4 shard, R0 is the primary (cluster-allow-replica-migration no), R3 is the replica, R4 will be a replica later.
    # 1. R0 goes down, and R3 trigger a failover and become the new primary.
    # 2. R0 (old primary) continues to be down while another R4 is added as a replica of R3 (new primary).
    # 3. R3 (new primary) goes down, and R4 trigger a failover and become the new primary.
    # 4. R0 (old primary) and R3 (old primary) come back up and start learning about the new topology.
    # 5. R0 (old primary) comes up thinking it was the primary, but has an older config epoch compared to R4 (new primary).
    # 6. R0 (old primary) learns about R4 (new primary) as a new node via gossip and assigns it a random shard_id.
    # 7. R0 (old primary) receives a direct ping from R4 (new primary).
    #    a. R4 (new primary) advertises the same set of slots that R0 (old primary) was earlier owning.
    #    b. Since R0 (old primary) assigns a random shard_id to R4 (new primary) early, R0 (old primary) thinks
    #       that it is still a primary and it lost all its slots to R4 (new primary), which is in another shard.
    #       R0 (old primary) become an empty primary.
    #    c. R0 (empty primary) then updates the actual shard_id of R4 (new primary) while processing the ping extensions.
    # 9. R0 (empty primary) and R4 (new primary) end up being primaries in the same shard while R4 continues to own slots.
    #
    # The second phase.
    # In the R0/R3/R4 shard, R4 is the primary, R3 is the replica, and R0 is en empty primary.
    # 1. We will perform a failover on R3, and perform a replicate on R0 to make R0 a replica of R3.
    # 2. When R3 becomes the new primary node, it will broadcast a message to all nodes in the cluster.
    # 3. When R4 receives the message, it becomes the new replica and also will broadcast the message to all nodes in the cluster.
    # 4. When R0 becomes a replica after the replication, it will broadcast a message to all nodes in the cluster.
    # 5. Let's assume that R1 and R2 receive the message from R0 and R4 first and then the message from R3 (new primary) later.
    # 6. R1 will receive messages from R0 after the replication, R0 is a replica, and its primary is R3.
    # 7. R2 will receive messages from R4 after the failover, R4 is a replica, and its primary is R3.
    test "Combined the test cases of #2279 and #2301 to test #2431" {
        # ============== Phase 1 start ==============
        R 0 config set cluster-allow-replica-migration no

        set CLUSTER_PACKET_TYPE_NONE -1
        set CLUSTER_PACKET_TYPE_ALL -2

        # We make R4 become a fresh new node.
        isolate_node 4

        # Set debug to R0 so that no packets can be exchanged when we resume it.
        R 0 debug disable-cluster-reconnection 1
        R 0 debug close-cluster-link-on-packet-drop 1
        R 0 debug drop-cluster-packet-filter $CLUSTER_PACKET_TYPE_ALL

        # Pause R0 and wait for R3 to become a new primary.
        pause_process [srv 0 pid]
        R 3 cluster failover force
        wait_for_condition 1000 50 {
            [s -3 role] eq {master}
        } else {
            fail "Failed waiting for R3 to takeover primaryship"
        }

        # Add R4 and wait for R4 to become a replica of R3.
        R 4 cluster meet [srv -3 host] [srv -3 port]
        wait_for_condition 50 100 {
            [cluster_get_node_by_id 4 [R 3 cluster myid]] != {}
        } else {
            fail "Node R4 never learned about node R3"
        }
        R 4 cluster replicate [R 3 cluster myid]
        wait_for_sync [srv -4 client]

        # Pause R3 and wait for R4 to become a new primary.
        pause_process [srv -3 pid]
        R 4 cluster failover takeover
        wait_for_condition 1000 50 {
            [s -4 role] eq {master}
        } else {
            fail "Failed waiting for R4 to become primary"
        }

        # Resume R0 and R3
        resume_process [srv 0 pid]
        resume_process [srv -3 pid]

        # Make sure R0 drop all the links so that it won't get the pending packets.
        wait_for_condition 1000 50 {
            [R 0 cluster links] eq {}
        } else {
            fail "Failed waiting for A to drop all cluster links"
        }

        # Un-debug R0 and let's start exchanging packets.
        R 0 debug disable-cluster-reconnection 0
        R 0 debug close-cluster-link-on-packet-drop 0
        R 0 debug drop-cluster-packet-filter $CLUSTER_PACKET_TYPE_NONE

        # ============== Phase 1 end ==============

        wait_for_cluster_propagation

        # ============== Phase 2 start ==============

        set R0_nodeid [R 0 cluster myid]
        set R1_nodeid [R 1 cluster myid]
        set R2_nodeid [R 2 cluster myid]
        set R3_nodeid [R 3 cluster myid]
        set R4_nodeid [R 4 cluster myid]

        set R0_shardid [R 0 cluster myshardid]
        set R3_shardid [R 3 cluster myshardid]
        set R4_shardid [R 4 cluster myshardid]

        # R0 now is an empty primary, R4 is the primary, R3 is the replica.
        # They are both in the same shard, this may be changed in #2279, and
        # the assert can be removed then.
        assert_equal [s 0 role] "master"
        assert_equal [s -3 role] "slave"
        assert_equal [s -4 role] "master"
        assert_equal $R0_shardid $R3_shardid
        assert_equal $R0_shardid $R4_shardid

        # Ensure that related nodes do not reconnect after we kill the cluster links.
        R 1 debug disable-cluster-reconnection 1
        R 2 debug disable-cluster-reconnection 1
        R 3 debug disable-cluster-reconnection 1
        R 4 debug disable-cluster-reconnection 1

        # R3 doing the failover, and R0 doing the replicate with R3.
        # R3 become the new primary after the failover.
        # R4 become a replica after the failover.
        # R0 become a replica after the replicate.
        # Before we do that, kill the cluster link to create test conditions.
        # Ensure that R1 and R2 of the other shards do not receive packets from R3 (new primary),
        # but receive packets from R0 and R4 respectively first.

        # R1 first receives the packet from R0.
        # Kill the cluster links between R1 and R3, and between R1 and R4 ensure that:
        # R1 can not receive messages from R3 (new primary),
        # R1 can not receive messages from R4 (replica),
        # and R1 can receive message from R0 (new replica).
        R 1 debug clusterlink kill all $R3_nodeid
        R 3 debug clusterlink kill all $R1_nodeid
        R 1 debug clusterlink kill all $R4_nodeid
        R 4 debug clusterlink kill all $R1_nodeid

        # R2 first receives the packet from R4.
        # Kill the cluster links between R2 and R3, and between R2 and R0 ensure that:
        # R2 can not receive messages from R3 (new primary),
        # R2 can not receive messages from R0 (new replica),
        # and R2 can receive message from R4 (replica).
        R 2 debug clusterlink kill all $R3_nodeid
        R 3 debug clusterlink kill all $R2_nodeid
        R 2 debug clusterlink kill all $R0_nodeid
        R 0 debug clusterlink kill all $R2_nodeid

        # R3 doing the failover, and R0 doing the replicate with R3
        R 3 cluster failover takeover
        wait_for_condition 1000 10 {
            [cluster_has_flag [cluster_get_node_by_id 0 $R3_nodeid] master] eq 1
        } else {
            fail "R3 does not become the primary node"
        }
        R 0 cluster replicate $R3_nodeid

        # Check that from the perspective of R1 and R2, when they first receive the
        # replica's packet, they correctly fix the sender's and its primary's role.

        # Check that from the perspectives of R1, when receiving the packet from R0,
        # R0 is a replica, and its primary is R3, this is due to replicate.
        wait_for_condition 1000 10 {
            [cluster_has_flag [cluster_get_node_by_id 1 $R0_nodeid] slave] eq 1 &&
            [cluster_has_flag [cluster_get_node_by_id 1 $R3_nodeid] master] eq 1
        } else {
            puts "R1 cluster nodes:"
            puts [R 1 cluster nodes]
            fail "The node is not marked with the correct flag in R1's view"
        }

        # Check that from the perspectives of R2, when receiving the packet from R4,
        # R4 is a replica, and its primary is R4, this is due to failover.
        wait_for_condition 1000 10 {
            [cluster_has_flag [cluster_get_node_by_id 2 $R4_nodeid] slave] eq 1 &&
            [cluster_has_flag [cluster_get_node_by_id 2 $R3_nodeid] master] eq 1
        } else {
            puts "R2 cluster nodes:"
            puts [R 2 cluster nodes]
            fail "The node is not marked with the correct flag in R2's view"
        }

        # ============== Phase 2 end ==============

        R 0 debug disable-cluster-reconnection 0
        R 1 debug disable-cluster-reconnection 0
        R 2 debug disable-cluster-reconnection 0
        R 3 debug disable-cluster-reconnection 0
        R 4 debug disable-cluster-reconnection 0
        wait_for_cluster_propagation
    }
}
}

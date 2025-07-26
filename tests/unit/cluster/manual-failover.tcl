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
    R 0 DEBUG SLEEP 10
}

test "Send CLUSTER FAILOVER to instance #5" {
    R 5 cluster failover
}

test "Instance #5 is still a slave after some time (no failover)" {
    after 5000
    assert {[s -5 role] eq {master}}
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
    R 0 DEBUG SLEEP 10
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
    # We trigger a manually failover on R3.
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

        # A failover occurred in shard, we will only go to this code branch,
        # verify we print the logs.
        set R0_slots 5462
        set pattern "*A failover occurred in shard $R3_shardid; node $R0_nodeid () lost $R0_slots slot(s) and failed over to node $R3_nodeid*"
        verify_log_message -1 $pattern $loglines1
        verify_log_message -2 $pattern $loglines2

        R 1 debug disable-cluster-reconnection 0
        R 2 debug disable-cluster-reconnection 0
        R 3 debug disable-cluster-reconnection 0

        wait_for_cluster_propagation
    }
}

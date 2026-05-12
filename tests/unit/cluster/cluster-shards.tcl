# Get the node info with the specific node_id from the
# given reference node. Valid type options are "node" and "shard"
proc get_node_info_from_shard {id reference {type node}} {
    set shards_response [R $reference CLUSTER SHARDS]
    foreach shard_response $shards_response {
        set nodes [dict get $shard_response nodes]
        foreach node $nodes {
            if {[dict get $node id] eq $id} {
                if {$type eq "node"} {
                    return $node
                } elseif {$type eq "shard"} {
                    return $shard_response
                } else {
                    return {}
                }
            }
        }
    }
    # No shard found, return nothing
    return {}
}

start_cluster 3 3 {tags {external:skip cluster}} {
    set primary_node 0
    set replica_node 3
    set validation_node 4
    set node_0_id ""
    set shard_0_slot_coverage {0 5461}

    test "Cluster should start ok" {
        wait_for_cluster_state ok
    }

    test "Cluster shards response is ok for shard 0" {
        set node_0_id [R $primary_node CLUSTER MYID]
        assert_equal $shard_0_slot_coverage [dict get [get_node_info_from_shard $node_0_id $validation_node "shard"] "slots"]
    }

    test "Kill a node and tell the replica to immediately takeover" {
        pause_process [srv $primary_node pid]
        R $replica_node CLUSTER failover force
    }

    test "Verify health as fail for killed node" {
        wait_for_condition 1000 50 {
            "fail" eq [dict get [get_node_info_from_shard $node_0_id $validation_node "node"] "health"]
        } else {
            fail "New primary never detected the node failed"
        }
    }

    test "CLUSTER SHARDS slot response is non-empty when primary node fails" {
        assert_equal $shard_0_slot_coverage [dict get [get_node_info_from_shard $node_0_id $validation_node "shard"] "slots"]
    }
}
# Initial slot distribution for split-slot cluster tests.
set ::slot0 [list 0 1000 1002 5459 5461 5461 10926 10926]
set ::slot1 [list 5460 5460 5462 10922 10925 10925]
set ::slot2 [list 10923 10924 10927 16383]
set ::slot3 [list 1001 1001]

# Slot allocator: assigns split slots to each master.
proc split_slot_allocation {masters replicas} {
    for {set j 0} {$j < $masters} {incr j} {
        R $j cluster ADDSLOTSRANGE {*}[set ::slot${j}]
    }
}

# Replica allocator: allocates only masters replicas, leaving the last server
# (R 8) as a standalone no-slot node for testing purposes.
proc split_slot_replica_allocation {masters replicas} {
    cluster_allocate_replicas $masters [expr {$replicas - 1}]
}

proc cluster_ensure_master {id} {
    if { [regexp "master" [R $id role]] == 0 } {
        assert_equal {OK} [R $id CLUSTER FAILOVER]
        wait_for_condition 50 100 {
            [regexp "master" [R $id role]] == 1
        } else {
            fail "instance $id is not master"
        }
    }
}

# start_cluster 4 masters + 5 nodes (4 replicas + 1 standalone R8)
start_cluster 4 5 {tags {external:skip cluster}} {

# cluster_master_nodes and cluster_replica_nodes refer to the active cluster members.
set ::cluster_master_nodes 4
set ::cluster_replica_nodes 4

test "Cluster should start ok" {
    wait_for_cluster_state ok
}

test "Set cluster hostnames and verify they are propagated" {
    for {set j 0} {$j < $::cluster_master_nodes + $::cluster_replica_nodes} {incr j} {
        R $j config set cluster-announce-hostname "host-$j.com"
    }

    # Wait for everyone to agree about the state
    wait_for_cluster_propagation
}

test "Verify information about the shards" {
    set ids {}
    for {set j 0} {$j < $::cluster_master_nodes + $::cluster_replica_nodes} {incr j} {
        lappend ids [R $j CLUSTER MYID]
    }
    set slots [list $::slot0 $::slot1 $::slot2 $::slot3 $::slot0 $::slot1 $::slot2 $::slot3]

    # Verify on each node (primary/replica), the response of the `CLUSTER SLOTS` command is consistent.
    for {set ref 0} {$ref < $::cluster_master_nodes + $::cluster_replica_nodes} {incr ref} {
        for {set i 0} {$i < $::cluster_master_nodes + $::cluster_replica_nodes} {incr i} {
            assert_equal [lindex $slots $i] [dict get [get_node_info_from_shard [lindex $ids $i] $ref "shard"] slots]
            assert_equal "host-$i.com" [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] hostname]
            assert_equal "127.0.0.1"  [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] ip]
            # Default value of 'cluster-preferred-endpoint-type' is ip.
            assert_equal "127.0.0.1"  [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] endpoint]

            if {$::tls} {
                assert_equal [srv [expr -1*$i] pport] [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] port]
                assert_equal [srv [expr -1*$i] port] [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] tls-port]
            } else {
                assert_equal [srv [expr -1*$i] port] [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] port]
            }

            if {$i < 4} {
                assert_equal "master" [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] role]
                assert_equal "online" [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] health]
            } else {
                assert_equal "replica" [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] role]
                # Replica could be in online or loading
            }
        }
    }
}

test "Verify no slot shard" {
    # R 8 is a standalone node with no slots assigned (left standalone by split_slot_replica_allocation)
    set node_8_id [R 8 CLUSTER MYID]
    assert_equal {} [dict get [get_node_info_from_shard $node_8_id 8 "shard"] slots]
    assert_equal {} [dict get [get_node_info_from_shard $node_8_id 0 "shard"] slots]
}

set node_0_id [R 0 CLUSTER MYID]

test "Kill a node and tell the replica to immediately takeover" {
    pause_process [srv 0 pid]
    R 4 cluster failover force
}

# Primary 0 node should report as fail, wait until the new primary acknowledges it.
test "Verify health as fail for killed node" {
    wait_for_condition 1000 50 {
        "fail" eq [dict get [get_node_info_from_shard $node_0_id 4 "node"] "health"]
    } else {
        fail "New primary never detected the node failed"
    }
}

set primary_id 4
set replica_id 0

test "Restarting primary node" {
    restart_server [expr -1*$replica_id] true false
}

test "Instance #0 gets converted into a replica" {
    wait_for_condition 1000 50 {
        [s [expr -1*$replica_id] role] eq {slave}
    } else {
        fail "Old primary was not converted into replica"
    }
}

test "Test the replica reports a loading state while it's loading" {
    # Test the command is good for verifying everything moves to a happy state
    set replica_cluster_id [R $replica_id CLUSTER MYID]
    wait_for_condition 50 1000 {
        [dict get [get_node_info_from_shard $replica_cluster_id $primary_id "node"] health] eq "online"
    } else {
        fail "Replica never transitioned to online"
    }

    # Set 1 MB of data, so there is something to load on full sync
    R $primary_id debug populate 1000 key 1000

    # Kill replica client for primary and load new data to the primary
    R $primary_id config set repl-backlog-size 100

    # Set the key load delay so that it will take at least
    # 2 seconds to fully load the data.
    R $replica_id config set key-load-delay 4000

    # Trigger event loop processing every 1024 bytes, this trigger
    # allows us to send and receive cluster messages, so we are setting
    # it low so that the cluster messages are sent more frequently.
    R $replica_id config set loading-process-events-interval-bytes 1024

    R $primary_id multi
    R $primary_id client kill type replica
    # populate the correct data
    set num 100
    set value [string repeat A 1024]
    for {set j 0} {$j < $num} {incr j} {
        # Use hashtag valid for shard #0
        set key "{ch3}$j"
        R $primary_id set $key $value
    }
    R $primary_id exec

    # The replica should reconnect and start a full sync, it will gossip about it's health to the primary.
    wait_for_condition 50 1000 {
        "loading" eq [dict get [get_node_info_from_shard $replica_cluster_id $primary_id "node"] health]
    } else {
        fail "Replica never transitioned to loading"
    }

    # Verify cluster shards and cluster slots (deprecated) API responds while the node is loading data.
    R $replica_id CLUSTER SHARDS
    R $replica_id CLUSTER SLOTS

    # Speed up the key loading and verify everything resumes
    R $replica_id config set key-load-delay 0

    wait_for_condition 50 1000 {
        "online" eq [dict get [get_node_info_from_shard $replica_cluster_id $primary_id "node"] health]
    } else {
        fail "Replica never transitioned to online"
    }

    # Final sanity, the replica agrees it is online.
    assert_equal "online" [dict get [get_node_info_from_shard $replica_cluster_id $replica_id "node"] health]
}

test "Regression test for a crash when calling SHARDS during handshake" {
    # Use R 8 (standalone node) to establish handshaking connections
    set id [R 8 CLUSTER MYID]
    R 8 CLUSTER RESET HARD
    for {set i 0} {$i < 8} {incr i} {
        R $i CLUSTER FORGET $id
    }
    R 8 cluster meet 127.0.0.1 [srv 0 port]
    # This line would previously crash, since all the outbound
    # connections were in handshake state.
    R 8 CLUSTER SHARDS
}

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Shard ids are unique" {
    set shard_ids {}
    for {set i 0} {$i < 4} {incr i} {
        set shard_id [R $i cluster myshardid]
        assert_equal [dict exists $shard_ids $shard_id] 0
        dict set shard_ids $shard_id 1
    }
}

test "CLUSTER MYSHARDID reports same id for both primary and replica" {
    for {set i 0} {$i < 4} {incr i} {
        assert_equal [R $i cluster myshardid] [R [expr $i+4] cluster myshardid]
        assert_equal [string length [R $i cluster myshardid]] 40
    }
}

test "New replica receives primary's shard id" {
    # find a primary
    set id 0
    for {} {$id < 8} {incr id} {
        if {[regexp "master" [R $id role]]} {
            break
        }
    }
    assert_not_equal [R 8 cluster myshardid] [R $id cluster myshardid]
    assert_equal {OK} [R 8 cluster replicate [R $id cluster myid]]
    assert_equal [R 8 cluster myshardid] [R $id cluster myshardid]
}

test "CLUSTER MYSHARDID reports same shard id after shard restart" {
    set node_ids {}
    for {set i 0} {$i < 8} {incr i 4} {
        dict set node_ids $i [R $i cluster myshardid]
        pause_process [srv [expr -1*$i] pid]
    }
    for {set i 0} {$i < 8} {incr i 4} {
        restart_server [expr -1*$i] true false
    }
    wait_for_cluster_state ok
    for {set i 0} {$i < 8} {incr i 4} {
        assert_equal [dict get $node_ids $i] [R $i cluster myshardid]
    }
}

test "CLUSTER MYSHARDID reports same shard id after cluster restart" {
    set node_ids {}
    for {set i 0} {$i < 8} {incr i} {
        dict set node_ids $i [R $i cluster myshardid]
    }
    for {set i 0} {$i < 8} {incr i} {
        pause_process [srv [expr -1*$i] pid]
    }
    for {set i 0} {$i < 8} {incr i} {
        restart_server [expr -1*$i] true false
    }
    wait_for_cluster_state ok
    for {set i 0} {$i < 8} {incr i} {
        assert_equal [dict get $node_ids $i] [R $i cluster myshardid]
    }
}

test "CLUSTER SHARDS id response validation" {
    # For each node in the cluster
    for {set i 0} {$i < $::cluster_master_nodes + $::cluster_replica_nodes} {incr i} {
        # Get the CLUSTER SHARDS output from this node
        set shards [R $i CLUSTER SHARDS]
        set seen_shard_ids {}

        # For each shard in the output
        foreach shard $shards {
            set shard_dict [dict create {*}$shard]

            # 1. Verify 'id' key exists
            assert {[dict exists $shard_dict id]}
            set shard_id [dict get $shard_dict id]

            # 2. Verify shard_id is a 40-char string
            assert {[string length $shard_id] == 40}

            # 3. Verify that for a given node's output, all shard IDs are unique
            assert {[dict exists $seen_shard_ids $shard_id] == 0}
            dict set seen_shard_ids $shard_id 1
        }
    }
}

} split_slot_allocation split_slot_replica_allocation

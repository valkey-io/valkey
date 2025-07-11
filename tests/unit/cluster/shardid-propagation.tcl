tags {tls:skip external:skip cluster singledb} {
    set base_conf [list cluster-enabled yes save ""] 
    start_multiple_servers 2 [list overrides $base_conf] {
        test "Cluster nodes are reachable" {
            for {set id 0} {$id < [llength $::servers]} {incr id} {
                # Every node should be reachable.
                wait_for_condition 1000 50 {
                    ([catch {R $id ping} ping_reply] == 0) &&
                    ($ping_reply eq {PONG})
                } else {
                    catch {R $id ping} err
                    fail "Node #$id keeps replying '$err' to PING."
                }
            }
        }

        test "Before slots allocation, all nodes report cluster failure" {
            wait_for_cluster_state fail
        }

        test "Cluster nodes haven't met each other" {
            assert {[llength [get_cluster_nodes 1]] == 1}
            assert {[llength [get_cluster_nodes 0]] == 1}
        }

        test "Allocate slots" {
            cluster_allocate_slots 1 1
        }

        test "Restart of node in cluster mode doesn't cause nodes.conf corruption due to shard id mismatch" {
            set primary_id [R 0 CLUSTER MYID]
            R 1 CLUSTER MEET 127.0.0.1 [srv 0 port]
            set pattern *$primary_id*
            wait_for_condition 100 50 {
                [string match $pattern [R 1 CLUSTER NODES]]
            } else {
                fail "Meet took longer"
            }
            R 1 CLUSTER REPLICATE $primary_id
            wait_replica_online [srv 0 client]
            set result [catch {R 0 DEBUG RESTART 0} err]

            if {$result != 0 && $err ne "I/O error reading reply"} {
                fail "Unexpected error restarting server: $err"
            }

             wait_for_condition 100 100 {
                [check_server_response 0] eq 1
            } else {
                fail "Server didn't come back online in time"
            }
        }
    }
}

start_cluster 3 1 {tags {external:skip cluster}} {
    test "The replica will have a new shard_id after cluster reset soft" {
        assert_equal [R 0 cluster myshardid] [R 3 cluster myshardid]

        R 3 cluster reset
        assert_not_equal [R 0 cluster myshardid] [R 3 cluster myshardid]

        wait_for_condition 1000 50 {
            [llength [R 0 cluster shards]] == 4 &&
            [llength [R 1 cluster shards]] == 4 &&
            [llength [R 2 cluster shards]] == 4
        } else {
            fail "R 3 does not become a new shard"
        }
    }
}

# R0 is Node A (cluster-allow-replica-migration no), R3 is Node B, R4 is Node C
# 1. Node A goes down, and Node B takes over primaryship.
# 2. Node A continues to be down while another Node C is added as a replica of B.
# 3. Node B goes down, and Node C takes over primaryship.
# 4. Node A and Node B come back up and start learning about the topology.
# 5. Node A comes up thinking it was the primary (but has an older config epoch compared to C).
# 6. Node A learns about Node C via gossip and assigns it a random `shard_id`.
# 7. Node A receives a direct ping from Node C.
#    a. Node C advertises the same set of slots that Node A was earlier owning.
#    b. Since Node A assigns a random shard ID to Node C, Node A thinks that it is still a primary and
#       it lost all its slots to Node C, which is in another shard.
# 8. Node A then updates the actual `shard_id` of Node C while processing `shard_id` in ping extensions.
# 9. Node A and Node C end up being primaries in the same shard while Node C continues to own slots.
start_cluster 3 2 {tags {external:skip cluster}} {
    test "Two primaries in the same shard, the one with the smaller config epoch will become the replica" {
        # We make R4 become a fresh new node.
        isolate_node 4

        set CLUSTER_PACKET_TYPE_NONE -1
        set CLUSTER_PACKET_TYPE_ALL -2

        set A 0
        set B 3
        set C 4

        R $A config set cluster-allow-replica-migration no
        R $A debug close-cluster-link-on-packet-drop 1
        R $A debug drop-cluster-packet-filter $CLUSTER_PACKET_TYPE_ALL

        # Wait for B to become a primary.
        pause_process [srv -$A pid]
        R $B cluster failover force
        wait_for_condition 1000 50 {
            [s -$B role] eq {master}
        } else {
            fail "Failed waiting for B to takeover primaryship"
        }

        # Wait for C to become a replica.
        R $C cluster meet 127.0.0.1 [srv -$B port]
        wait_for_condition 50 100 {
            [cluster_get_node_by_id $C [R $B cluster myid]] != {}
        } else {
            fail "Node $C never learned about node $B"
        }
        R $C cluster replicate [R $B cluster myid]
        wait_for_sync [srv -$C client]

        # Wait for C to become a primary.
        pause_process [srv -$B pid]
        R $C cluster failover takeover
        wait_for_condition 1000 50 {
            [s -$C role] eq {master}
        } else {
            fail "Failed waiting for C to become primary"
        }

        # Resume A and B and make sure A drop all the links so that it won't get the pending packets.
        resume_process [srv -$A pid]
        resume_process [srv -$B pid]

        # Ensure that related nodes do not reconnect.
        R 0 debug disable-cluster-reconnection 1
        R 1 debug disable-cluster-reconnection 1
        R 2 debug disable-cluster-reconnection 1
        R 3 debug disable-cluster-reconnection 1
        R 4 debug disable-cluster-reconnection 1

        wait_for_condition 1000 50 {
            [R $A cluster links] eq {}
        } else {
            fail "Failed waiting for A to drop all cluster links"
        }

        R 0 debug disable-cluster-reconnection 0
        R 1 debug disable-cluster-reconnection 0
        R 2 debug disable-cluster-reconnection 0
        R 3 debug disable-cluster-reconnection 0
        R 4 debug disable-cluster-reconnection 0

        # Make sure A and B become the replica.
        R $A debug close-cluster-link-on-packet-drop 0
        R $A debug drop-cluster-packet-filter $CLUSTER_PACKET_TYPE_NONE
        wait_for_condition 1000 50 {
            [s -$A role] eq {slave} &&
            [s -$B role] eq {slave}
        } else {
            fail "Failed waiting for $A and $B to become replicas"
        }
    }
}

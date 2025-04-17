# make sure the test infra won't use SELECT
set old_singledb $::singledb
set ::singledb 1

tags {tls:skip external:skip cluster} {
    set CLUSTER_PACKET_TYPE_PING 0
    set CLUSTER_PACKET_TYPE_PONG 1
    set CLUSTER_PACKET_TYPE_MEET 2
    set CLUSTER_PACKET_TYPE_NONE -1
    set CLUSTER_PACKET_TYPE_ALL -2

    set base_conf [list cluster-enabled yes]
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
            cluster_allocate_slots 2 0
        }

        test "MEET is reliable when target drops the initial MEETs" {
            # Make 0 drop the initial MEET messages due to link failure
            R 0 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_MEET
            R 0 DEBUG CLOSE-CLUSTER-LINK-ON-PACKET-DROP 1

            R 1 CLUSTER MEET 127.0.0.1 [srv 0 port]

            # Wait for at least a few MEETs to be sent so that we are sure that 0 is
            # dropping them.
            wait_for_condition 1000 50 {
                [CI 0 cluster_stats_messages_meet_received] >= 3
            } else {
                fail "Cluster node 1 never sent multiple MEETs to 0"
            }

            # Make sure the nodes still don't know about each other
            # Using a wait condition here as an assert can be flaky - especially
            # when cluster nodes is processed when the link is established to send MEET.
            wait_for_condition 1000 50 {
                [llength [get_cluster_nodes 1 connected]] == 1
            } else {
                fail "Node 1 recognizes node 0 even though node 0 drops MEETs from node 1"
            }
            assert {[llength [get_cluster_nodes 0 connected]] == 1}

            R 0 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_NONE

            # If the MEET is reliable, both a and b will turn to cluster state ok
            wait_for_condition 1000 50 {
                [CI 1 cluster_state] eq {ok} && [CI 0 cluster_state] eq {ok} &&
                [CI 0 cluster_stats_messages_meet_received] >= 4 &&
                [CI 1 cluster_stats_messages_meet_sent] == [CI 0 cluster_stats_messages_meet_received]
            } else {
                fail "Unexpected cluster state: node 1 cluster_state:[CI 1 cluster_state], node 0 cluster_state: [CI 0 cluster_state]"
            }
        }
    } ;# stop servers
} ;# tags

set ::singledb $old_singledb

proc cluster_get_first_node_in_handshake id {
    set nodes [get_cluster_nodes $id]
    foreach n $nodes {
        if {[cluster_has_flag $n handshake]} {
            return [dict get $n id]
        }
    }
    return {}
}

proc cluster_nodes_all_know_each_other {num_nodes} {
    # Collect node IDs dynamically
    set node_ids {}
    for {set i 0} {$i < $num_nodes} {incr i} {
        lappend node_ids [dict get [get_myself $i] id]
    }

    # Check if all nodes know each other
    foreach node_id $node_ids {
        foreach check_node_id $node_ids {
            for {set node_index 0} {$node_index < $num_nodes} {incr node_index} {
                if {[cluster_get_node_by_id $node_index $check_node_id] == {}} {
                    return 0
                }
            }
        }
    }

    # Verify cluster link counts for each node
    set expected_links [expr {2 * ($num_nodes - 1)}]
    for {set i 0} {$i < $num_nodes} {incr i} {
        if {[llength [R $i CLUSTER LINKS]] != $expected_links} {
            return 0
        }
    }

    return 1
}

start_cluster 2 0 {tags {external:skip cluster} overrides {cluster-node-timeout 4000 cluster-replica-no-failover yes}} {
    set CLUSTER_PACKET_TYPE_PING 0
    set CLUSTER_PACKET_TYPE_PONG 1
    set CLUSTER_PACKET_TYPE_MEET 2
    set CLUSTER_PACKET_TYPE_NONE -1
    set CLUSTER_PACKET_TYPE_ALL -2

    test "Handshake eventually succeeds after node handshake timeout on both sides with inconsistent view of the cluster" {
        set cluster_port [find_available_port $::baseport $::portcount]
        start_server [list overrides [list cluster-enabled yes cluster-node-timeout 4000 cluster-port $cluster_port]] {
            # In this test we will trigger a handshake timeout on both sides of the handshake.
            # Node 1 and 2 already know each other, then we make node 1 meet node 0:
            #
            # Node 1 -- MEET -> Node 0 [Node 0 might learn about Node 2 from the gossip section of the msg]
            # Node 1 <- PONG -- Node 0 [we drop this message, so Node 1 will eventually mark the handshake as timed out]
            # Node 1 <- PING -- Node 0 [we drop this message, so Node 1 will never send a PONG and Node 0 will eventually mark the handshake as timed out]
            #
            # After the handshake is timed out, we allow all cluster bus messages to go through.
            # Eventually Node 0 should send a MEET packet to the other nodes to complete the handshake.

            set node0_id [dict get [get_myself 0] id]
            set node1_id [dict get [get_myself 1] id]
            set node2_id [dict get [get_myself 2] id]

            # Drop all cluster bus messages
            R 1 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_ALL
            # Drop MEET cluster bus messages, so that Node 0 cannot start a handshake with Node 2.
            R 2 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_MEET

            R 1 CLUSTER MEET [srv 0 host] [srv 0 port] $cluster_port

            # Wait for Node 0 to be in handshake
            wait_for_condition 10 400 {
                [cluster_get_first_node_in_handshake 0] != {}
            } else {
                fail "Node 0 never entered handshake state"
            }

            # We want Node 0 to learn about Node 2 through the gossip section of the MEET message
            set meet_retry 0
            while {[cluster_get_node_by_id 0 $node2_id] eq {}} {
                if {$meet_retry == 10} {
                    error "assertion: Retried to meet Node 0 too many times"
                }
                # If Node 0 doesn't know about Node 1 & 2, it means Node 1 did not gossip about node 2 in its MEET message.
                # So we kill the outbound link from Node 1 to Node 0, to force a reconnect and a re-send of the MEET message.
                after 100
                # Since we are in handshake, we use a randomly generated ID we have to find
                R 1 DEBUG CLUSTERLINK KILL ALL [cluster_get_first_node_in_handshake 1]
                incr meet_retry 1
            }

            # Wait for Node 1's handshake to timeout
            wait_for_condition 50 100 {
                [cluster_get_first_node_in_handshake 1] eq {}
            } else {
                fail "Node 1 never exited handshake state"
            }

            # Wait for Node 0's handshake to timeout
            wait_for_condition 50 100 {
                [cluster_get_first_node_in_handshake 0] eq {}
            } else {
                fail "Node 0 never exited handshake state"
            }

            # At this point Node 0 knows Node 2 through the gossip, but Node 1 & 2 don't know Node 0.
            wait_for_condition 50 100 {
                [cluster_get_node_by_id 0 $node2_id] != {} &&
                [cluster_get_node_by_id 1 $node0_id] eq {} &&
                [cluster_get_node_by_id 2 $node0_id] eq {}
            } else {
                fail "Unexpected CLUSTER NODES output, nodes 1 & 2 should not know node 0."
            }

            # Allow all messages to go through again
            R 1 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_NONE
            R 2 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_NONE

            # Now Node 0 will send a MEET packet to Node 1 & 2 since it has an outbound link to these nodes but no inbound link.
            # Handshake should now complete successfully.
            wait_for_condition 50 200 {
                [cluster_nodes_all_know_each_other 3]
            } else {
                fail "Unexpected CLUSTER NODES output, all nodes should know each other."
            }
        } ;# stop Node 0
    } ;# test
} ;# stop cluster

start_cluster 2 0 {tags {external:skip cluster} overrides {cluster-node-timeout 4000 cluster-replica-no-failover yes}} {
    set CLUSTER_PACKET_TYPE_PING 0
    set CLUSTER_PACKET_TYPE_PONG 1
    set CLUSTER_PACKET_TYPE_MEET 2
    set CLUSTER_PACKET_TYPE_NONE -1
    set CLUSTER_PACKET_TYPE_ALL -2

    test "Handshake eventually succeeds after node handshake timeout on one side with inconsistent view of the cluster" {
        set cluster_port [find_available_port $::baseport $::portcount]
        start_server [list overrides [list cluster-enabled yes cluster-node-timeout 4000 cluster-port $cluster_port]] {
            # In this test we will trigger a handshake timeout on one side of the handshake.
            # Node 1 and 2 already know each other, then we make node 0 meet node 1:
            #
            # Node 0 -- MEET -> Node 1
            # Node 0 <- PONG -- Node 1
            # Node 0 <- PING -- Node 1 [Node 0 will mark the handshake as successful]
            # Node 0 -- PONG -> Node 1 [we drop this message, so node 1 will eventually mark the handshake as timed out]
            #
            # After the handshake is timed out, we allow all cluster bus messages to go through.
            # Eventually Node 0 should send a MEET packet to the other nodes to complete the handshake.

            set node0_id [dict get [get_myself 0] id]
            set node1_id [dict get [get_myself 1] id]
            set node2_id [dict get [get_myself 2] id]

            # Drop PONG messages
            R 1 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_PONG
            # Drop MEET cluster bus messages, so that Node 0 cannot start a handshake with Node 2.
            R 2 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_MEET

            # Node 0 meets node 1
            R 0 CLUSTER MEET [srv -1 host] [srv -1 port]

            # Wait for node 0 to know about the other nodes in the cluster
            wait_for_condition 50 100 {
                [cluster_get_node_by_id 0 $node1_id] != {}
            } else {
                fail "Node 0 never learned about node 1"
            }
            # At this point, node 0 knows about node 1 and might know node 2 if node 1 gossiped about it.
            wait_for_condition 50 100 {
                [cluster_get_first_node_in_handshake 0] eq {}
            } else {
                fail "Node 1 never exited handshake state"
            }
            # At this point, from node 0 point of view, the handshake with node 1 succeeded.

            wait_for_condition 50 100 {
                [cluster_get_first_node_in_handshake 1] eq {}
            } else {
                fail "Node 1 never exited handshake state"
            }
            assert {[cluster_get_node_by_id 1 $node0_id] eq {}}
            # At this point, from node 1 point of view, the handshake with node 0 timed out.

            # Allow all messages
            R 1 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_NONE
            R 2 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_NONE

            # Now Node 0 will send a MEET packet to Node 1 & 2 since it has an outbound link to these nodes but no inblound link.
            # Handshake should now complete successfully.
            wait_for_condition 50 200 {
                [cluster_nodes_all_know_each_other 3]
            } else {
                fail "Unexpected CLUSTER NODES output, all nodes should know each other."
            }
        } ;# stop Node 0
    } ;# test
} ;# stop cluster

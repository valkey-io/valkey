proc cluster_allocate_mixedSlots {masters replicas} {
    set slot 16383
    while {$slot >= 0} {
        set node [expr {$slot % $masters}]
        lappend slots_$node $slot
        incr slot -1
    }
    for {set j 0} {$j < $masters} {incr j} {
        R $j cluster addslots {*}[set slots_${j}]
    }
}

start_cluster 5 10 {overrides {cluster-ping-interval 1000 cluster-node-timeout 90000} tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test [srv 0 port]
}

test "Instance #5 is a slave" {
    assert {[s -5 role] eq {slave}}
}

test "client do not break when cluster slot" {
    R 0 config set client-output-buffer-limit "normal 33554432 16777216 60"
    if { [catch {R 0 cluster slots}] } {
        fail "output overflow when cluster slots"
    }
}

test "client can handle keys with hash tag" {
    set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]
    $cluster set foo{tag} bar
    $cluster close
}

test "slot migration is valid from primary to another primary" {
    set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]
    set key order1
    set slot [$cluster cluster keyslot $key]
    array set nodefrom [$cluster masternode_for_slot $slot]
    array set nodeto [$cluster masternode_notfor_slot $slot]

    assert_equal {OK} [$nodefrom(link) cluster setslot $slot node $nodeto(id)]
    assert_equal {OK} [$nodeto(link) cluster setslot $slot node $nodeto(id)]
}

test "slot migration is invalid from primary to replica" {
    set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]
    set key order1
    set slot [$cluster cluster keyslot $key]
    array set nodefrom [$cluster masternode_for_slot $slot]

    # Get replica node serving slot.
    set replicanodeinfo [$cluster cluster replicas $nodefrom(id)]
    set args [split $replicanodeinfo " "]
    set replicaid [lindex [split [lindex $args 0] \{] 1]

    catch {[$nodefrom(link) cluster setslot $slot node $replicaid]} err
    assert_match "*Target node is not a master" $err
}

proc count_bound_slots {n} {
     set slot_count 0
     foreach slot_range_mapping [$n cluster slots] {
         set start_slot [lindex $slot_range_mapping 0]
         set end_slot [lindex $slot_range_mapping 1]
         incr slot_count [expr $end_slot - $start_slot + 1]
     }
     return $slot_count
 }

 test "slot must be unbound on the owner when it is deleted" {
     set node0 [Rn 0]
     set node1 [Rn 1]
     assert {[count_bound_slots $node0] eq 16384}
     assert {[count_bound_slots $node1] eq 16384}

     set slot_to_delete 0
     # Delete
     $node0 CLUSTER DELSLOTS $slot_to_delete

     # Verify
     # The node that owns the slot must unbind the slot that was deleted
     wait_for_condition 1000 50 {
         [count_bound_slots $node0] == 16383
     } else {
         fail "Cluster slot deletion was not recorded on the node that owns the slot"
     }

     # We don't propagate slot deletion across all nodes in the cluster.
     # This can lead to extra redirect before the clients find out that the slot is unbound.
     wait_for_condition 1000 50 {
         [count_bound_slots $node1] == 16384
     } else {
         fail "Cluster slot deletion should not be propagated to all nodes in the cluster"
     }
 }

if {$::tls} {
    test {CLUSTER SLOTS from non-TLS client in TLS cluster} {
        set slots_tls [R 0 cluster slots]
        set host [srv 0 host]
        set plaintext_port [srv 0 pport]
        set client_plain [valkey $host $plaintext_port 0 0]
        set slots_plain [$client_plain cluster slots]
        $client_plain close
        # Compare the ports in the first row
        assert_no_match [lindex $slots_tls 0 3 1] [lindex $slots_plain 0 3 1]
    }
}

} cluster_allocate_mixedSlots cluster_allocate_replicas ;# start_cluster

# Returns the id of the first node in handshake that instance `id` knows, or
# an empty string when there is none.
proc cluster_handshake_node_id {id} {
    foreach n [get_cluster_nodes $id] {
        if {[cluster_has_flag $n handshake]} {return [dict get $n id]}
    }
    return ""
}

# Returns the first slot in the CLUSTER NODES slots field of `node`.
proc cluster_node_first_slot {node} {
    return [lindex [split [lindex [dict get $node slots] 0] -] 0]
}

# Returns the id of the primary that owns slot `slot` in instance `id`'s
# CLUSTER SLOTS reply, or an empty string when the slot is unassigned.
proc cluster_slot_owner_id {id slot} {
    foreach range [R $id cluster slots] {
        if {$slot >= [lindex $range 0] && $slot <= [lindex $range 1]} {
            return [lindex $range 2 2]
        }
    }
    return ""
}

# A single node that owns slot 0 only. cluster-node-timeout 5000 keeps the
# handshake node alive well past the rejections below, even on a slow or
# loaded worker, and still lets it time out inside the test.
start_server {tags {external:skip cluster singledb} overrides {cluster-enabled yes cluster-node-timeout 5000}} {
    test {CLUSTER SETSLOT rejects a target node that is still in handshake} {
        set myid [R 0 cluster myid]
        R 0 cluster addslots 0

        # MEET a port where nothing listens. The node is created at once with
        # a placeholder id and the handshake flag. The bus port is set to the
        # same unused port so that no other cluster bus can answer.
        set dead_port [find_available_port $::baseport $::portcount]
        R 0 cluster meet 127.0.0.1 $dead_port $dead_port
        wait_for_condition 50 100 {
            [cluster_handshake_node_id 0] ne ""
        } else {
            fail "handshake node was never created"
        }
        set hs [cluster_handshake_node_id 0]

        set err {ERR Target node is still in handshake or has no known address*}
        assert_error $err {R 0 cluster setslot 0 node $hs}
        assert_error $err {R 0 cluster setslot 1 node $hs}
        assert_error $err {R 0 cluster setslot 0 migrating $hs}
        assert_error $err {R 0 cluster setslot 1 importing $hs}

        # Nothing changed: slot 0 is still mine and I am still a primary.
        assert_equal $myid [cluster_slot_owner_id 0 0]
        assert {[cluster_has_flag [cluster_get_myself 0] master]}

        # When the handshake node finally times out and is deleted, the slot
        # table and its cached CLUSTER SLOTS reply are unchanged. On the base
        # (where the SETSLOT NODE above returned OK) this is the point the
        # cached reply named a deleted node and CLUSTER SLOTS crashed.
        set slots_before [R 0 cluster slots]
        wait_for_condition 200 100 {
            [cluster_handshake_node_id 0] eq ""
        } else {
            fail "handshake node was never deleted"
        }
        assert_equal $slots_before [R 0 cluster slots]
    }
}

start_cluster 2 0 {tags {external:skip cluster}} {
    test {CLUSTER SETSLOT rejects a target node that has no address} {
        set other_id [R 1 cluster myid]
        set my_slot [cluster_node_first_slot [cluster_get_myself 0]]
        set other_slot [cluster_node_first_slot [cluster_get_node_by_id 0 $other_id]]

        # After CLUSTER RESET HARD the other node has a new id. The first PONG
        # with the new id makes us mark the old id as NOADDR.
        R 1 cluster reset hard
        wait_for_condition 1000 10 {
            [cluster_has_flag [cluster_get_node_by_id 0 $other_id] noaddr]
        } else {
            fail "the old id of the reset node was never marked NOADDR"
        }

        set err {ERR Target node is still in handshake or has no known address*}
        assert_error $err {R 0 cluster setslot $my_slot node $other_id}
        assert_error $err {R 0 cluster setslot $my_slot migrating $other_id}
        assert_error $err {R 0 cluster setslot $other_slot importing $other_id}
    }
}

# A primary must not reject a SETSLOT replicated from its own primary just
# because its local view of the target differs. Here the replica marks the
# target NOADDR while the primary still sees it as a normal primary, so the
# primary accepts and replicates the command. If the replica rejected it, the
# primary would apply the slot change and the replica would not, splitting the
# shard's view of slot ownership.
start_cluster 2 1 {tags {external:skip cluster} overrides {cluster-node-timeout 3000}} {
    set CLUSTER_PACKET_TYPE_PONG 1
    set CLUSTER_PACKET_TYPE_NONE -1

    test {A replica applies a SETSLOT replicated to a target it sees as NOADDR} {
        # Node 0 is a primary that owns slot 0, node 2 is its replica, node 1
        # is the SETSLOT target (another primary).
        assert {[s -2 role] eq {slave}}
        wait_for_condition 1000 50 {
            [s -2 master_link_status] eq {up}
        } else {
            fail "replica link to its primary is not up"
        }
        set target_id [R 1 cluster myid]
        assert_equal [R 0 cluster myid] [cluster_slot_owner_id 0 0]

        # Make node 0 drop every PONG. Node 1 then does CLUSTER RESET HARD and
        # comes back with a new id. Node 2 learns the mismatch on its own link
        # and marks the old id NOADDR, but node 0 never processes the PONG that
        # would tell it, so node 0 keeps the old id as a normal addressed node.
        R 0 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_PONG
        R 1 cluster reset hard
        wait_for_condition 1000 10 {
            [cluster_has_flag [cluster_get_node_by_id 2 $target_id] noaddr]
        } else {
            fail "the replica never marked the target NOADDR"
        }
        assert {![cluster_has_flag [cluster_get_node_by_id 0 $target_id] noaddr]}

        # Node 0 accepts the command (its view of the target is not pending),
        # replicates it to node 2 and waits for the acknowledgement. Node 2 must
        # apply the same slot assignment instead of rejecting it.
        assert_equal {OK} [R 0 cluster setslot 0 node $target_id]
        wait_for_condition 1000 50 {
            [cluster_slot_owner_id 2 0] eq $target_id
        } else {
            fail "replica did not apply the replicated SETSLOT (slot 0 owner is\
                  [cluster_slot_owner_id 2 0], expected $target_id)"
        }

        R 0 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_NONE
    }
}

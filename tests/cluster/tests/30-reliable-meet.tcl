set do_not_join_cluster_nodes 1

source "../tests/includes/init-tests.tcl"

# Create a cluster composed of the specified number of primaries.
proc create_primaries_only_cluster {primaries} {
    cluster_allocate_slots $primaries
    set ::cluster_master_nodes $primaries
}

set CLUSTER_PACKET_TYPE_MEET 2
set CLUSTER_PACKET_TYPE_NONE -1

set a 0
set b 1

test "Cluster nodes haven't met each other" {
    assert {[llength [get_cluster_nodes $a]] == 1}
    assert {[llength [get_cluster_nodes $b]] == 1}
}

test "Create a 2 nodes cluster with 2 shards" {
    create_primaries_only_cluster 2
}

test "MEET is reliabile when target drops the initial MEETs" {
    set b_port [get_instance_attrib valkey $b port]

    # Make b drop the initial MEET messages due to link failure
    R $b DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_MEET
    R $b DEBUG CLOSE-CLUSTER-LINK-ON-PACKET-DROP 1

    R $a CLUSTER MEET 127.0.0.1 $b_port

    wait_for_condition 1000 50 {
        [CI $b cluster_stats_messages_meet_received] >= 3
    } else {
      fail "Cluster node $a never sent multiple MEETs to $b"
    }

    # Make sure the nodes still don't know about each other
    assert {[llength [get_cluster_nodes $a connected]] == 1}
    assert {[llength [get_cluster_nodes $b connected]] == 1}

    R $b DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_NONE

    # If the MEET is reliable, both a and b will turn to cluster state ok
    wait_for_condition 1000 50 {
        [CI $a cluster_state] eq {ok} && [CI $b cluster_state] eq {ok}
    } else {
        fail "$a cluster_state:[CI $a cluster_state], $b cluster_state: [CI $b cluster_state]"
    }
}


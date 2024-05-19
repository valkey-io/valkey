# make sure the test infra won't use SELECT
set old_singledb $::singledb
set ::singledb 1

tags {tls:skip external:skip cluster} {

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

set CLUSTER_PACKET_TYPE_MEET 2
set CLUSTER_PACKET_TYPE_NONE -1

set b 0
set a 1

test "Cluster nodes haven't met each other" {
    assert {[llength [get_cluster_nodes $a]] == 1}
    assert {[llength [get_cluster_nodes $b]] == 1}
}

test "Allocate slots" {
    cluster_allocate_slots 2 0
}

test "MEET is reliabile when target drops the initial MEETs" {
    # Make b drop the initial MEET messages due to link failure
    R $b DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_MEET
    R $b DEBUG CLOSE-CLUSTER-LINK-ON-PACKET-DROP 1

    set b_port [srv 0 port]

    R $a CLUSTER MEET 127.0.0.1 $b_port

    # Wait for at least a few MEETs to be sent so that we are sure that b is
    # dropping them.
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
        [CI $a cluster_state] eq {ok} && [CI $b cluster_state] eq {ok} &&
        [CI $b cluster_stats_messages_meet_received] >= 4 &&
        [CI $a cluster_stats_messages_meet_sent] == [CI $b cluster_stats_messages_meet_received]
    } else {
        fail "$a cluster_state:[CI $a cluster_state], $b cluster_state: [CI $b cluster_state]"
    }
}
} ;# stop servers

} ;# tags

set ::singledb $old_singledb


proc leave_last_slot_unassigned {masters} {
    R 0 CLUSTER ADDSLOTSRANGE 0 16382
}

start_cluster 1 1 {tags {external:skip cluster} overrides {cluster-require-full-coverage no}} {
    test "Replica whose primary was deleted survives a primary claiming an unassigned slot" {
        # Replicate a node that is still in handshake. When the handshake
        # times out the node is deleted and R 1 is left with no primary.
        set dead_port [find_available_port $::baseport $::portcount]
        R 1 CLUSTER MEET 127.0.0.1 $dead_port
        foreach n [get_cluster_nodes 1] {
            if {[cluster_has_flag $n handshake]} {
                R 1 CLUSTER REPLICATE [dict get $n id]
            }
        }
        wait_for_condition 100 100 {
            [llength [get_cluster_nodes 1]] == 2
        } else {
            fail "Handshake node was not deleted"
        }
        assert_equal "-" [dict get [cluster_get_myself 1] slaveof]

        R 0 CLUSTER ADDSLOTS 16383
        wait_for_condition 100 100 {
            [lindex [R 1 CLUSTER SLOTS] 0 1] == 16383
        } else {
            fail "R 1 did not learn the new slot"
        }
        assert_equal PONG [R 1 PING]
    }
} leave_last_slot_unassigned

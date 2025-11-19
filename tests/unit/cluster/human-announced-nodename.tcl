# Check if cluster's view of human announced nodename is reported in logs
start_cluster 4 0 {tags {external:skip cluster}} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        R $j config set loglevel debug
        R $j config set loglevel debug
    }

    set RO_node_id [dict get [cluster_get_myself 0] id]
    set R0_port [srv 0 port]
    set R1_port [srv -1 port]
    set R2_port [srv -2 port]
    set R3_port [srv -3 port]

    test "Nodes have default human nodenames when not explicitly set" {
        wait_for_log_messages 0 [list "*Sending ping packet to node * (127.0.0.1:$R1_port) *"] 0 1000 10
        wait_for_log_messages 0 [list "*Sending ping packet to node * (127.0.0.1:$R2_port) *"] 0 1000 10
        wait_for_log_messages 0 [list "*Sending ping packet to node * (127.0.0.1:$R3_port) *"] 0 1000 10
        wait_for_log_messages -1 [list "*Sending ping packet to node $RO_node_id (127.0.0.1:$R0_port) *"] 0 1000 10
        wait_for_log_messages -2 [list "*Sending ping packet to node $RO_node_id (127.0.0.1:$R0_port) *"] 0 1000 10
        wait_for_log_messages -3 [list "*Sending ping packet to node $RO_node_id (127.0.0.1:$R0_port) *"] 0 1000 10
    }

    test "Set cluster human announced nodename and let it propagate" {
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            R $j config set cluster-announce-hostname "host-$j.com"
            R $j config set cluster-announce-human-nodename "nodename-$j"
        }

        # We wait for everyone to agree on the hostnames. Since they are gossiped
        # the same way as nodenames, it implies everyone knows the nodenames too.
        wait_for_condition 50 100 {
            [are_hostnames_propagated "host-*.com"] eq 1
        } else {
            fail "cluster hostnames were not propagated"
        }

        # Ensure the human nodenames are visible in logs
        wait_for_log_messages 0 [list "*Sending ping packet to node * (nodename-1) *"] 0 1000 10
        wait_for_log_messages 0 [list "*Sending ping packet to node * (nodename-2) *"] 0 1000 10
        wait_for_log_messages 0 [list "*Sending ping packet to node * (nodename-3) *"] 0 1000 10
        set R1_log [wait_for_log_messages -1 [list "*Sending ping packet to node $RO_node_id (nodename-0) *"] 0 1000 10]
        set R2_log [wait_for_log_messages -2 [list "*Sending ping packet to node $RO_node_id (nodename-0) *"] 0 1000 10]
        set R3_log [wait_for_log_messages -3 [list "*Sending ping packet to node $RO_node_id (nodename-0) *"] 0 1000 10]
        # These line numbers will be later used as starting position to scan for newer logs
        set R1_log_line [lindex $R1_log 1]
        set R2_log_line [lindex $R2_log 1]
        set R3_log_line [lindex $R3_log 1]
    }

    test "User can reset human nodename to empty string" {
        R 0 config set cluster-announce-human-nodename ""
        wait_for_log_messages -1 [list "*Sending ping packet to node $RO_node_id () *"] $R1_log_line 1000 10
        wait_for_log_messages -2 [list "*Sending ping packet to node $RO_node_id () *"] $R2_log_line 1000 10
        wait_for_log_messages -3 [list "*Sending ping packet to node $RO_node_id () *"] $R3_log_line 1000 10
    }
}

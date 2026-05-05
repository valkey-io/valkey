start_cluster 2 2 {tags {external:skip cluster}} {

    test "Test change cluster-announce-port and cluster-announce-tls-port at runtime" {
        if {$::tls} {
            set baseport [lindex [R 0 config get tls-port] 1]
        } else {
            set baseport [lindex [R 0 config get port] 1]
        }
        set count [expr [llength $::servers] + 1]
        set used_port [find_available_port $baseport $count]

        # We execute CLUSTER SLOTS command to trigger the `debugServerAssertWithInfo` in `clusterCommandSlots` function, ensuring
        # that the cached response is invalidated upon updating any of cluster-announce-tls-port or cluster-announce-port.
        R 0 CLUSTER SLOTS
        R 1 CLUSTER SLOTS

        R 0 config set cluster-announce-tls-port $used_port
        R 0 config set cluster-announce-port $used_port

        assert_match "*:$used_port@*" [R 0 CLUSTER NODES]
        assert_match "*$used_port*" [R 0 CLUSTER SLOTS]
        wait_for_condition 50 100 {
            ([string match "*:$used_port@*" [R 1 CLUSTER NODES]] && [string match "*$used_port*" [R 1 CLUSTER SLOTS]])
        } else {
            fail "Cluster announced port was not propagated via gossip"
        }

        R 0 config set cluster-announce-tls-port 0
        R 0 config set cluster-announce-port 0
        assert_match "*:$baseport@*" [R 0 CLUSTER NODES]
    }

    test "Test change cluster-announce-bus-port at runtime" {
        if {$::tls} {
            set baseport [lindex [R 0 config get tls-port] 1]
        } else {
            set baseport [lindex [R 0 config get port] 1]
        }
        set count [expr [llength $::servers] + 1]
        set used_port [find_available_port $baseport $count]

        # Verify config set cluster-announce-bus-port
        R 0 config set cluster-announce-bus-port $used_port
        assert_match "*@$used_port *" [R 0 CLUSTER NODES]
        wait_for_condition 50 100 {
            [string match "*@$used_port *" [R 1 CLUSTER NODES]]
        } else {
            fail "Cluster announced port was not propagated via gossip"
        }

        # Verify restore default cluster-announce-port
        set base_bus_port [expr $baseport + 10000]
        R 0 config set cluster-announce-bus-port 0
        assert_match "*@$base_bus_port *" [R 0 CLUSTER NODES]
    }

    test "Test change port and tls-port on runtime" {
        if {$::tls} {
            set baseport [lindex [R 0 config get tls-port] 1]
        } else {
            set baseport [lindex [R 0 config get port] 1]
        }
        set count [expr [llength $::servers] + 1]
        set used_port [find_available_port $baseport $count]

        # We execute CLUSTER SLOTS command to trigger the `debugServerAssertWithInfo` in `clusterCommandSlots` function, ensuring
        # that the cached response is invalidated upon updating any of port or tls-port.
        R 0 CLUSTER SLOTS
        R 1 CLUSTER SLOTS

        # Set port or tls-port to ensure changes are consistent across the cluster.
        if {$::tls} {
            R 0 config set tls-port $used_port
        } else {
            R 0 config set port $used_port
        }
        # Make sure changes in myself node's view are consistent.
        assert_match "*:$used_port@*" [R 0 CLUSTER NODES]
        assert_match "*$used_port*" [R 0 CLUSTER SLOTS]
        # Make sure changes in other node's view are consistent.
        wait_for_condition 50 100 {
            [string match "*:$used_port@*" [R 1 CLUSTER NODES]] &&
            [string match "*$used_port*" [R 1 CLUSTER SLOTS]]
        } else {
            fail "Node port was not propagated via gossip"
        }

        # Restore the original configuration item value.
        if {$::tls} {
            R 0 config set tls-port $baseport
        } else {
            R 0 config set port $baseport
        }
    }
}

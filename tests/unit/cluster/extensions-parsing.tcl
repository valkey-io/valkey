
proc is_hostname_propagated {start_node end_node match_string} {
    set cnt 0
    for {set j $start_node} {$j < $end_node} {incr j} {
        set cfg [R $j cluster slots]
        foreach node $cfg {
            for {set i 2} {$i < [llength $node]} {incr i} {
                if {[string match $match_string [lindex [lindex [lindex $node $i] 3] 1]] } {
                    incr cnt
                }
            }
        }
    }
    return $cnt
}

start_cluster 2 2 {tags {external:skip cluster}} {

    test "Cluster state should be healthy" {
        wait_for_cluster_state "ok"
    }

    test "Set cluster hostnames on primary of shard 0 and verify they are propagated after enabling extensions processing" {
        R 0 config set cluster-announce-hostname "host-0.com"

        # Verify all nodes have received the hostname for host 0 via cluster message extensions.
        wait_for_condition 50 100 {
            [is_hostname_propagated 0 4 "host-0.com"] eq [llength $::servers]
        } else {
            fail "cluster hostname for primary 0 were not propagated to all nodes"
        }

        R 1 debug send-clustermsg-extensions 0
        R 1 config set cluster-announce-hostname "host-1.com"

        # Verify only self (node) has received the hostname.
        wait_for_condition 50 100 {
            [is_hostname_propagated 0 4 "host-1.com"] eq 1
        } else {
            fail "cluster hostname for primary 1 was propagated to other nodes"
        }

        R 1 debug send-clustermsg-extensions 1

        # Verify all nodes have received the hostname for host 1 via cluster message extensions.
        wait_for_condition 50 100 {
            [is_hostname_propagated 0 4 "host-1.com"] eq [llength $::servers]
        } else {
            fail "cluster hostname for primary 1 were not propagated to all nodes"
        }

        # Now that everything is propagated, assert everyone agrees
        wait_for_cluster_propagation
    } {} {needs:debug}
}


proc are_hostnames_propagated_by_index {start_node end_node match_string} {
    for {set j $start_node} {$j < $end_node} {incr j} {
        set cfg [R $j cluster slots]
        foreach node $cfg {
            for {set i 2} {$i < [llength $node]} {incr i} {
                if {! [string match $match_string [lindex [lindex [lindex $node $i] 3] 1]] } {
                    return 0
                }
            }
        }
    }
    return 1
}

start_cluster 2 2 {tags {external:skip cluster}} {

    test "Cluster state should be healthy" {
        wait_for_cluster_state "ok"
    }

    test "Disable cluster extensions handling" {
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            R $j DEBUG process-clustermsg-extensions 0
        }
    } {} {needs:debug}

    test "Cluster state should be healthy" {
        wait_for_cluster_state "ok"
    }

    test "Set cluster hostnames on primary and verify they are propagated after enabling extensions processing" {
        # Enable cluster message processing on primary nodes and set hostname(s).
        for {set j 0} {$j < 2} {incr j} {
            R $j DEBUG process-clustermsg-extensions 1
            R $j config set cluster-announce-hostname "host-$j.com"
        }
        

        # Verify primary nodes have received the hostname via cluster message extensions.
        wait_for_condition 50 100 {
            [are_hostnames_propagated_by_index 0 2 "host-*.com"] eq 1
        } else {
            fail "cluster hostnames were not propagated to primary"
        }

        # Verify replica nodes have not received the hostname.
        wait_for_condition 50 100 {
            [are_hostnames_propagated_by_index 2 4 "host-*.com"] eq 0
        } else {
            fail "cluster hostnames were propagated to replicas"
        }


        # Enable cluster message extensions processing on all nodes and set hostname(s).
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            R $j DEBUG process-clustermsg-extensions 1
            R $j config set cluster-announce-hostname "host-$j.com"
        }

        # Verify all nodes have received the hostname.
        wait_for_condition 50 100 {
            [are_hostnames_propagated "host-*.com"] eq 1
        } else {
            fail "cluster hostnames were not propagated to all nodes"
        }

        # Now that everything is propagated, assert everyone agrees
        wait_for_cluster_propagation
    } {} {needs:debug}
}

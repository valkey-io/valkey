# Verify socket prioritization in Cluster mode (with and without TLS)
start_cluster 2 2 {tags {socket-prioritization external:skip cluster}} {
    test "Cluster is up and running" {
        wait_for_cluster_state ok
    }

    test "Verify CLIENT LIST qos filters for replica links" {
        set high_clients [R 0 client list flags H]
        assert_match "*flags=*H*" $high_clients

        set normal_cl [valkey 127.0.0.1 [srv 0 port] 0 $::tls]
        $normal_cl client setname clusternorm

        assert_match "*name=clusternorm*flags=N*" [R 0 client list not-flags H name clusternorm]
        assert_equal "" [R 0 client list flags H name clusternorm]
        $normal_cl close
    }

    test "Verify cluster slot synchronization under pipeline load" {
        wait_for_cluster_state ok

        set load_clients {}
        foreach idx {0 1} {
            set port [srv [expr -1*$idx] port]
            for {set c 0} {$c < 3} {incr c} {
                lappend load_clients [valkey 127.0.0.1 $port 0 $::tls]
            }
        }

        set val [string repeat "y" 128]
        set total_ops 0
        for {set iter 0} {$iter < 5} {incr iter} {
            foreach cl $load_clients {
                catch {
                    $cl write "*3\r\n\$3\r\nSET\r\n\$7\r\npipekey\r\n\$128\r\n$val\r\n"
                    $cl flush
                    $cl read
                    incr total_ops 1
                }
            }
        }

        assert {$total_ops > 0}

        wait_for_condition 100 50 {
            [R 0 dbsize] + [R 1 dbsize] == 1 &&
            [R 2 dbsize] == [R 0 dbsize] &&
            [R 3 dbsize] == [R 1 dbsize]
        } else {
            fail "Replicas failed to complete sync during pipelined load"
        }

        foreach cl $load_clients { catch { $cl close } }
    }

    proc local_slot_ranges_contains_slot {slot_ranges slot} {
        set ranges [split $slot_ranges " "]
        foreach slot_range $ranges {
            lassign [split $slot_range -] start end
            if {$end == {}} {set end $start}
            if {$slot >= $start && $slot <= $end} {
                return 1
            }
        }
        return 0
    }

    proc local_is_slot_migrated {node_idx slot} {
        set target_id [R $node_idx CLUSTER MYID]
        set nodes [get_cluster_nodes $node_idx]
        foreach n $nodes {
            set node_id [dict get $n id]
            if {$node_id eq $target_id} {
                set slot_ranges [dict get $n slots]
                if {[local_slot_ranges_contains_slot $slot_ranges $slot]} {
                    return 1
                }
            }
        }
        return 0
    }

    proc check_prioritized_client_count {node_idx expected_min} {
        set prioritized [string trim [R $node_idx client list flags H]]
        if {$prioritized eq ""} {
            set count 0
        } else {
            set count [llength [split $prioritized "\n"]]
        }
        return [expr {$count > $expected_min}]
    }

    test "Verify CLUSTER MIGRATESLOTS connection is prioritized" {
        set target_id [R 1 CLUSTER MYID]
        set prioritized_before [string trim [R 1 client list flags H]]
        if {$prioritized_before eq ""} {
            set expected_min 0
        } else {
            set expected_min [llength [split $prioritized_before "\n"]]
        }
        
        R 1 DEBUG slotmigration prevent-failover 1
        
        assert_equal "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 NODE $target_id]
        
        wait_for_condition 100 50 {
            [check_prioritized_client_count 1 $expected_min]
        } else {
            puts "Before: $prioritized_before"
            puts "After: [R 1 client list flags H]"
            fail "Migration connection did not appear on target as prioritized"
        }
        
        R 1 DEBUG slotmigration prevent-failover 0
        
        wait_for_condition 100 50 {
            [local_is_slot_migrated 1 0]
        } else {
            fail "Slot 0 was not migrated to R1"
        }
    }

    test "Verify cluster nodes connections are prioritized from beginning" {
        set links [R 0 CLUSTER LINKS]
        assert {[llength $links] > 0}
        foreach link $links {
            assert_equal "prioritized" [dict get $link qos]
        }
    }
}

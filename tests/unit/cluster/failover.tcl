# Check the basic monitoring and failover capabilities.

start_cluster 5 5 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test [srv 0 port]
}

test "Instance #5 is a slave" {
    assert {[s -5 role] eq {slave}}
}

test "Instance #5 synced with the master" {
    wait_for_condition 1000 50 {
        [s -5 master_link_status] eq {up}
    } else {
        fail "Instance #5 master link status is not up"
    }
}

set current_epoch [CI 1 cluster_current_epoch]

set paused_pid [srv 0 pid]
test "Killing one master node" {
    pause_process $paused_pid
}

test "Wait for failover" {
    wait_for_condition 1000 50 {
        [CI 1 cluster_current_epoch] > $current_epoch
    } else {
        fail "No failover detected"
    }
}

test "Cluster should eventually be up again" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {[process_is_paused [srv -$j pid]]} continue
        wait_for_condition 1000 50 {
            [CI $j cluster_state] eq "ok"
        } else {
            fail "Cluster node $j cluster_state:[CI $j cluster_state]"
        }
    }
}

test "Cluster is writable" {
    cluster_write_test [srv -1 port]
}

test "Instance #5 is now a master" {
    assert {[s -5 role] eq {master}}
}

test "Restarting the previously killed master node" {
    resume_process $paused_pid
}

test "Instance #0 gets converted into a slave" {
    wait_for_condition 1000 50 {
        [s 0 role] eq {slave}
    } else {
        fail "Old master was not converted into slave"
    }
    wait_for_cluster_propagation
}

} ;# start_cluster

start_cluster 3 6 {tags {external:skip cluster}} {

    test "Cluster is up" {
        wait_for_cluster_state ok
    }

    test "Cluster is writable" {
        cluster_write_test [srv 0 port]
    }

    set current_epoch [CI 1 cluster_current_epoch]

    set paused_pid [srv 0 pid]
    test "Killing the first primary node" {
        pause_process $paused_pid
    }

    test "Wait for failover" {
        wait_for_condition 1000 50 {
            [CI 1 cluster_current_epoch] > $current_epoch
        } else {
            fail "No failover detected"
        }
    }

    test "Cluster should eventually be up again" {
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            if {[process_is_paused [srv -$j pid]]} continue
            wait_for_condition 1000 50 {
                [CI $j cluster_state] eq "ok"
            } else {
                fail "Cluster node $j cluster_state:[CI $j cluster_state]"
            }
        }
    }

    test "Restarting the previously killed primary node" {
        resume_process $paused_pid
    }

    test "Instance #0 gets converted into a replica" {
        wait_for_condition 1000 50 {
            [s 0 role] eq {slave}
        } else {
            fail "Old primary was not converted into replica"
        }
        wait_for_cluster_propagation
    }

    test "Make sure the replicas always get different ranks" {
        set log3 [exec cat [srv -3 stdout]]
        set log6 [exec cat [srv -6 stdout]]
        
        # Get the LAST "Start of election" line for each replica
        set election_lines3 [regexp -all -inline {Start of election delayed for \d+ milliseconds \(rank #\d+} $log3]
        set election_lines6 [regexp -all -inline {Start of election delayed for \d+ milliseconds \(rank #\d+} $log6]
        
        # Get the last one (final rank)
        set last_election3 [lindex $election_lines3 end]
        set last_election6 [lindex $election_lines6 end]
        
        # Extract rank number from the last election line
        regexp {rank #(\d+)} $last_election3 -> rank3
        regexp {rank #(\d+)} $last_election6 -> rank6
        
        puts "========== DEBUG INFO =========="
        puts "Replica -3 final rank: $rank3"
        puts "Replica -6 final rank: $rank6"
        puts "========== END DEBUG INFO =========="
        
        # Verify they have different ranks
        assert {$rank3 ne $rank6} "Both replicas have the same rank: $rank3"
    }

} ;# start_cluster

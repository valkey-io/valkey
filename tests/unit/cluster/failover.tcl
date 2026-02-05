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

    test "Make sure the replicas always get the different ranks" {
        puts "========== MAKE SURE TEST STARTING =========="
        # Get logs first
        set log3 [exec cat [srv -3 stdout]]
        set log6 [exec cat [srv -6 stdout]]
        
        # Extract rank information
        set srv3_has_rank0 [string match "*Start of election*rank #0*" $log3]
        set srv3_has_rank1 [string match "*Start of election*rank #1*" $log3]
        set srv6_has_rank0 [string match "*Start of election*rank #0*" $log6]
        set srv6_has_rank1 [string match "*Start of election*rank #1*" $log6]
        
        # Check if test will pass
        set role3 [s -3 role]
        set test_will_pass 0
        
        if {$role3 == "master"} {
            if {$srv3_has_rank0 && $srv6_has_rank1} {
                set test_will_pass 1
            }
        } else {
            if {$srv3_has_rank1 && $srv6_has_rank0} {
                set test_will_pass 1
            }
        }
        
        # If test will fail, print debug info
        if {!$test_will_pass} {
            puts "========== TEST FAILING - DEBUG INFO =========="
            puts "Replica -3 role: $role3"
            puts "Replica -6 role: [s -6 role]"
            puts ""
            puts "Replica -3 has rank #0: $srv3_has_rank0"
            puts "Replica -3 has rank #1: $srv3_has_rank1"
            puts "Replica -6 has rank #0: $srv6_has_rank0"
            puts "Replica -6 has rank #1: $srv6_has_rank1"
            puts "========== END DEBUG INFO =========="
        }
        
        # Original test logic
        if {$role3 == "master"} {
            verify_log_message -3 "*Start of election*rank #0*" 0
            verify_log_message -6 "*Start of election*rank #1*" 0
        } else {
            verify_log_message -3 "*Start of election*rank #1*" 0
            verify_log_message -6 "*Start of election*rank #0*" 0
        }
    }

} ;# start_cluster

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
        set log3 [exec cat [srv -3 stdout]]
        set log6 [exec cat [srv -6 stdout]]
    
        set srv3_has_rank0 [string match "*Start of election*(rank #0*" $log3]
        set srv3_has_rank1 [string match "*Start of election*(rank #1*" $log3]
        set srv6_has_rank0 [string match "*Start of election*(rank #0*" $log6]
        set srv6_has_rank1 [string match "*Start of election*(rank #1*" $log6]
    
        # One should have rank #0, other should have rank #1 (different ranks)
        if {!(($srv3_has_rank0 && $srv6_has_rank1) || ($srv3_has_rank1 && $srv6_has_rank0))} {
            fail "Replicas should have different ranks: srv3_rank0=$srv3_has_rank0, srv3_rank1=$srv3_has_rank1, srv6_rank0=$srv6_has_rank0, srv6_rank1=$srv6_has_rank1"
        }
    }

} ;# start_cluster

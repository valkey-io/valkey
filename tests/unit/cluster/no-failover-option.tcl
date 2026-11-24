# Check that the no-failover option works

source tests/support/cluster.tcl

start_cluster 3 3 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Instance #3 is a replica" {
    assert {[s -3 role] eq {slave}}

    # Configure it to never failover the master
    R 3 CONFIG SET cluster-replica-no-failover yes
}

test "Instance #3 synced with the master" {
    wait_for_condition 1000 50 {
        [s -3 master_link_status] eq {up}
    } else {
        fail "Instance #3 master link status is not up"
    }
}

test "The nofailover flag is propagated" {
    set replica3_id [dict get [cluster_get_myself 3] id]

    for {set j 0} {$j < [llength $::servers]} {incr j} {
        wait_for_condition 1000 50 {
            [cluster_has_flag [cluster_get_node_by_id $j $replica3_id] nofailover]
        } else {
            fail "Instance $id can't see the nofailover flag of replica"
        }
    }
}

test "Killing one master node" {
    pause_process_persistent [srv 0 pid]
}

test "Cluster should be still down after some time" {
    wait_for_condition 1000 50 {
        [CI 1 cluster_state] eq {fail} &&
        [CI 2 cluster_state] eq {fail} &&
        [CI 3 cluster_state] eq {fail} &&
        [CI 4 cluster_state] eq {fail} &&
        [CI 5 cluster_state] eq {fail}
    } else {
        fail "Cluster doesn't fail"
    }
}

test "Instance #3 is still a replica" {
    assert {[s -3 role] eq {slave}}
}

test "Restarting the previously killed master node" {
    resume_process [srv 0 pid]
}

test "The previously killed master node recovers" {
    set node0_id [dict get [cluster_get_myself 0] id]
    # Wait for all nodes to clear the FAIL flag
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        wait_for_condition 1000 50 {
            ![cluster_has_flag [cluster_get_node_by_id $j $node0_id] fail]
        } else {
            fail "Instance $j still sees FAIL flag on recovered node"
        }
    }
} ;# NODE_RECOVER propagation depends on outbound link timing

} ;# start_cluster

# Setting cluster-replica-no-failover to yes after the failover was scheduled
# must hold it back, and setting it back to no must let it go ahead. Raft only:
# gossip starts the election of the best ranked replica right away, so there is
# no window to change the config in.
if {$::cluster_raft} {
start_cluster 3 3 {tags {external:skip cluster needs:debug}} {
    test "No-failover set after scheduling holds the failover back" {
        # Leave enough time to change the config before the failover fires.
        R 3 DEBUG cluster-failover-delay 3000
        wait_for_condition 1000 50 {
            [s -3 master_link_status] eq {up}
        } else {
            fail "Instance #3 master link status is not up"
        }

        set loglines [count_log_lines -3]
        pause_process [srv 0 pid]
        wait_for_log_messages -3 {"*scheduling failover in 3000 milliseconds*"} $loglines 1000 10
        R 3 CONFIG SET cluster-replica-no-failover yes

        # Wait past the scheduled time.
        after 4000
        assert_equal {slave} [s -3 role]

        R 3 CONFIG SET cluster-replica-no-failover no
        wait_for_condition 1000 50 {
            [s -3 role] eq {master}
        } else {
            fail "Instance #3 did not fail over after no-failover was turned off"
        }

        resume_process [srv 0 pid]
    }
} ;# start_cluster
}

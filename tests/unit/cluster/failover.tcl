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

# Verify failover works when slot boundaries are not 64-bit aligned.
# When we use 3-shard layout, it puts boundaries at 5461 and 10922 (mid-word in the bitmap).
# We need memrev64ifbe after memcpy so ctzll returns the right bit positions
# on big-endian hosts, otherwise this test will fail early if there is no failover consensus.
start_cluster 3 1 {tags {external:skip cluster}} {
    test "Failover succeeds with non 64 bit aligned slot boundaries" {
        R 3 cluster failover
        wait_for_condition 1000 50 {
            [s -3 role] eq {master} &&
            [s 0 role] eq {slave}
        } else {
            fail "Failover did not happen"
        }
    }
} ;# start_cluster

# Failover stress test.
# In this test a different node is killed in a loop for N
# iterations. The test checks that certain properties
# are preserved across iterations.
start_cluster 5 5 {tags {external:skip cluster}} {
    set iterations 10
    set cluster [valkey_cluster 127.0.0.1:[srv 0 port]]

    while {[incr iterations -1]} {
        set tokill [randomInt 10]
        set other [expr {($tokill+1)%10}] ; # Some other instance.
        set key [randstring 20 20 alpha]
        set val [randstring 20 20 alpha]
        set role [s [expr -1*$tokill] role]
        if {$role eq {master}} {
            set slave {}
            set myid [dict get [cluster_get_myself $tokill] id]
            for {set id 0} {$id < [llength $::servers]} {incr id} {
                if {$id == $tokill} continue
                if {[dict get [cluster_get_myself $id] slaveof] eq $myid} {
                    set slave $id
                }
            }
            if {$slave eq {}} {
                fail "Unable to retrieve slave's ID for master #$tokill"
            }
        }

        if {$role eq {master}} {
            test "Wait for slave of #$tokill to sync" {
                wait_for_condition 1000 50 {
                    [string match {*state=online*} [s [expr -1*$tokill] slave0]]
                } else {
                    fail "Slave of node #$tokill is not ok"
                }
            }
            set slave_config_epoch [CI $slave cluster_my_epoch]
        }

        test "Cluster is writable before failover" {
            for {set i 0} {$i < 100} {incr i} {
                catch {$cluster set $key:$i $val:$i} err
                assert {$err eq {OK}}
            }
            # Wait for the write to propagate to the slave if we
            # are going to kill a master.
            if {$role eq {master}} {
                R $tokill wait 1 20000
            }
        }

        test "Terminating node #$tokill" {
            catch {R $tokill shutdown nosave}
        }

        if {$role eq {master}} {
            test "Wait failover by #$slave with old epoch $slave_config_epoch" {
                wait_for_condition 1000 50 {
                    [CI $slave cluster_my_epoch] > $slave_config_epoch
                } else {
                    fail "No failover detected, epoch is still [CI $slave cluster_my_epoch]"
                }
            }
        }

        test "Cluster should eventually be up again" {
            wait_for_cluster_state ok
        }

        test "Cluster is writable again" {
            for {set i 0} {$i < 100} {incr i} {
                catch {$cluster set $key:$i:2 $val:$i:2} err
                assert {$err eq {OK}}
            }
        }

        test "Restarting node #$tokill" {
            restart_server [expr -1*$tokill] true false
        }

        test "Instance #$tokill is now a slave" {
            wait_for_condition 1000 50 {
                [s [expr -1*$tokill] role] eq {slave}
            } else {
                fail "Restarted instance is not a slave"
            }
        }

        test "We can read back the value we set before" {
            for {set i 0} {$i < 100} {incr i} {
                catch {$cluster get $key:$i} err
                assert {$err eq "$val:$i"}
                catch {$cluster get $key:$i:2} err
                assert {$err eq "$val:$i:2"}
            }
        }
    }

    test "Post condition: current_epoch >= my_epoch everywhere" {
        for {set id 0} {$id < [llength $::servers]} {incr id} {
            assert {[CI $id cluster_current_epoch] >= [CI $id cluster_my_epoch]}
        }
    }
} ;# start_cluster

proc test_small_timeout {timeout} {
    test "Failover with cluster-node-time set to $timeout" {
        R 3 config set cluster-node-timeout $timeout

        pause_process [srv 0 pid]
        wait_for_condition 1000 50 {
            [s -3 role] == "master"
        } else {
            fail "Failover did not happen"
        }

        resume_process [srv 0 pid]
    }
}

start_cluster 3 1 {tags {external:skip cluster}} {
    test_small_timeout 30
} ;# start_cluster

start_cluster 3 1 {tags {external:skip cluster}} {
    test_small_timeout 0
} ;# start_cluster

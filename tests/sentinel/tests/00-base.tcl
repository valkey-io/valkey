# Check the basic monitoring and failover capabilities.
source "../tests/includes/start-init-tests.tcl"
source "../tests/includes/init-tests.tcl"

foreach_sentinel_id id {
    S $id sentinel debug default-down-after 1000
}

if {$::simulate_error} {
    test "This test will fail" {
        fail "Simulated error"
    }
}

test "Sentinel command flag infrastructure works correctly" {
    foreach_sentinel_id id {
        set command_list [S $id command list]

        foreach cmd {ping info subscribe client|setinfo} {
            assert_not_equal [S $id command docs $cmd] {}
            assert_not_equal [lsearch $command_list $cmd] -1
        }

        foreach cmd {save bgrewriteaof blpop replicaof} {
            assert_equal [S $id command docs $cmd] {}
            assert_equal [lsearch $command_list $cmd] -1
            assert_error {ERR unknown command*} {S $id $cmd}
        }

        assert_error {ERR unknown subcommand*} {S $id client no-touch}
    }
}

test "SENTINEL HELP output the sentinel subcommand help" {
    assert_match "*SENTINEL <subcommand> *" [S 0 SENTINEL HELP]
}

test "SENTINEL MYID return the sentinel instance ID" {
    assert_equal 40 [string length [S 0 SENTINEL MYID]]
    assert_equal [S 0 SENTINEL MYID] [S 0 SENTINEL MYID]
}

test "SENTINEL INFO CACHE returns the cached info" {
    set res [S 0 SENTINEL INFO-CACHE mymaster]
    assert_morethan_equal [llength $res] 2
    assert_equal "mymaster" [lindex $res 0]

    set res [lindex $res 1]
    assert_morethan_equal [llength $res] 2
    assert_morethan [lindex $res 0] 0
    assert_match "*# Server*" [lindex $res 1]
}

test "SENTINEL PENDING-SCRIPTS returns the information about pending scripts" {
    # may or may not have a value, so assert greater than or equal to 0.
    assert_morethan_equal [llength [S 0 SENTINEL PENDING-SCRIPTS]] 0
}

test "SENTINEL PRIMARIES returns a list of monitored primaries" {
    assert_match "*mymaster*" [S 0 SENTINEL PRIMARIES]
    assert_morethan_equal [llength [S 0 SENTINEL PRIMARIES]] 1
}

test "SENTINEL SENTINELS returns a list of sentinel instances" {
     assert_morethan_equal [llength [S 0 SENTINEL SENTINELS mymaster]] 1
}

test "SENTINEL SLAVES returns a list of the monitored replicas" {
     assert_morethan_equal [llength [S 0 SENTINEL REPLICAS mymaster]] 1
}

test "SENTINEL SIMULATE-FAILURE HELP list supported flags" {
    set res [S 0 SENTINEL SIMULATE-FAILURE HELP]
    assert_morethan_equal [llength $res] 2
    assert_equal {crash-after-election crash-after-promotion} $res
}

test "Basic failover works if the primary is down" {
    set old_port [RPort $master_id]
    set addr [S 0 SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]
    assert {[lindex $addr 1] == $old_port}
    kill_instance valkey $master_id
    foreach_sentinel_id id {
        S $id sentinel debug ping-period 500
        S $id sentinel debug ask-period 500  
        wait_for_condition 1000 100 {
            [lindex [S $id SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster] 1] != $old_port
        } else {
            fail "At least one Sentinel did not receive failover info"
        }
    }
    restart_instance valkey $master_id
    set addr [S 0 SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]
    set master_id [get_instance_id_by_port valkey [lindex $addr 1]]
}

test "New primary [join $addr {:}] role matches" {
    assert {[RI $master_id role] eq {master}}
}

test "All the other slaves now point to the new primary" {
    foreach_valkey_id id {
        if {$id != $master_id && $id != 0} {
            wait_for_condition 1000 50 {
                [RI $id master_port] == [lindex $addr 1]
            } else {
                fail "Valkey ID $id not configured to replicate with new master"
            }
        }
    }
}

test "The old primary eventually gets reconfigured as a slave" {
    wait_for_condition 1000 50 {
        [RI 0 master_port] == [lindex $addr 1]
    } else {
        fail "Old master not reconfigured as slave of new master"
    }
}

test "ODOWN is not possible without N (quorum) Sentinels reports" {
    foreach_sentinel_id id {
        S $id SENTINEL SET mymaster quorum [expr $sentinels+1]
    }
    set old_port [RPort $master_id]
    set addr [S 0 SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]
    assert {[lindex $addr 1] == $old_port}
    kill_instance valkey $master_id

    # Make sure failover did not happened.
    set addr [S 0 SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]
    assert {[lindex $addr 1] == $old_port}
    restart_instance valkey $master_id
}

test "Failover is not possible without majority agreement" {
    foreach_sentinel_id id {
        S $id SENTINEL SET mymaster quorum $quorum
    }

    # Crash majority of sentinels
    for {set id 0} {$id < $quorum} {incr id} {
        kill_instance sentinel $id
    }

    # Kill the current master
    kill_instance valkey $master_id

    # Make sure failover did not happened.
    set addr [S $quorum SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]
    assert {[lindex $addr 1] == $old_port}
    restart_instance valkey $master_id

    # Cleanup: restart Sentinels to monitor the master.
    for {set id 0} {$id < $quorum} {incr id} {
        restart_instance sentinel $id
    }
}

test "Failover works if we configure for absolute agreement" {
    foreach_sentinel_id id {
        S $id SENTINEL SET mymaster quorum $sentinels
    }

    # Wait for Sentinels to monitor the master again
    foreach_sentinel_id id {
        wait_for_condition 1000 100 {
            [dict get [S $id SENTINEL PRIMARY mymaster] info-refresh] < 100000
        } else {
            fail "At least one Sentinel is not monitoring the primary"
        }
    }

    kill_instance valkey $master_id

    foreach_sentinel_id id {
        wait_for_condition 1000 100 {
            [lindex [S $id SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster] 1] != $old_port
        } else {
            fail "At least one Sentinel did not receive failover info"
        }
    }
    restart_instance valkey $master_id
    set addr [S 0 SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]
    set master_id [get_instance_id_by_port valkey [lindex $addr 1]]

    # Set the min ODOWN agreement back to strict majority.
    foreach_sentinel_id id {
        S $id SENTINEL SET mymaster quorum $quorum
    }
}

test "New primary [join $addr {:}] role matches" {
    assert {[RI $master_id role] eq {master}}
}

test "SENTINEL RESET can resets the primary" {
    # After SENTINEL RESET, sometimes the sentinel can sense the primary again,
    # causing the test to fail. Here we give it a few more chances.
    for {set j 0} {$j < 10} {incr j} {
        assert_equal 1 [S 0 SENTINEL RESET mymaster]
        set res1 [llength [S 0 SENTINEL SENTINELS mymaster]]
        set res2 [llength [S 0 SENTINEL SLAVES mymaster]]
        set res3 [llength [S 0 SENTINEL REPLICAS mymaster]]
        if {$res1 eq 0 && $res2 eq 0 && $res3 eq 0} break
    }
    assert_equal 0 $res1
    assert_equal 0 $res2
    assert_equal 0 $res3
}

test "SENTINEL IS-PRIMARY-DOWN-BY-ADDR checks if the primary is down" {
    set sentinel_id [S 0 SENTINEL MYID]
    set master_ip_port [S 0 SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]
    set master_ip [lindex $master_ip_port 0]
    set master_port [lindex $master_ip_port 1]
    set result [S 0 SENTINEL IS-PRIMARY-DOWN-BY-ADDR $master_ip $master_port 50 $sentinel_id]
    assert_equal $sentinel_id [lindex $result 1]
    assert_equal {50} [lindex $result 2]
}

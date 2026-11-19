# Test Sentinel configuration consistency after partitions heal.
source "../tests/includes/init-tests.tcl"

test "SENTINEL MASTERS returns a list of monitored masters (SENTINEL MASTERS as a deprecated command)" {
    assert_match "*mymaster*" [S 0 SENTINEL MASTERS]
    assert_morethan_equal [llength [S 0 SENTINEL MASTERS]] 1
}

test "SENTINEL SLAVES returns a list of the monitored slaves (SENTINEL SLAVES as a deprecated command)" {
     assert_morethan_equal [llength [S 0 SENTINEL SLAVES mymaster]] 1
}

test "SENTINEL MASTER returns the information list of the monitored master (SENTINEL MASTER as a deprecated command)" {
     set info [S 0 SENTINEL MASTER mymaster]
     assert_equal mymaster [dict get $info name]
}

test "SENTINEL IS-MASTER-DOWN-BY-ADDR checks if the primary is down (SENTINEL IS-MASTER-DOWN-BY-ADDR as a deprecated command)" {
    set sentinel_id [S 0 SENTINEL MYID]
    set master_ip_port [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    set master_ip [lindex $master_ip_port 0]
    set master_port [lindex $master_ip_port 1]
    set result [S 0 SENTINEL IS-MASTER-DOWN-BY-ADDR $master_ip $master_port 99 $sentinel_id]
    assert_equal $sentinel_id [lindex $result 1]
    assert_equal {99} [lindex $result 2]
}


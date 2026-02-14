foreach topology {mesh ring} {
start_server {tags {"multi-master external:skip"} overrides {save {} active-replica yes multi-master yes replica-read-only no}} {
start_server {overrides {save {} active-replica yes multi-master yes replica-read-only no}} {
start_server {overrides {save {} active-replica yes multi-master yes replica-read-only no}} {
start_server {overrides {save {} active-replica yes multi-master yes replica-read-only no}} {
    for {set j 0} {$j < 4} {incr j} {
        set R($j) [srv [expr 0-$j] client]
        set RH($j) [srv [expr 0-$j] host]
        set RP($j) [srv [expr 0-$j] port]
    }

    if {$topology eq "mesh"} {
        # Star mesh anchored at node0 (stable with current active-upstream runtime).
        $R(1) replicaof add $RH(0) $RP(0)
        $R(2) replicaof add $RH(0) $RP(0)
        $R(3) replicaof add $RH(0) $RP(0)
    } else {
        # Ring-like chain rooted at node0.
        $R(1) replicaof add $RH(0) $RP(0)
        $R(2) replicaof add $RH(1) $RP(1)
        $R(3) replicaof add $RH(2) $RP(2)
    }

    test "$topology all nodes up" {
        for {set j 1} {$j < 4} {incr j} {
            wait_for_condition 100 100 {
                [string match {*master_global_link_status:up*} [$R($j) info replication]]
            } else {
                fail "node $j did not establish replication link"
            }
        }
    }

    test "$topology propagates writes over cycle links" {
        for {set n 1} {$n < 4} {incr n} {
            set key "mm:key:$n"
            set val "v$n"
            $R(0) set $key $val
            wait_for_condition 100 100 {
                [$R($n) get $key] eq $val
            } else {
                fail "node $n did not receive key '$key' from root node"
            }
        }
    }

    test "$topology converges after sequential updates" {
        $R(0) set mm:counter 1
        after 200
        $R(0) set mm:counter 2
        after 200
        $R(0) set mm:counter 3
        wait_for_condition 150 100 {
            [$R(0) get mm:counter] eq [$R(1) get mm:counter] &&
            [$R(1) get mm:counter] eq [$R(2) get mm:counter] &&
            [$R(2) get mm:counter] eq [$R(3) get mm:counter]
        } else {
            fail "cluster did not converge on the same counter value"
        }
    }
}}}}
}

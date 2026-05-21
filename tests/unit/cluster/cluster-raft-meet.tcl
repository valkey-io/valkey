# Test CLUSTER MEET scenarios for the Raft cluster protocol.

proc get_cluster_info_field {client field} {
    set info [$client CLUSTER INFO]
    foreach line [split $info "\n"] {
        set line [string trim $line "\r"]
        if {[string match "${field}:*" $line]} {
            return [lindex [split $line ":"] 1]
        }
    }
    return ""
}

tags {external:skip cluster singledb} {

test "Raft MEET: two singletons" {
    start_multiple_servers 2 {overrides {cluster-enabled yes cluster-protocol raft}} {
        set r0 [srv 0 client]
        set r1 [srv -1 client]

        $r0 CLUSTER MEET [srv -1 host] [srv -1 port]

        wait_for_condition 50 100 {
            [get_cluster_info_field $r0 cluster_size] == 2 &&
            [get_cluster_info_field $r1 cluster_size] == 2
        } else {
            fail "Cluster size: [get_cluster_info_field $r0 cluster_size] [get_cluster_info_field $r1 cluster_size]"
        }
    }
}

test "Raft MEET: star formation - first node meets each node" {
    start_multiple_servers 5 {overrides {cluster-enabled yes cluster-protocol raft}} {
        set r0 [srv 0 client]

        for {set i 1} {$i < 5} {incr i} {
            $r0 CLUSTER MEET [srv -$i host] [srv -$i port]
        }

        wait_for_condition 50 200 {
            [get_cluster_info_field $r0 cluster_size] == 5 &&
            [get_cluster_info_field [srv -1 client] cluster_size] == 5 &&
            [get_cluster_info_field [srv -4 client] cluster_size] == 5
        } else {
            fail "Sizes: [get_cluster_info_field $r0 cluster_size] [get_cluster_info_field [srv -1 client] cluster_size] [get_cluster_info_field [srv -4 client] cluster_size]"
        }
    }
}

test "Raft MEET: reverse star formation - each node meets the first node" {
    start_multiple_servers 5 {overrides {cluster-enabled yes cluster-protocol raft}} {
        set r0 [srv 0 client]

        for {set i 1} {$i < 5} {incr i} {
            [srv -$i client] CLUSTER MEET [srv 0 host] [srv 0 port]
        }

        wait_for_condition 50 200 {
            [get_cluster_info_field $r0 cluster_size] == 5 &&
            [get_cluster_info_field [srv -1 client] cluster_size] == 5 &&
            [get_cluster_info_field [srv -4 client] cluster_size] == 5
        } else {
            fail "Sizes: [get_cluster_info_field $r0 cluster_size] [get_cluster_info_field [srv -1 client] cluster_size] [get_cluster_info_field [srv -4 client] cluster_size]"
        }
    }
}

test "Raft MEET: chain formation - each node meets the next" {
    start_multiple_servers 5 {overrides {cluster-enabled yes cluster-protocol raft}} {
        set r0 [srv 0 client]

        for {set i 0} {$i < 4} {incr i} {
            [srv -$i client] CLUSTER MEET [srv -[expr {$i+1}] host] [srv -[expr {$i+1}] port]
        }

        wait_for_condition 50 200 {
            [get_cluster_info_field $r0 cluster_size] == 5 &&
            [get_cluster_info_field [srv -1 client] cluster_size] == 5 &&
            [get_cluster_info_field [srv -4 client] cluster_size] == 5
        } else {
            fail "Sizes: [get_cluster_info_field $r0 cluster_size] [get_cluster_info_field [srv -1 client] cluster_size] [get_cluster_info_field [srv -4 client] cluster_size]"
        }
    }
}

test "Raft MEET: addslots after meet" {
    start_multiple_servers 2 {overrides {cluster-enabled yes cluster-protocol raft}} {
        set r0 [srv 0 client]
        set r1 [srv -1 client]

        $r0 CLUSTER MEET [srv -1 host] [srv -1 port]
        wait_for_condition 50 100 {
            [get_cluster_info_field $r0 cluster_size] == 2
        } else {
            fail "Cluster did not form"
        }

        $r0 CLUSTER ADDSLOTSRANGE 0 16383

        wait_for_condition 50 100 {
            [get_cluster_info_field $r0 cluster_slots_assigned] == 16384 &&
            [get_cluster_info_field $r1 cluster_slots_assigned] == 16384
        } else {
            fail "Slots: [get_cluster_info_field $r0 cluster_slots_assigned] [get_cluster_info_field $r1 cluster_slots_assigned]"
        }

        assert_equal ok [get_cluster_info_field $r0 cluster_state]
        assert_equal ok [get_cluster_info_field $r1 cluster_state]
    }
}

} ;# tags

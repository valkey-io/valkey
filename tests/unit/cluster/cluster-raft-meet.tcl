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
    start_server {overrides {cluster-enabled yes cluster-protocol raft}} {
    start_server {overrides {cluster-enabled yes cluster-protocol raft}} {
        set r0 [srv 0 client]
        set r1 [srv -1 client]

        $r0 CLUSTER MEET [srv -1 host] [srv -1 port]

        wait_for_condition 50 100 {
            [get_cluster_info_field $r0 cluster_size] == 2 &&
            [get_cluster_info_field $r1 cluster_size] == 2
        } else {
            fail "Cluster size: [get_cluster_info_field $r0 cluster_size] [get_cluster_info_field $r1 cluster_size]"
        }
    }}
}

test "Raft MEET: singleton joins 2-node cluster" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft}} {
    start_server {overrides {cluster-enabled yes cluster-protocol raft}} {
    start_server {overrides {cluster-enabled yes cluster-protocol raft}} {
        set r0 [srv 0 client]
        set r1 [srv -1 client]
        set r2 [srv -2 client]

        $r0 CLUSTER MEET [srv -1 host] [srv -1 port]
        wait_for_condition 50 100 {
            [get_cluster_info_field $r0 cluster_size] == 2
        } else {
            fail "2-node cluster did not form"
        }

        $r0 CLUSTER MEET [srv -2 host] [srv -2 port]
        wait_for_condition 50 100 {
            [get_cluster_info_field $r0 cluster_size] == 3 &&
            [get_cluster_info_field $r1 cluster_size] == 3 &&
            [get_cluster_info_field $r2 cluster_size] == 3
        } else {
            fail "Sizes: [get_cluster_info_field $r0 cluster_size] [get_cluster_info_field $r1 cluster_size] [get_cluster_info_field $r2 cluster_size]"
        }
    }}}
}

test "Raft MEET: chain meet via follower" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft}} {
    start_server {overrides {cluster-enabled yes cluster-protocol raft}} {
    start_server {overrides {cluster-enabled yes cluster-protocol raft}} {
        set r0 [srv 0 client]
        set r1 [srv -1 client]
        set r2 [srv -2 client]

        $r0 CLUSTER MEET [srv -1 host] [srv -1 port]
        wait_for_condition 50 100 {
            [get_cluster_info_field $r0 cluster_size] == 2
        } else {
            fail "2-node cluster did not form"
        }

        # r1 (follower) meets r2 — should forward to leader.
        $r1 CLUSTER MEET [srv -2 host] [srv -2 port]
        wait_for_condition 50 100 {
            [get_cluster_info_field $r0 cluster_size] == 3 &&
            [get_cluster_info_field $r1 cluster_size] == 3 &&
            [get_cluster_info_field $r2 cluster_size] == 3
        } else {
            fail "Sizes: [get_cluster_info_field $r0 cluster_size] [get_cluster_info_field $r1 cluster_size] [get_cluster_info_field $r2 cluster_size]"
        }
    }}}
}

test "Raft MEET: addslots after meet" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft}} {
    start_server {overrides {cluster-enabled yes cluster-protocol raft}} {
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
    }}
}

} ;# tags

proc slots_map_has_az_last {slots_resp} {
    foreach entry $slots_resp {
        foreach node [lrange $entry 2 end] {
            set extra_map [lindex $node 3]
            if {[llength $extra_map] == 0} {
                return 0
            }
            if {[lindex $extra_map end-1] ne "availability-zone"} {
                return 0
            }
        }
    }
    return 1
}

proc shards_map_has_az_last {shards_resp} {
    foreach shard $shards_resp {
        set nodes_index [lsearch -exact $shard "nodes"]
        if {$nodes_index < 0} {
            return 0
        }
        set nodes [lindex $shard [expr {$nodes_index + 1}]]
        foreach node $nodes {
            if {[lindex $node end-1] ne "availability-zone"} {
                return 0
            }
        }
    }
    return 1
}

start_cluster 2 0 {tags {external:skip cluster} overrides {cluster-ping-interval 100}} {
    test "Availability zone appears in SLOTS/SHARDS" {
        R 0 CONFIG SET availability-zone zone-a
        R 1 CONFIG SET availability-zone zone-b

        wait_for_condition 50 100 {
            [slots_map_has_az_last [R 0 CLUSTER SLOTS]]
        } else {
            fail "Availability zone was not propagated in CLUSTER SLOTS"
        }

        set slots_resp [R 0 CLUSTER SLOTS]
        set slots_str [join $slots_resp " "]
        assert_match "*availability-zone*" $slots_str
        assert_match "*zone-a*" $slots_str
        assert_match "*zone-b*" $slots_str

        puts "CLUSTER SLOTS (after initial set):"
        puts $slots_resp

        wait_for_condition 50 100 {
            [shards_map_has_az_last [R 0 CLUSTER SHARDS]]
        } else {
            fail "Availability zone was not propagated in CLUSTER SHARDS"
        }

        set shards_resp [R 0 CLUSTER SHARDS]
        set shards_str [join $shards_resp " "]
        assert_match "*availability-zone*" $shards_str
        assert_match "*zone-a*" $shards_str
        assert_match "*zone-b*" $shards_str

        puts "CLUSTER SHARDS (after initial set):"
        puts $shards_resp
    }

    test "Availability zone updates at runtime" {
        R 0 CONFIG SET availability-zone zone-a
        R 0 CONFIG SET availability-zone zone-c

        wait_for_condition 50 100 {
            [slots_map_has_az_last [R 1 CLUSTER SLOTS]] &&
            [string match "*zone-c*" [join [R 1 CLUSTER SLOTS] " "]]
        } else {
            fail "Availability zone was not propagated in CLUSTER SLOTS"
        }

        set slots_resp [R 1 CLUSTER SLOTS]
        set slots_str [join $slots_resp " "]
        assert_match "*availability-zone*" $slots_str
        assert_match "*zone-c*" $slots_str

        puts "CLUSTER SLOTS (after update to zone-c):"
        puts $slots_resp

        wait_for_condition 50 100 {
            [shards_map_has_az_last [R 1 CLUSTER SHARDS]] &&
            [string match "*zone-c*" [join [R 1 CLUSTER SHARDS] " "]]
        } else {
            fail "Availability zone was not propagated in CLUSTER SHARDS"
        }

        set shards_resp [R 1 CLUSTER SHARDS]
        set shards_str [join $shards_resp " "]
        assert_match "*availability-zone*" $shards_str
        assert_match "*zone-c*" $shards_str

        puts "CLUSTER SHARDS (after update to zone-c):"
        puts $shards_resp
    }

    test "Availability zone removed when set to empty string" {
        R 0 CONFIG SET availability-zone ""
        R 1 CONFIG SET availability-zone ""

        wait_for_condition 50 100 {
            ![string match "*availability-zone*" [join [R 0 CLUSTER SLOTS] " "]] &&
            ![string match "*availability-zone*" [join [R 0 CLUSTER SHARDS] " "]]
        } else {
            fail "Availability zone was not cleared from CLUSTER SLOTS/SHARDS"
        }

        set slots_resp [R 0 CLUSTER SLOTS]
        set slots_str [join $slots_resp " "]
        assert {[string match "*availability-zone*" $slots_str] == 0}
        assert {[string match "*zone-a*" $slots_str] == 0}
        assert {[string match "*zone-b*" $slots_str] == 0}
        assert {[string match "*zone-c*" $slots_str] == 0}

        puts "CLUSTER SLOTS (after clearing availability zone):"
        puts $slots_resp

        set shards_resp [R 0 CLUSTER SHARDS]
        set shards_str [join $shards_resp " "]
        assert {[string match "*availability-zone*" $shards_str] == 0}

        puts "CLUSTER SHARDS (after clearing availability zone):"
        puts $shards_resp
    }
}

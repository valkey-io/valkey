start_cluster 2 0 {tags {external:skip cluster} overrides {cluster-ping-interval 100}} {
    test "Availability zone appears in CLUSTER NODES/SLOTS/SHARDS" {
        R 0 CONFIG SET availability-zone zone-a
        R 1 CONFIG SET availability-zone zone-b

        wait_for_condition 50 100 {
            [string match "* zone-a*" [R 1 CLUSTER NODES]] &&
            [string match "* zone-b*" [R 0 CLUSTER NODES]]
        } else {
            fail "Availability zone was not propagated in CLUSTER NODES"
        }

        puts "CLUSTER NODES (after initial set):"
        puts [R 0 CLUSTER NODES]

        set slots_resp [R 0 CLUSTER SLOTS]
        set slots_str [join $slots_resp " "]
        assert_match "*zone-a*" $slots_str
        assert_match "*zone-b*" $slots_str

        puts "CLUSTER SLOTS (after initial set):"
        puts $slots_resp

        set shards_resp [R 0 CLUSTER SHARDS]
        set shards_str [join $shards_resp " "]
        assert_match "*availability_zone*" $shards_str
        assert_match "*zone-a*" $shards_str
        assert_match "*zone-b*" $shards_str

        puts "CLUSTER SHARDS (after initial set):"
        puts $shards_resp
    }

    test "Availability zone updates at runtime" {
        R 0 CONFIG SET availability-zone zone-a
        wait_for_condition 50 100 {
            [string match "* zone-a*" [R 1 CLUSTER NODES]]
        } else {
            fail "Initial availability zone not propagated in CLUSTER NODES"
        }

        R 0 CONFIG SET availability-zone zone-c
        wait_for_condition 50 100 {
            [string match "* zone-c*" [R 1 CLUSTER NODES]]
        } else {
            fail "Updated availability zone not propagated in CLUSTER NODES"
        }

        puts "CLUSTER NODES (after update to zone-c):"
        puts [R 1 CLUSTER NODES]

        set slots_resp [R 1 CLUSTER SLOTS]
        set slots_str [join $slots_resp " "]
        assert_match "*zone-c*" $slots_str

        puts "CLUSTER SLOTS (after update to zone-c):"
        puts $slots_resp

        set shards_resp [R 1 CLUSTER SHARDS]
        set shards_str [join $shards_resp " "]
        assert_match "*availability_zone*" $shards_str
        assert_match "*zone-c*" $shards_str

        puts "CLUSTER SHARDS (after update to zone-c):"
        puts $shards_resp
    }

    test "Availability zone removed when set to empty string" {
        R 0 CONFIG SET availability-zone ""
        R 1 CONFIG SET availability-zone ""

        wait_for_condition 50 100 {
            ![string match "* zone-a*" [R 0 CLUSTER NODES]] &&
            ![string match "* zone-b*" [R 0 CLUSTER NODES]] &&
            ![string match "* zone-c*" [R 0 CLUSTER NODES]] &&
            ![string match "* zone-a*" [R 1 CLUSTER NODES]] &&
            ![string match "* zone-b*" [R 1 CLUSTER NODES]] &&
            ![string match "* zone-c*" [R 1 CLUSTER NODES]]
        } else {
            fail "Availability zone was not cleared from CLUSTER NODES"
        }

        puts "CLUSTER NODES (after clearing availability zone):"
        puts [R 0 CLUSTER NODES]

        set slots_resp [R 0 CLUSTER SLOTS]
        set slots_str [join $slots_resp " "]
        assert {[string match "*zone-a*" $slots_str] == 0}
        assert {[string match "*zone-b*" $slots_str] == 0}
        assert {[string match "*zone-c*" $slots_str] == 0}

        puts "CLUSTER SLOTS (after clearing availability zone):"
        puts $slots_resp

        set shards_resp [R 0 CLUSTER SHARDS]
        set shards_str [join $shards_resp " "]
        assert {[string match "*availability_zone*" $shards_str] == 0}

        puts "CLUSTER SHARDS (after clearing availability zone):"
        puts $shards_resp
    }
}

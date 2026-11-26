# Reply blocking on the target of an atomic slot migration: a key the import
# client writes must not be served before it is durable on the target.

proc slot_ranges_contains_slot {slot_ranges slot} {
    set ranges [split $slot_ranges " "]
    foreach slot_range $ranges {
        lassign [split $slot_range -] start end
        if {$end == {}} {set end $start}
        if {$slot >= $start && $slot <= $end} {
            return 1
        }
    }
    return 0
}

proc is_slot_migrated {node_idx slot} {
    set target_id [R $node_idx CLUSTER MYID]
    set nodes [get_cluster_nodes $node_idx]
    foreach n $nodes {
        set node_id [dict get $n id]
        if {$node_id eq $target_id} {
            set slot_ranges [dict get $n slots]
            if {[slot_ranges_contains_slot $slot_ranges $slot]} {
                return 1
            }
        }
    }
    return 0
}

start_cluster 3 0 {tags {logreqres:skip external:skip cluster network durability} overrides {appendonly yes appendfsync always bio-aof-offload-enabled yes auto-aof-rewrite-percentage 0}} {
    set tag "{6ZJ}"
    set node0_id [R 0 CLUSTER MYID]

    test "A key imported by slot migration is held until it is durable on the target" {
        R 2 set $tag:k imported-value
        R 0 DEBUG reply-blocking-pause aof
        assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
        wait_for_condition 100 100 {
            [is_slot_migrated 0 16383]
        } else {
            R 0 DEBUG reply-blocking-resume aof
            fail "slot was not migrated"
        }
        set rd [valkey_deferring_client 0]
        $rd get $tag:k
        after 300
        set fd [$rd channel]
        fconfigure $fd -blocking 0
        set got [read $fd]
        fconfigure $fd -blocking 1
        R 0 DEBUG reply-blocking-resume aof
        assert_equal "" $got
        assert_equal imported-value [$rd read]
        $rd close
    }
}

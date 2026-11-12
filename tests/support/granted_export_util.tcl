# Helpers for tests that hold an atomic slot export in failover-granted
# (tests/unit/cluster/granted-export.tcl, tests/unit/moduleapi/cluster.tcl).
# Once the source has replied FAILOVER-GRANTED, the target may take the slots
# at any time, so the source must stay paused until the takeover (or until
# the pause times out). A write it accepts after unpausing is lost when the
# target's higher epoch wins.

proc granted_export_slot_in_ranges {slot_ranges slot} {
    foreach slot_range [split $slot_ranges " "] {
        lassign [split $slot_range -] start end
        if {$end == {}} {set end $start}
        if {$slot >= $start && $slot <= $end} {return 1}
    }
    return 0
}

proc granted_export_node_owns_slot {node_idx slot} {
    set myid [R $node_idx CLUSTER MYID]
    foreach n [get_cluster_nodes $node_idx] {
        if {[dict get $n id] eq $myid} {
            return [granted_export_slot_in_ranges [dict get $n slots] $slot]
        }
    }
    return 0
}

proc granted_export_get_job {node_idx {name ""} {slot 0}} {
    foreach migration [R $node_idx CLUSTER GETSLOTMIGRATIONS] {
        if {$name ne "" ? [dict get $migration name] eq $name
                        : [granted_export_slot_in_ranges [dict get $migration slot_ranges] $slot]} {
            return $migration
        }
    }
    return ""
}

proc granted_export_wait_for_state {node_idx jobname state} {
    wait_for_condition 100 100 {
        [granted_export_get_job $node_idx $jobname] ne "" &&
        [dict get [granted_export_get_job $node_idx $jobname] state] eq $state
    } else {
        fail "Migration $jobname on node $node_idx not in state $state: [granted_export_get_job $node_idx $jobname]"
    }
}

# Hold the export of slot 0 from node 0 to node 2 in failover-granted, with
# the target stopped (SIGSTOP) before it reads FAILOVER-GRANTED. Returns the
# job name. Needs a 3-primary cluster where node 0 owns slot 0.
proc granted_export_hold {} {
    set node2_id [R 2 CLUSTER MYID]
    set source_pid [srv 0 pid]
    set target_pid [srv -2 pid]
    for {set i 0} {$i < [llength $::servers]} {incr i} {
        assert_match "OK" [R $i DEBUG SLOTMIGRATION PREVENT-PAUSE 1]
    }
    # Runs after the test ends, once paused processes are resumed.
    test_defer {
        for {set i 0} {$i < [llength $::servers]} {incr i} {
            R $i DEBUG SLOTMIGRATION PREVENT-PAUSE 0
        }
    }
    assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 NODE $node2_id]
    set jobname [dict get [granted_export_get_job 0] name]
    granted_export_wait_for_state 0 $jobname waiting-to-pause
    granted_export_wait_for_state 2 $jobname waiting-for-paused
    pause_process $target_pid
    assert_match "OK" [R 0 DEBUG SLOTMIGRATION PREVENT-PAUSE 0]
    granted_export_wait_for_state 0 $jobname failover-paused
    pause_process $source_pid
    resume_process $target_pid
    granted_export_wait_for_state 2 $jobname failover-requested
    pause_process $target_pid
    resume_process $source_pid
    granted_export_wait_for_state 0 $jobname failover-granted
    # The target is stopped, so PREVENT-PAUSE is cleared only on the source.
    # A helper that loops over all servers would block on the stopped node.
    return $jobname
}

# Hold the export in failover-granted, run 'trigger' on the source, then send
# SET + WAIT 1 and resume the target. The source must stay paused, so the SET
# is held until the takeover and no acknowledged write is lost.
proc granted_export_check_trigger {trigger} {
    set key "{06S}:granted"
    assert_match "OK" [R 0 SET $key old]
    assert_equal 1 [R 0 WAIT 1 2000]
    set target_pid [srv -2 pid]
    set jobname [granted_export_hold]
    assert_equal "slot_migration_in_progress" [s -0 paused_reason]

    uplevel 1 $trigger
    # Give the trigger time to take effect (a DEBUG hook may act on the next
    # cron tick).
    after 300
    assert_equal "slot_migration_in_progress" [s -0 paused_reason]
    assert_equal "failover-granted" [dict get [granted_export_get_job 0 $jobname] state]

    set rd [valkey_deferring_client_by_addr [srv 0 host] [srv 0 port]]
    test_defer [list $rd close]
    $rd SET $key new
    $rd flush
    wait_for_blocked_client

    resume_process $target_pid
    wait_for_condition 100 100 {
        [granted_export_node_owns_slot 2 0]
    } else {
        fail "The target did not take over slot 0"
    }
    assert_error "MOVED 0 *" {$rd read}
    assert_equal "old" [R 2 GET $key]
}

# Disable replica migration to prevent empty nodes from joining other shards.
start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no}} {

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

proc get_link_name {node_idx slot} {
    set migrations [R $node_idx CLUSTER MIGRATIONS]
    foreach migration $migrations {
        set slot_ranges [dict get $migration slot_ranges]
        if {[slot_ranges_contains_slot $slot_ranges $slot]} {
            return [dict get $migration link_name]
        }
    }
    return ""
}

proc get_migration_by_slot {node_idx slot} {
    set migrations [R $node_idx CLUSTER MIGRATIONS]
    foreach migration $migrations {
        set slot_ranges [dict get $migration slot_ranges]
        if {[slot_ranges_contains_slot $slot_ranges $slot]} {
            return $migration
        }
    }
    return ""
}

proc get_migration_by_linkname {node_idx linkname} {
    set migrations [R $node_idx CLUSTER MIGRATIONS]
    foreach migration $migrations {
        if {[dict get $migration link_name] eq $linkname} {
            return $migration
        }
    }
    return ""
}


proc get_migration_log_by_slot {node_idx slot} {
    set logs [R $node_idx CLUSTER MIGRATIONLOG]
    foreach log $logs {
        set slot_ranges [dict get $log slot_ranges]
        if {[slot_ranges_contains_slot $slot_ranges $slot]} {
            return $log
        }
    }
    return ""
}

proc get_migration_log_by_linkname {node_idx linkname} {
    set logs [R $node_idx CLUSTER MIGRATIONLOG]
    foreach log $logs {
        if {[dict get $log link_name] eq $linkname} {
            return $log
        }
    }
    return ""
}

proc is_link_up {node_idx linkname} {
    return [expr {[get_migration_by_linkname $node_idx $linkname] ne ""}]
}

proc wait_for_ready_to_commit {node_idx linkname} {
    wait_for_condition 100 100 {
        [dict get [get_migration_by_linkname $node_idx $linkname] state] eq "replicating"
    } else {
        fail "Migration $linkname on node $node_idx was not ready to commit within 10000 ms"
    }
}

proc wait_for_link_down {node_idx linkname} {
    wait_for_condition 100 100 {
        [get_migration_by_linkname $node_idx $linkname] eq ""
    } else {
        fail "Migration $linkname on node $node_idx was not terminated within 10000 ms"
    }
}

proc wait_for_link_down_by_slot {node_idx slot} {
    wait_for_condition 100 100 {
        [get_migration_by_slot $node_idx $slot] eq ""
    } else {
        fail "Migration of slot $slot on node $node_idx was not terminated within 10000 ms"
    }
}

proc wait_for_countkeysinslot {node_idx slot value} {
    wait_for_condition 100 100 {
        [R $node_idx CLUSTER COUNTKEYSINSLOT $slot] eq "$value"
    } else {
        fail "Node $node_idx did not have $value keys in slot $slot within 10000 ms"
    }
}

proc wait_for_migration {node_idx slot} {
    set target_id [R $node_idx CLUSTER MYID]
    wait_for_condition 100 100 {
        [is_slot_migrated $node_idx $slot]
    } else {
        fail "Cluster node $target_id did not get slot $slot within 10000 ms"
    }
    wait_for_cluster_propagation
}

proc write_data {idx prefix num size} {
    [Rn $idx] deferred 1
    if {$num > 16} {set pipeline 16} else {set pipeline $num}
    set val [string repeat A $size]
    for {set j 0} {$j < $pipeline} {incr j} {
        [Rn $idx] set $prefix$j $val
    }
    for {} {$j < $num} {incr j} {
        [Rn $idx] set $prefix$j $val
        [Rn $idx] read
    }
    for {set j 0} {$j < $pipeline} {incr j} {
        [Rn $idx] read
    }
    [Rn $idx] deferred 0
}

test "Test command interface" {
    foreach command {"IMPORT" "IMPORT-PREPARE"} {
        assert_error "*wrong number of arguments*" {R 0 CLUSTER $command}
        assert_error "*syntax error*" {R 0 CLUSTER $command INVALID 0 1}
        assert_error "*wrong number of arguments*" {R 0 CLUSTER $command SLOTSRANGE}
        assert_error "*wrong number of arguments*" {R 0 CLUSTER $command SLOTSRANGE 0}
        assert_error "*Invalid or out of range slot*" {R 0 CLUSTER $command SLOTSRANGE 16385 16388}
        assert_error "*Invalid or out of range slot*" {R 0 CLUSTER $command SLOTSRANGE 16380 16388}
        assert_error "*Invalid or out of range slot*" {R 0 CLUSTER $command SLOTSRANGE a 0}
        assert_error "*Invalid or out of range slot*" {R 0 CLUSTER $command SLOTSRANGE 0 a}
        assert_error "*Start slot number 1 is greater than end slot number 0*" {R 0 CLUSTER $command SLOTSRANGE 1 0}
        assert_error "*The slot ranges can not cross nodes*" {R 0 CLUSTER $command SLOTSRANGE 0 16383}
        assert_error "*Slot range 3-6 overlaps with previous range 0-5*" {R 0 CLUSTER $command SLOTSRANGE 0 5 3 6}
        assert_error "*Slot range 0-5 overlaps with previous range 3-6*" {R 0 CLUSTER $command SLOTSRANGE 3 6 0 5}

        set source_node_id [R 0 CLUSTER MYID]
        set target_node_id [R 1 CLUSTER MYID]
        R 0 CLUSTER SETSLOT 0 MIGRATING $target_node_id
        R 1 CLUSTER SETSLOT 0 IMPORTING $source_node_id
        assert_error "*Some slots are being manually migrated*" {R 0 CLUSTER $command SLOTSRANGE 16383 16383}
        assert_error "*Some slots are being manually imported*" {R 1 CLUSTER $command SLOTSRANGE 16383 16383}
        R 0 CLUSTER SETSLOT 0 STABLE
        R 1 CLUSTER SETSLOT 0 STABLE

        R 0 CLUSTER DELSLOTS 0
        assert_error "*Slot 0 has no node served*" {R 0 CLUSTER $command SLOTSRANGE 0 0}
        R 0 CLUSTER ADDSLOTS 0

        assert_error "*Import can only be used on primary nodes*" {R 3 CLUSTER $command SLOTSRANGE 0 0}
        assert_error "*Slots are already served by myself*" {R 0 CLUSTER $command SLOTSRANGE 0 0}
    }

    assert_error "*wrong number of arguments*" {R 0 CLUSTER IMPORT-CANCEL}
    assert_error "*No imports ongoing*" {R 0 CLUSTER IMPORT-CANCEL ALL}
    assert_error "*syntax error*" {R 0 CLUSTER IMPORT-CANCEL LINK}
    assert_error "*not found*" {R 0 CLUSTER IMPORT-CANCEL LINK abcdef}

    assert_error "*wrong number of arguments*" {R 0 CLUSTER IMPORT-COMMIT}
    assert_error "*wrong number of arguments*" {R 0 CLUSTER IMPORT-COMMIT LINK}
    assert_error "*not found*" {R 0 CLUSTER IMPORT-COMMIT LINK abcdef}
    assert_error "*syntax error*" {R 0 CLUSTER IMPORT-COMMIT INVALID abcdef}
}

test "Test CLUSTER IMPORT already importing" {
    assert_match "OK" [R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 16383 16383]
    assert_error "*I am already importing slot 16383*" {R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 16383 16383}
    assert_error "*I am already importing slot 16383*" {R 0 CLUSTER IMPORT SLOTSRANGE 16383 16383}
    R 0 CLUSTER IMPORT-CANCEL ALL

    assert_match "OK" [R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 16381 16383]
    assert_error "*I am already importing slot 16382*" {R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 16382 16382}
    assert_error "*I am already importing slot 16382*" {R 0 CLUSTER IMPORT SLOTSRANGE 16382 16382}
    R 0 CLUSTER IMPORT-CANCEL ALL

    assert_match "OK" [R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 16382 16382]
    assert_error "*I am already importing slot 16382*" {R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 16381 16383}
    assert_error "*I am already importing slot 16382*" {R 0 CLUSTER IMPORT SLOTSRANGE 16381 16383}
    R 0 CLUSTER IMPORT-CANCEL ALL
}

test "Test CLUSTER IMPORT-CANCEL ALL" {
    assert_match "OK" [R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 16382 16382]
    assert_match "OK" [R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 16383 16383]
    set linkname1 [get_link_name 0 16382]
    set linkname2 [get_link_name 0 16383]
    wait_for_ready_to_commit 0 $linkname1
    wait_for_ready_to_commit 0 $linkname2

    # Links are up, no migration logs yet
    assert [is_link_up 0 $linkname1]
    assert [is_link_up 0 $linkname2]
    assert [is_link_up 2 $linkname1]
    assert [is_link_up 2 $linkname2]
    assert {[get_migration_log_by_linkname 0 $linkname1] eq ""}
    assert {[get_migration_log_by_linkname 0 $linkname2] eq ""}
    assert {[get_migration_log_by_linkname 2 $linkname1] eq ""}
    assert {[get_migration_log_by_linkname 2 $linkname2] eq ""}

    assert_match "OK" [R 0 CLUSTER IMPORT-CANCEL ALL]

    # Links are no longer up, migration logs say cancelled
    assert {![is_link_up 0 $linkname1]}
    assert {![is_link_up 0 $linkname2]}
    wait_for_link_down 2 $linkname1
    wait_for_link_down 2 $linkname2
    assert {[dict get [get_migration_log_by_linkname 0 $linkname1] state] eq "cancelled"}
    assert {[dict get [get_migration_log_by_linkname 0 $linkname2] state] eq "cancelled"}
    assert {[dict get [get_migration_log_by_linkname 2 $linkname1] state] eq "cancelled"}
    assert {[dict get [get_migration_log_by_linkname 2 $linkname2] state] eq "cancelled"}
}

test "Test CLUSTER IMPORT-CANCEL LINK" {
    assert_match "OK" [R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 16382 16382]
    assert_match "OK" [R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 16383 16383]
    set linkname1 [get_link_name 0 16382]
    set linkname2 [get_link_name 0 16383]
    wait_for_ready_to_commit 0 $linkname1
    wait_for_ready_to_commit 0 $linkname2

    # Links are up, no migration logs yet
    assert [is_link_up 0 $linkname1]
    assert [is_link_up 0 $linkname2]
    assert [is_link_up 2 $linkname1]
    assert [is_link_up 2 $linkname2]
    assert {[get_migration_log_by_linkname 0 $linkname1] eq ""}
    assert {[get_migration_log_by_linkname 0 $linkname2] eq ""}
    assert {[get_migration_log_by_linkname 2 $linkname1] eq ""}
    assert {[get_migration_log_by_linkname 2 $linkname2] eq ""}

    assert_match "OK" [R 0 CLUSTER IMPORT-CANCEL LINK $linkname1]

    # One link is closed, migration log says "cancelled"
    assert {![is_link_up 0 $linkname1]}
    assert [is_link_up 0 $linkname2]
    wait_for_link_down 2 $linkname1
    assert [is_link_up 2 $linkname2]
    assert {[dict get [get_migration_log_by_linkname 0 $linkname1] state] eq "cancelled"}
    assert {[dict get [get_migration_log_by_linkname 2 $linkname1] state] eq "cancelled"}
    assert {[get_migration_log_by_linkname 0 $linkname2] eq ""}
    assert {[get_migration_log_by_linkname 2 $linkname2] eq ""}

    assert_match "OK" [R 0 CLUSTER IMPORT-CANCEL LINK $linkname2]

    # Now both links are closed with logs in state "cancelled"
    assert {![is_link_up 0 $linkname1]}
    assert {![is_link_up 0 $linkname2]}
    wait_for_link_down 2 $linkname1
    wait_for_link_down 2 $linkname2
    assert {[dict get [get_migration_log_by_linkname 0 $linkname1] state] eq "cancelled"}
    assert {[dict get [get_migration_log_by_linkname 0 $linkname2] state] eq "cancelled"}
    assert {[dict get [get_migration_log_by_linkname 2 $linkname1] state] eq "cancelled"}
    assert {[dict get [get_migration_log_by_linkname 2 $linkname2] state] eq "cancelled"}
}

set 0_slot_tag "{06S}"
set 5462_slot_tag "{450}"
set 16379_slot_tag "{YY}"
set 16380_slot_tag "{wu}"
set 16381_slot_tag "{0TG}"
set 16382_slot_tag "{4oi}"
set 16383_slot_tag "{6ZJ}"

test "Single source import - one shot" {
    set prev_full_syncs_0 [status [Rn 0] sync_full]
    set prev_full_syncs_2 [status [Rn 2] sync_full]

    # Populate data before migration
    write_data 2 "$16383_slot_tag:" 1000 1000

    # Perform one-shot import
    assert_match "OK" [R 0 CLUSTER IMPORT SLOTSRANGE 16383 16383]
    wait_for_migration 0 16383

    # Keys successfully migrated
    assert_match "1000" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

    # Also eventually reflected in replicas
    wait_for_countkeysinslot 3 16383 1000
    wait_for_countkeysinslot 5 16383 0

    # Migration log shows success on both ends
    assert {[dict get [get_migration_log_by_slot 0 16383] state] eq "success"}
    assert {[dict get [get_migration_log_by_slot 2 16383] state] eq "success"}

    # Did not cause full sync from affected replicas
    assert {$prev_full_syncs_0 eq [status [Rn 0] sync_full]}
    assert {$prev_full_syncs_2 eq [status [Rn 2] sync_full]}

    # Cleanup for next test
    assert_match "OK" [R 0 FLUSHDB SYNC]
}

test "Single source import - two phase" {
    set prev_full_syncs_0 [status [Rn 0] sync_full]
    set prev_full_syncs_2 [status [Rn 2] sync_full]

    # Load data before the snapshot
    write_data 0 "$16383_slot_tag:1:" 333 1000

    # Load data while the snapshot is ongoing
    assert_match "OK" [R 2 CLUSTER IMPORT-PREPARE SLOTSRANGE 16383 16383]
    set linkname [get_link_name 2 16383]
    write_data 0 "$16383_slot_tag:2:" 333 1000

    # Load data after the snapshot
    wait_for_ready_to_commit 2 $linkname
    write_data 0 "$16383_slot_tag:3:" 334 1000

    # Commit and verify
    assert_match "OK" [R 2 CLUSTER IMPORT-COMMIT LINK $linkname]
    wait_for_migration 2 16383
    assert_match "1000" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 0 CLUSTER COUNTKEYSINSLOT 16383]

    # Also eventually reflected in replicas
    wait_for_countkeysinslot 5 16383 1000
    wait_for_countkeysinslot 3 16383 0

    # Migration log shows success on both ends
    assert {[dict get [get_migration_log_by_linkname 0 $linkname] state] eq "success"}
    assert {[dict get [get_migration_log_by_linkname 2 $linkname] state] eq "success"}

    # Did not cause full sync from affected replicas
    assert {$prev_full_syncs_0 eq [status [Rn 0] sync_full]}
    assert {$prev_full_syncs_2 eq [status [Rn 2] sync_full]}

    # Cleanup for the next test
    assert_match "OK" [R 2 FLUSHDB SYNC]
}

test "Simultaneous imports" {
    set prev_full_syncs_0 [status [Rn 0] sync_full]
    set prev_full_syncs_1 [status [Rn 2] sync_full]
    set prev_full_syncs_2 [status [Rn 2] sync_full]

    # Populate data before migration
    write_data 1 "$5462_slot_tag:1:" 100 1000
    write_data 2 "$16383_slot_tag:1:" 100 1000

    # Prepare imports
    assert_match "OK" [R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 5462 5462]
    assert_match "OK" [R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 16383 16383]
    set linkname1 [get_link_name 0 5462]
    set linkname2 [get_link_name 0 16383]
    write_data 1 "$5462_slot_tag:2:" 100 1000
    write_data 2 "$16383_slot_tag:2:" 100 1000
    wait_for_ready_to_commit 0 $linkname1
    wait_for_ready_to_commit 0 $linkname2
    write_data 1 "$5462_slot_tag:3:" 100 1000
    write_data 2 "$16383_slot_tag:3:" 100 1000

    # Do the imports
    assert_match "OK" [R 0 CLUSTER IMPORT-COMMIT LINK $linkname1]
    assert_match "OK" [R 0 CLUSTER IMPORT-COMMIT LINK $linkname2]
    wait_for_migration 0 5462
    wait_for_migration 0 16383
    assert_match "300" [R 0 CLUSTER COUNTKEYSINSLOT 5462]
    assert_match "300" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 1 CLUSTER COUNTKEYSINSLOT 5462]
    assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

    # Also eventually reflected in replicas
    wait_for_countkeysinslot 3 5462 300
    wait_for_countkeysinslot 3 16383 300
    wait_for_countkeysinslot 4 5462 0
    wait_for_countkeysinslot 5 16383 0

    # Migration logs shows success on both ends
    assert {[dict get [get_migration_log_by_linkname 0 $linkname1] state] eq "success"}
    assert {[dict get [get_migration_log_by_linkname 1 $linkname1] state] eq "success"}
    assert {[dict get [get_migration_log_by_linkname 0 $linkname2] state] eq "success"}
    assert {[dict get [get_migration_log_by_linkname 2 $linkname2] state] eq "success"}

    # Did not cause full sync from affected replicas
    assert {$prev_full_syncs_0 eq [status [Rn 0] sync_full]}
    assert {$prev_full_syncs_1 eq [status [Rn 1] sync_full]}
    assert {$prev_full_syncs_2 eq [status [Rn 2] sync_full]}

    assert_match "OK" [R 0 FLUSHDB SYNC]
}

test "Simultaneous exports" {
    set prev_full_syncs_0 [status [Rn 0] sync_full]
    set prev_full_syncs_1 [status [Rn 1] sync_full]
    set prev_full_syncs_2 [status [Rn 2] sync_full]

    # Populate data before migration
    write_data 0 "$5462_slot_tag:1:" 100 1000
    write_data 0 "$16383_slot_tag:1:" 100 1000

    # Prepare imports
    assert_match "OK" [R 1 CLUSTER IMPORT-PREPARE SLOTSRANGE 5462 5462]
    assert_match "OK" [R 2 CLUSTER IMPORT-PREPARE SLOTSRANGE 16383 16383]
    set linkname1 [get_link_name 1 5462]
    set linkname2 [get_link_name 2 16383]
    write_data 0 "$5462_slot_tag:2:" 100 1000
    write_data 0 "$16383_slot_tag:2:" 100 1000
    wait_for_ready_to_commit 1 $linkname1
    wait_for_ready_to_commit 2 $linkname2
    write_data 0 "$5462_slot_tag:3:" 100 1000
    write_data 0 "$16383_slot_tag:3:" 100 1000

    # Do the imports
    assert_match "OK" [R 1 CLUSTER IMPORT-COMMIT LINK $linkname1]
    assert_match "OK" [R 2 CLUSTER IMPORT-COMMIT LINK $linkname2]
    wait_for_migration 1 5462
    wait_for_migration 2 16383
    assert_match "300" [R 1 CLUSTER COUNTKEYSINSLOT 5462]
    assert_match "300" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 0 CLUSTER COUNTKEYSINSLOT 5462]
    assert_match "0" [R 0 CLUSTER COUNTKEYSINSLOT 16383]

    # Also eventually reflected in replicas
    wait_for_countkeysinslot 4 5462 300
    wait_for_countkeysinslot 5 16383 300
    wait_for_countkeysinslot 3 5462 0
    wait_for_countkeysinslot 3 16383 0

    # Migration logs shows success on both ends
    assert {[dict get [get_migration_log_by_linkname 0 $linkname1] state] eq "success"}
    assert {[dict get [get_migration_log_by_linkname 1 $linkname1] state] eq "success"}
    assert {[dict get [get_migration_log_by_linkname 0 $linkname2] state] eq "success"}
    assert {[dict get [get_migration_log_by_linkname 2 $linkname2] state] eq "success"}

    # Did not cause full sync from affected replicas
    assert {$prev_full_syncs_0 eq [status [Rn 0] sync_full]}
    assert {$prev_full_syncs_1 eq [status [Rn 1] sync_full]}
    assert {$prev_full_syncs_2 eq [status [Rn 2] sync_full]}

    # Cleanup for next test
    assert_match "OK" [R 1 FLUSHDB SYNC]
    assert_match "OK" [R 2 FLUSHDB SYNC]
}

test "Multiple slot ranges from same source" {
    set prev_full_syncs_0 [status [Rn 0] sync_full]
    set prev_full_syncs_2 [status [Rn 2] sync_full]

    # Populate data before migration
    write_data 2 "$16382_slot_tag:1:" 100 1000
    write_data 2 "$16383_slot_tag:1:" 100 1000

    # Prepare imports
    assert_match "OK" [R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 16382 16382]
    assert_match "OK" [R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 16383 16383]
    set linkname1 [get_link_name 0 16382]
    set linkname2 [get_link_name 0 16383]
    write_data 2 "$16382_slot_tag:2:" 100 1000
    write_data 2 "$16383_slot_tag:2:" 100 1000
    wait_for_ready_to_commit 0 $linkname1
    wait_for_ready_to_commit 0 $linkname2
    write_data 2 "$16382_slot_tag:3:" 100 1000
    write_data 2 "$16383_slot_tag:3:" 100 1000

    # Do the imports
    assert_match "OK" [R 0 CLUSTER IMPORT-COMMIT LINK $linkname1]
    assert_match "OK" [R 0 CLUSTER IMPORT-COMMIT LINK $linkname2]
    wait_for_migration 0 16382
    wait_for_migration 0 16383
    assert_match "300" [R 0 CLUSTER COUNTKEYSINSLOT 16382]
    assert_match "300" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16382]
    assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

    # Also eventually reflected in replicas
    wait_for_countkeysinslot 3 16382 300
    wait_for_countkeysinslot 3 16383 300
    wait_for_countkeysinslot 5 16382 0
    wait_for_countkeysinslot 5 16383 0

    # Migration logs shows success on both ends
    assert {[dict get [get_migration_log_by_linkname 0 $linkname1] state] eq "success"}
    assert {[dict get [get_migration_log_by_linkname 2 $linkname1] state] eq "success"}
    assert {[dict get [get_migration_log_by_linkname 0 $linkname2] state] eq "success"}
    assert {[dict get [get_migration_log_by_linkname 2 $linkname2] state] eq "success"}

    # Did not cause full sync from affected replicas
    assert {$prev_full_syncs_0 eq [status [Rn 0] sync_full]}
    assert {$prev_full_syncs_2 eq [status [Rn 2] sync_full]}

    assert_match "OK" [R 0 FLUSHDB SYNC]
}

test "Import slot range with multiple slots" {
    set prev_full_syncs_0 [status [Rn 0] sync_full]
    set prev_full_syncs_2 [status [Rn 2] sync_full]

    # Populate data before migration
    write_data 0 "$16382_slot_tag:" 500 1000
    write_data 0 "$16383_slot_tag:" 500 1000

    # Perform one-shot import
    assert_match "OK" [R 2 CLUSTER IMPORT SLOTSRANGE 16382 16383]
    wait_for_migration 2 16383

    # Keys successfully migrated
    assert_match "500" [R 2 CLUSTER COUNTKEYSINSLOT 16382]
    assert_match "500" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 0 CLUSTER COUNTKEYSINSLOT 16382]
    assert_match "0" [R 0 CLUSTER COUNTKEYSINSLOT 16383]

    # Also eventually reflected in replicas
    wait_for_countkeysinslot 5 16382 500
    wait_for_countkeysinslot 5 16383 500
    wait_for_countkeysinslot 3 16382 0
    wait_for_countkeysinslot 3 16383 0

    # Migration logs shows success on both ends
    assert {[dict get [get_migration_log_by_slot 0 16383] state] eq "success"}
    assert {[dict get [get_migration_log_by_slot 2 16383] state] eq "success"}

    # Did not cause full sync from affected replicas
    assert {$prev_full_syncs_0 eq [status [Rn 0] sync_full]}
    assert {$prev_full_syncs_2 eq [status [Rn 2] sync_full]}

    # Cleanup for next test
    assert_match "OK" [R 2 FLUSHDB SYNC]
}

test "Import multiple slot ranges with multiple slots" {
    set prev_full_syncs_0 [status [Rn 0] sync_full]
    set prev_full_syncs_2 [status [Rn 2] sync_full]

    # Populate data before migration
    write_data 2 "$16379_slot_tag:" 250 1000
    write_data 2 "$16380_slot_tag:" 250 1000
    write_data 2 "$16382_slot_tag:" 250 1000
    write_data 2 "$16383_slot_tag:" 250 1000

    # Perform one-shot import
    assert_match "OK" [R 0 CLUSTER IMPORT SLOTSRANGE 16379 16380 16382 16383]
    wait_for_migration 0 16383

    # Keys successfully migrated
    assert_match "250" [R 0 CLUSTER COUNTKEYSINSLOT 16379]
    assert_match "250" [R 0 CLUSTER COUNTKEYSINSLOT 16380]
    assert_match "250" [R 0 CLUSTER COUNTKEYSINSLOT 16382]
    assert_match "250" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16379]
    assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16380]
    assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16382]
    assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

    # Also eventually reflected in replicas
    wait_for_countkeysinslot 3 16379 250
    wait_for_countkeysinslot 3 16380 250
    wait_for_countkeysinslot 3 16382 250
    wait_for_countkeysinslot 3 16383 250
    wait_for_countkeysinslot 5 16379 0
    wait_for_countkeysinslot 5 16380 0
    wait_for_countkeysinslot 5 16382 0
    wait_for_countkeysinslot 5 16383 0

    # Migration logs shows success on both ends
    assert {[dict get [get_migration_log_by_slot 0 16383] state] eq "success"}
    assert {[dict get [get_migration_log_by_slot 2 16383] state] eq "success"}

    # Did not cause full sync from affected replicas
    assert {$prev_full_syncs_0 eq [status [Rn 0] sync_full]}
    assert {$prev_full_syncs_2 eq [status [Rn 2] sync_full]}

    # Cleanup for next test
    assert_match "OK" [R 0 FLUSHDB SYNC]
}

test "Export all slots from node" {
    set prev_full_syncs_0 [status [Rn 0] sync_full]
    set prev_full_syncs_2 [status [Rn 2] sync_full]

    # Populate data before migration
    write_data 0 "$16383_slot_tag:" 1000 1000

    # Perform one-shot import
    assert_match "OK" [R 2 CLUSTER IMPORT SLOTSRANGE 0 5461 16379 16380 16382 16383]
    wait_for_migration 2 16383

    # Keys successfully migrated
    assert_match "1000" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 0 CLUSTER COUNTKEYSINSLOT 16383]

    # Also eventually reflected in replicas
    wait_for_countkeysinslot 5 16383 1000
    wait_for_countkeysinslot 3 16383 0

    # Migration logs shows success on both ends
    assert {[dict get [get_migration_log_by_slot 0 0] state] eq "success"}
    assert {[dict get [get_migration_log_by_slot 2 0] state] eq "success"}

    # Did not cause full sync from affected replicas
    assert {$prev_full_syncs_0 eq [status [Rn 0] sync_full]}
    assert {$prev_full_syncs_2 eq [status [Rn 2] sync_full]}

    # Cleanup for next test
    assert_match "OK" [R 2 FLUSHDB SYNC]
}

test "Import slots to node with no slots" {
    set prev_full_syncs_0 [status [Rn 0] sync_full]
    set prev_full_syncs_2 [status [Rn 2] sync_full]

    # Populate data before migration
    write_data 2 "$0_slot_tag:" 1000 1000

    # Perform one-shot import
    assert_match "OK" [R 0 CLUSTER IMPORT SLOTSRANGE 0 5461]
    wait_for_migration 0 5461

    # Keys successfully migrated
    assert_match "1000" [R 0 CLUSTER COUNTKEYSINSLOT 0]
    assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 0]

    # Also eventually reflected in replicas
    wait_for_countkeysinslot 3 0 1000
    wait_for_countkeysinslot 5 0 0

    # Migration logs shows success on both ends
    assert {[dict get [get_migration_log_by_slot 0 0] state] eq "success"}
    assert {[dict get [get_migration_log_by_slot 2 0] state] eq "success"}

    # Did not cause full sync from affected replicas
    assert {$prev_full_syncs_0 eq [status [Rn 0] sync_full]}
    assert {$prev_full_syncs_2 eq [status [Rn 2] sync_full]}

    # Cleanup for next test
    assert_match "OK" [R 0 FLUSHDB SYNC]
}

test "Partial data removed on cancel" {
    set prev_full_syncs_0 [status [Rn 0] sync_full]
    set prev_full_syncs_2 [status [Rn 2] sync_full]

    # Load data before the snapshot
    write_data 2 "$16383_slot_tag:1:" 333 1000

    # Load data while the snapshot is ongoing
    assert_match "OK" [R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 16383 16383]
    set linkname [get_link_name 0 16383]
    write_data 2 "$16383_slot_tag:2:" 333 1000

    # Load data after the snapshot
    wait_for_ready_to_commit 2 $linkname
    write_data 2 "$16383_slot_tag:3:" 334 1000

    # Cancel and the data should be dropped
    assert_match "OK" [R 0 CLUSTER IMPORT-CANCEL LINK $linkname]
    assert {![is_link_up 0 $linkname]}
    assert_match "0" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "1000" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

    # Also eventually reflected in replicas
    wait_for_countkeysinslot 3 16383 0
    wait_for_countkeysinslot 5 16383 1000

    # Migration logs shows cancelled on both ends
    assert {[dict get [get_migration_log_by_linkname 0 $linkname] state] eq "cancelled"}
    assert {[dict get [get_migration_log_by_linkname 2 $linkname] state] eq "cancelled"}

    # Did not cause full sync from affected replicas
    assert {$prev_full_syncs_0 eq [status [Rn 0] sync_full]}
    assert {$prev_full_syncs_2 eq [status [Rn 2] sync_full]}

    # Cleanup for the next test
    assert_match "OK" [R 2 FLUSHDB SYNC]
}

test "OOM on target aborts migration" {
    set prev_full_syncs_0 [status [Rn 0] sync_full]
    set prev_full_syncs_2 [status [Rn 2] sync_full]

    # Load some data before the snapshot
    write_data 2 "$16383_slot_tag:1:" 500 1000
    assert_match "OK" [R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 16383 16383]
    set linkname [get_link_name 0 16383]
    wait_for_ready_to_commit 0 $linkname

    # Set maxmemory to simulate OOM
    assert_match "OK" [R 0 CONFIG SET maxmemory 1]

    # Loading more data should cause a failure
    write_data 2 "$16383_slot_tag:3:" 500 1000
    assert {![is_link_up 0 $linkname]}

    # Verify the keys are eventually dropped on target
    assert_match "1000" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
    wait_for_countkeysinslot 0 16383 0

    # Also eventually reflected in replicas
    wait_for_countkeysinslot 5 16383 1000
    wait_for_countkeysinslot 3 16383 0

    # Migration logs shows failure on both ends
    assert {[dict get [get_migration_log_by_linkname 0 $linkname] state] eq "failed"}
    assert {[string match {*OOM*} [dict get [get_migration_log_by_linkname 0 $linkname] message]]}
    assert {[dict get [get_migration_log_by_linkname 2 $linkname] state] eq "failed"}
    assert {[string match {*Connection lost to target*} [dict get [get_migration_log_by_linkname 2 $linkname] message]]}

    # Did not cause full sync from affected replicas
    assert {$prev_full_syncs_0 eq [status [Rn 0] sync_full]}
    assert {$prev_full_syncs_2 eq [status [Rn 2] sync_full]}

    # Cleanup for the next test
    assert_match "OK" [R 0 CONFIG SET maxmemory 0]
    assert_match "OK" [R 2 FLUSHDB SYNC]
}

test "Partial data in replica removed on failover" {
    # Load some data before the snapshot
    write_data 2 "$16383_slot_tag:1:" 500 1000

    # Prepare and wait for ready
    assert_match "OK" [R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 16383 16383]
    set linkname [get_link_name 0 16383]
    wait_for_ready_to_commit 0 $linkname

    # Make sure the replica has it
    wait_for_countkeysinslot 3 16383 500

    # Trigger failover
    assert_match "OK" [R 3 CLUSTER FAILOVER]

    # Links should be dropped on both ends
    wait_for_link_down 0 $linkname
    wait_for_link_down 2 $linkname

    # Keys should be dropped in target shard
    assert_match "0" [R 3 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 0 CLUSTER COUNTKEYSINSLOT 16383]

    # Keys on existing shard are untouched
    assert_match "500" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "500" [R 5 CLUSTER COUNTKEYSINSLOT 16383]

    # Migration logs shows failure on both ends
    assert {[dict get [get_migration_log_by_linkname 0 $linkname] state] eq "failed"}
    assert {[string match {*I was demoted to a replica*} [dict get [get_migration_log_by_linkname 0 $linkname] message]]}
    assert {[dict get [get_migration_log_by_linkname 2 $linkname] state] eq "failed"}
    assert {[string match {*Connection lost to target*} [dict get [get_migration_log_by_linkname 2 $linkname] message]]}

    # Cleanup for the next test
    assert_match "OK" [R 2 FLUSHDB SYNC]
}

test "Slot export failed on failover" {
    # Load some data before the snapshot
    write_data 3 "$0_slot_tag:1:" 500 1000

    # Prepare and wait for ready
    assert_match "OK" [R 2 CLUSTER IMPORT-PREPARE SLOTSRANGE 0 0]
    set linkname [get_link_name 2 0]
    wait_for_ready_to_commit 2 $linkname

    # Trigger failover
    assert_match "OK" [R 0 CLUSTER FAILOVER]

    # Links should be dropped on both ends
    wait_for_link_down 3 $linkname
    wait_for_link_down 2 $linkname

    # Keys should be dropped in target shard
    assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 0]
    wait_for_countkeysinslot 5 0 0

    # Keys on existing shard are untouched
    assert_match "500" [R 3 CLUSTER COUNTKEYSINSLOT 0]
    assert_match "500" [R 0 CLUSTER COUNTKEYSINSLOT 0]

    # Migration logs shows failure on both ends
    assert {[dict get [get_migration_log_by_linkname 3 $linkname] state] eq "failed"}
    # assert {[string match {*Slots are no longer owned by myself*} [dict get [get_migration_log_by_linkname 3 $linkname] message]]}
    assert {[dict get [get_migration_log_by_linkname 2 $linkname] state] eq "failed"}
    # assert {[string match {*Slots are no longer owned by source node*} [dict get [get_migration_log_by_linkname 2 $linkname] message]]}

    # Cleanup for the next test
    assert_match "OK" [R 0 FLUSHDB SYNC]
}

test "Slots split across shards during import" {
    # Load some data before the snapshot
    write_data 0 "$0_slot_tag:1:" 500 1000

    # Prepare and wait for ready
    assert_match "OK" [R 1 CLUSTER IMPORT-PREPARE SLOTSRANGE 0 0]
    assert_match "OK" [R 2 CLUSTER IMPORT-PREPARE SLOTSRANGE 0 1]
    set linkname1 [get_link_name 1 0]
    set linkname2 [get_link_name 2 0]
    wait_for_ready_to_commit 1 $linkname1
    wait_for_ready_to_commit 2 $linkname2

    # Trigger commit
    assert_match "OK" [R 1 CLUSTER IMPORT-COMMIT LINK $linkname1]
    wait_for_migration 1 0

    # Second link should get dropped on either end
    wait_for_link_down 2 $linkname2
    wait_for_link_down 0 $linkname2

    # Keys should be dropped on cancelled link
    assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 0]
    assert_match "0" [R 5 CLUSTER COUNTKEYSINSLOT 0]

    # Migration logs shows success on both ends for linkname1
    assert {[dict get [get_migration_log_by_linkname 0 $linkname1] state] eq "success"}
    assert {[dict get [get_migration_log_by_linkname 1 $linkname1] state] eq "success"}

    # Migration logs for linkname2 shows failure on both ends
    assert {[dict get [get_migration_log_by_linkname 0 $linkname2] state] eq "failed"}
    # assert {[string match {*Slots are no longer owned by myself*} [dict get [get_migration_log_by_linkname 0 $linkname2] message]]}
    assert {[dict get [get_migration_log_by_linkname 2 $linkname2] state] eq "failed"}
    # assert {[string match {*Slots are no longer owned by source node*} [dict get [get_migration_log_by_linkname 2 $linkname2] message]]}

    # Cleanup for the next test
    assert_match "OK" [R 1 FLUSHDB SYNC]
}

set fake_linkname "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
set node2_id [R 2 CLUSTER MYID]

test "Export same slot to two nodes" {
    # Pretend to be another node in order to acquire the FAILOVER-GRANTED state
    set client [valkey_client_by_addr [srv -1 host] [srv -1 port]]
    $client CLUSTER SYNCSLOTS SNAPSHOT TARGET $node2_id LINKNAME $fake_linkname SLOTSRANGE 0 0
    $client CLUSTER SYNCSLOTS PAUSE
    $client CLUSTER SYNCSLOTS REQUEST-FAILOVER

    # Prepare another export at the same time
    assert_match "OK" [R 0 CLUSTER IMPORT-PREPARE SLOTSRANGE 0 0]
    set linkname [get_link_name 0 0]
    wait_for_ready_to_commit 0 $linkname

    # Trigger commit
    assert_match "OK" [R 0 CLUSTER IMPORT-COMMIT LINK $linkname]

    # Link should still be up
    wait_for_ready_to_commit 0 $linkname
    assert {[is_link_up 0 $linkname]}
    assert {[string match {*failover attempt denied*} [dict get [get_migration_by_linkname 0 $linkname] message]]}

    # Now drop our fake link
    $client deferred 1
    $client CLUSTER SYNCSLOTS CANCEL
    $client flush
    $client close
    wait_for_link_down 1 $fake_linkname

    # Now, commit should succeed
    assert_match "OK" [R 0 CLUSTER IMPORT-COMMIT LINK $linkname]
    wait_for_migration 0 0

    # Migration logs shows success on both ends
    assert {[dict get [get_migration_log_by_linkname 0 $linkname] state] eq "success"}
    assert {[dict get [get_migration_log_by_linkname 1 $linkname] state] eq "success"}

    # Migration logs for fake link shows failure
    assert {[dict get [get_migration_log_by_linkname 1 $fake_linkname] state] eq "cancelled"}
}

test "One shot import gives up on FAILOVER-DENIED" {
    # Pretend to be another node in order to acquire the FAILOVER-GRANTED state
    set client [valkey_client_by_addr [srv 0 host] [srv 0 port]]
    $client CLUSTER SYNCSLOTS SNAPSHOT TARGET $node2_id LINKNAME $fake_linkname SLOTSRANGE 0 0
    $client CLUSTER SYNCSLOTS PAUSE
    $client CLUSTER SYNCSLOTS REQUEST-FAILOVER

    # Prepare another export at the same time
    assert_match "OK" [R 1 CLUSTER IMPORT SLOTSRANGE 0 0]

    # Should fail since it can't get the FAILOVER-GRANTED response
    wait_for_link_down_by_slot 1 0

    # Migration logs should show failure
    assert {[dict get [get_migration_log_by_slot 1 0] state] eq "failed"}
    assert {[string match {*Failover denied*} [dict get [get_migration_log_by_slot 1 0] message]]}

    # Close the fake link now
    $client deferred 1
    $client CLUSTER SYNCSLOTS CANCEL
    $client flush
    $client close
    wait_for_link_down 0 $fake_linkname
}

test "Export unpauses itself even if failover doesn't occur" {
    # Lower manual failover timeout for this test
    set mf_timeout_old [lindex [R 0 CONFIG GET cluster-manual-failover-timeout] 1]
    R 0 CONFIG SET cluster-manual-failover-timeout 100

    # Pretend to be another node in order to acquire the FAILOVER-GRANTED state
    set node2_id [R 2 CLUSTER MYID]
    set client [valkey_client_by_addr [srv 0 host] [srv 0 port]]
    $client CLUSTER SYNCSLOTS SNAPSHOT TARGET $node2_id LINKNAME $fake_linkname SLOTSRANGE 0 0
    $client CLUSTER SYNCSLOTS PAUSE
    $client CLUSTER SYNCSLOTS REQUEST-FAILOVER

    # Our fake link should get dropped after pause timeout expires
    wait_for_link_down 0 $fake_linkname
    $client close

    # Migration logs for fake link shows failure
    assert {[dict get [get_migration_log_by_linkname 0 $fake_linkname] state] eq "failed"}
    assert {[string match {*Unpaused before migration completed*} [dict get [get_migration_log_by_linkname 0 $fake_linkname] message]]}

    # Validate no longer paused
    write_data 0 "$0_slot_tag:" 1 1000

    # Reset manual failover timeout
    R 0 CONFIG SET cluster-manual-failover-timeout $mf_timeout_old

    # Cleanup for the next test
    assert_match "OK" [R 0 FLUSHDB SYNC]
}

proc assert_causes_syncslots_fail {node_idx args} {
    set client [valkey_client_by_addr [srv -$node_idx host] [srv -$node_idx port]]
    assert_match "*CLUSTER SYNCSLOTS FAIL*" [$client {*}$args]
    $client deferred 1
    catch {
        $client read
    } result
    $client deferred 0
    $client close
    assert_match "*I/O error reading reply*" $result
}

proc assert_causes_conn_drop {node_idx args} {
    set client [valkey_client_by_addr [srv -$node_idx host] [srv -$node_idx port]]
    catch {
        $client {*}$args
    } result
    $client close
    assert_match "*I/O error reading reply*" $result
}

test "CLUSTER SYNCSLOTS invalid state machine traversal" {
    # Invalid state machine traversal on export node will send SYNCSLOTS FAIL
    assert_causes_syncslots_fail 0 CLUSTER SYNCSLOTS PAUSE
    assert_causes_syncslots_fail 0 CLUSTER SYNCSLOTS STREAM
    assert_causes_syncslots_fail 0 CLUSTER SYNCSLOTS REQUEST-FAILOVER

    # Invalid state machine traversal on import node just drops connection
    assert_causes_conn_drop 0 CLUSTER SYNCSLOTS SNAPSHOT-EOF
    assert_causes_conn_drop 0 CLUSTER SYNCSLOTS PAUSED
    assert_causes_conn_drop 0 CLUSTER SYNCSLOTS FAILOVER-GRANTED
    assert_causes_conn_drop 0 CLUSTER SYNCSLOTS FAILOVER-DENIED
    assert_causes_conn_drop 0 CLUSTER SYNCSLOTS ACK
    assert_causes_conn_drop 0 CLUSTER SYNCSLOTS CANCEL
    assert_causes_conn_drop 0 CLUSTER SYNCSLOTS FAIL
}

test "CLUSTER SYNCSLOTS SNAPSHOT command interface" {
    # No arguments
    assert_causes_syncslots_fail 0 CLUSTER SYNCSLOTS SNAPSHOT

    # No target
    assert_causes_syncslots_fail 0 CLUSTER SYNCSLOTS SNAPSHOT LINKNAME $fake_linkname SLOTSRANGE 0 0

    # No linkname
    assert_causes_syncslots_fail 0 CLUSTER SYNCSLOTS SNAPSHOT TARGET $node2_id SLOTSRANGE 0 0

    # No slotsrange
    assert_causes_syncslots_fail 0 CLUSTER SYNCSLOTS SNAPSHOT TARGET $node2_id LINKNAME $fake_linkname

    # No end slot
    assert_causes_syncslots_fail 0 CLUSTER SYNCSLOTS SNAPSHOT TARGET $node2_id LINKNAME $fake_linkname SLOTSRANGE 0

    # Unknown target
    assert_causes_syncslots_fail 0 CLUSTER SYNCSLOTS SNAPSHOT TARGET $fake_linkname LINKNAME $fake_linkname SLOTSRANGE 0 0

    # Unowned slotsrange
    assert_causes_syncslots_fail 0 CLUSTER SYNCSLOTS SNAPSHOT TARGET $node2_id LINKNAME $fake_linkname SLOTSRANGE 16383 16383

    # Not primary
    assert_causes_syncslots_fail 3 CLUSTER SYNCSLOTS SNAPSHOT TARGET $node2_id LINKNAME $fake_linkname SLOTSRANGE 0 0

    # Invalid target name
    assert_causes_syncslots_fail 0 CLUSTER SYNCSLOTS SNAPSHOT TARGET invalid LINKNAME $fake_linkname SLOTSRANGE 0 0

    # Invalid link name
    assert_causes_syncslots_fail 0 CLUSTER SYNCSLOTS SNAPSHOT TARGET $node2_id LINKNAME invalid SLOTSRANGE 0 0
}

test "Import cancelled when source hangs" {

}

test "Export cancelled when target hangs" {

}

test "Import with AUTH on" {
}

test "Import cannot connect to source" {

}

test "Connection drop during import causes failure in one-shot mode" {

}

test "Connection drop during import is reconnected in two-phase mode" {

}

test "Import failed if SETSLOT is used" {

}

test "Export failed if SETSLOT is used" {

}

test "Import backs off if source sends FAIL" {

}

test "FLUSHDB with partially imported slot" {
    
}

}
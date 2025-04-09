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

proc wait_for_migration {node_idx slot} {
    set target_id [R $node_idx CLUSTER MYID]
    wait_for_condition 100 100 {
        [is_slot_migrated $node_idx $slot]
    } else {
        fail "Cluster node $target_id did not get slot %slot within 10000 ms"
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
    puts [get_migration_log_by_linkname 2 $linkname1]
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

    # Also reflected in replicas
    assert_match "1000" [R 3 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 5 CLUSTER COUNTKEYSINSLOT 16383]

    # Migration log shows success on both ends
    puts [get_migration_log_by_slot 0 16383]
    assert {[dict get [get_migration_log_by_slot 0 16383] state] eq "success"}
    puts [get_migration_log_by_slot 2 16383]
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
    assert_match "1000" [R 5 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 3 CLUSTER COUNTKEYSINSLOT 16383]

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

    # Also reflected in replicas
    assert_match "300" [R 3 CLUSTER COUNTKEYSINSLOT 5462]
    assert_match "300" [R 3 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 4 CLUSTER COUNTKEYSINSLOT 5462]
    assert_match "0" [R 5 CLUSTER COUNTKEYSINSLOT 16383]

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

    # Also reflected in replicas
    assert_match "300" [R 4 CLUSTER COUNTKEYSINSLOT 5462]
    assert_match "300" [R 5 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 3 CLUSTER COUNTKEYSINSLOT 5462]
    assert_match "0" [R 3 CLUSTER COUNTKEYSINSLOT 16383]

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

    # Also reflected in replicas
    assert_match "300" [R 3 CLUSTER COUNTKEYSINSLOT 16382]
    assert_match "300" [R 3 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 5 CLUSTER COUNTKEYSINSLOT 16382]
    assert_match "0" [R 5 CLUSTER COUNTKEYSINSLOT 16383]

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

    # Also reflected in replicas
    assert_match "500" [R 5 CLUSTER COUNTKEYSINSLOT 16382]
    assert_match "500" [R 5 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 3 CLUSTER COUNTKEYSINSLOT 16382]
    assert_match "0" [R 3 CLUSTER COUNTKEYSINSLOT 16383]

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

    # Also reflected in replicas
    assert_match "250" [R 3 CLUSTER COUNTKEYSINSLOT 16379]
    assert_match "250" [R 3 CLUSTER COUNTKEYSINSLOT 16380]
    assert_match "250" [R 3 CLUSTER COUNTKEYSINSLOT 16382]
    assert_match "250" [R 3 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 5 CLUSTER COUNTKEYSINSLOT 16379]
    assert_match "0" [R 5 CLUSTER COUNTKEYSINSLOT 16380]
    assert_match "0" [R 5 CLUSTER COUNTKEYSINSLOT 16382]
    assert_match "0" [R 5 CLUSTER COUNTKEYSINSLOT 16383]

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

    # Also reflected in replicas
    assert_match "1000" [R 5 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 3 CLUSTER COUNTKEYSINSLOT 16383]

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

    # Also reflected in replicas
    assert_match "1000" [R 3 CLUSTER COUNTKEYSINSLOT 0]
    assert_match "0" [R 5 CLUSTER COUNTKEYSINSLOT 0]

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
    assert_match "0" [R 3 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "1000" [R 5 CLUSTER COUNTKEYSINSLOT 16383]

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
    set before_maxmemory [R 0 CONFIG GET maxmemory]
    assert_match "OK" [R 0 CONFIG SET maxmemory 1]

    # Loading more data should cause a failure
    write_data 2 "$16383_slot_tag:3:" 500 1000
    assert {![is_link_up 0 $linkname]}

    # Verify the keys are dropped on target
    assert_match "1000" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "1000" [R 5 CLUSTER COUNTKEYSINSLOT 16383]
    assert_match "0" [R 3 CLUSTER COUNTKEYSINSLOT 16383]

    # Did not cause full sync from affected replicas
    assert {$prev_full_syncs_0 eq [status [Rn 0] sync_full]}
    assert {$prev_full_syncs_2 eq [status [Rn 2] sync_full]}

    # Cleanup for the next test
    assert_match "OK" [R 2 FLUSHDB SYNC]
}

test "Partial data in replica removed on failover" {

}

test "Import with IO threads" {

}

test "Import on TLS connection" {

}

}
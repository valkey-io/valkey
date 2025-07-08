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

proc wait_for_migration_field {node_idx linkname field value} {
    wait_for_condition 100 100 {
        [get_migration_by_linkname $node_idx $linkname] ne "" && [dict get [get_migration_by_linkname $node_idx $linkname] $field] eq $value
    } else {
        set curr_state [get_migration_by_linkname $node_idx $linkname]
        fail "Migration $linkname on node $node_idx did not have $field == $value (currently $curr_state) within 10000 ms"
    }
}

proc wait_for_countkeysinslot {node_idx slot value} {
    wait_for_condition 100 100 {
        [R $node_idx CLUSTER COUNTKEYSINSLOT $slot] eq "$value"
    } else {
        set curr_count [R $node_idx CLUSTER COUNTKEYSINSLOT $slot]
        fail "Node $node_idx did not have $value keys in slot $slot within 10000 ms (current $curr_count)"
    }
}

proc wait_for_migration {node_idx slot} {
    set target_id [R $node_idx CLUSTER MYID]
    wait_for_condition 100 100 {
        [is_slot_migrated $node_idx $slot]
    } else {
        set nodes [get_cluster_nodes $node_idx]
        fail "Cluster node $target_id did not get slot $slot within 10000 ms (current $nodes)"
    }
    wait_for_cluster_propagation
}

proc get_cluster_total_syncs_count {} {
    set total 0
    foreach node {0 1 2 3 4 5} {
        set total [expr [status [Rn $node] sync_full] + $total]
        set total [expr [status [Rn $node] sync_partial_ok] + $total]
        set total [expr [status [Rn $node] sync_partial_err] + $total]
    }
    return $total
}

# Helper to wrap a test, and assert it doesn't cause a resync
proc assert_does_not_resync {body} {
    set prev_syncs [get_cluster_total_syncs_count]
    uplevel 1 $body
    assert_equal $prev_syncs [get_cluster_total_syncs_count]
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

proc set_debug_prevent_pause {value} {
    for {set i 0} {$i < [llength $::servers]} {incr i} {
        assert_match "OK" [R $i DEBUG SLOTMIGRATION PREVENT-PAUSE $value]
    }
}

proc set_debug_prevent_failover {value} {
    for {set i 0} {$i < [llength $::servers]} {incr i} {
        assert_match "OK" [R $i DEBUG SLOTMIGRATION PREVENT-FAILOVER $value]
    }
}

# Disable replica migration to prevent empty nodes from joining other shards.
start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no cluster-node-timeout 15000 cluster-databases 16}} {

    set node0_id [R 0 CLUSTER MYID]
    set node1_id [R 1 CLUSTER MYID]
    set node2_id [R 2 CLUSTER MYID]
    set fake_linkname "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

    test "General command interface" {
        assert_error "*wrong number of arguments*" {R 0 CLUSTER MIGRATE}
        assert_error "*syntax error*" {R 0 CLUSTER MIGRATE INVALID 0 1}
        assert_error "*wrong number of arguments*" {R 0 CLUSTER MIGRATE SLOTSRANGE}
        assert_error "*No end slot for final slot range*" {R 0 CLUSTER MIGRATE SLOTSRANGE 0}
        assert_error "*Invalid or out of range slot*" {R 0 CLUSTER MIGRATE SLOTSRANGE 16385 16388}
        assert_error "*Invalid or out of range slot*" {R 0 CLUSTER MIGRATE SLOTSRANGE 16380 16388}
        assert_error "*No slot ranges specified*" {R 0 CLUSTER MIGRATE SLOTSRANGE a 0}
        assert_error "*Invalid or out of range slot*" {R 0 CLUSTER MIGRATE SLOTSRANGE 0 a}
        assert_error "*Start slot number 1 is greater than end slot number 0*" {R 0 CLUSTER MIGRATE SLOTSRANGE 1 0}
        assert_error "*The slot ranges are not all owned by the same node*" {R 0 CLUSTER MIGRATE SLOTSRANGE 0 16383}
        assert_error "*Slot range 3-6 overlaps with previous range 0-5*" {R 0 CLUSTER MIGRATE SLOTSRANGE 0 5 3 6}
        assert_error "*Slot range 0-5 overlaps with previous range 3-6*" {R 0 CLUSTER MIGRATE SLOTSRANGE 3 6 0 5}
        assert_error "*syntax error*" {R 0 CLUSTER MIGRATE SLOTSRANGE 0 0}
        assert_error "*syntax error*" {R 0 CLUSTER MIGRATE SLOTSRANGE 0 0 NODE}

        set source_node_id [R 0 CLUSTER MYID]
        set target_node_id [R 1 CLUSTER MYID]
        R 0 CLUSTER SETSLOT 0 MIGRATING $target_node_id
        R 1 CLUSTER SETSLOT 0 IMPORTING $source_node_id
        assert_error "*Some slots are being manually migrated*" {R 0 CLUSTER MIGRATE SLOTSRANGE 16383 16383}
        assert_error "*Some slots are being manually imported*" {R 1 CLUSTER MIGRATE SLOTSRANGE 16383 16383}
        R 0 CLUSTER SETSLOT 0 STABLE
        R 1 CLUSTER SETSLOT 0 STABLE

        R 0 CLUSTER DELSLOTS 0
        assert_error "*Slot 0 has no node served*" {R 0 CLUSTER MIGRATE SLOTSRANGE 0 0}
        R 0 CLUSTER ADDSLOTS 0

        assert_error "*Slot migration can only be used on primary nodes*" {R 3 CLUSTER MIGRATE SLOTSRANGE 0 0}
        assert_error "*Slots are not served by myself*" {R 2 CLUSTER MIGRATE SLOTSRANGE 0 0 NODE $node0_id}

        assert_error "*wrong number of arguments*" {R 0 CLUSTER CANCELMIGRATION}
        assert_error "*No migrations ongoing*" {R 0 CLUSTER CANCELMIGRATION ALL}
        assert_error "*syntax error*" {R 0 CLUSTER CANCELMIGRATION LINK}
        assert_error "*No outgoing migration with link name found*" {R 0 CLUSTER CANCELMIGRATION LINK abcdef}
    }

    test "CLUSTER MIGRATE already migrating" {
        set_debug_prevent_pause 1
        assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
        set linkname [get_link_name 2 16383]
        assert_error "*I am already migrating slot 16383*" {R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id}
        R 2 CLUSTER CANCELMIGRATION LINK $linkname
        wait_for_migration_field 0 $linkname state failed

        assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16381 16383 NODE $node0_id]
        set linkname [get_link_name 2 16381]
        assert_error "*I am already migrating slot 16382*" {R 2 CLUSTER MIGRATE SLOTSRANGE 16382 16382 NODE $node0_id}
        R 2 CLUSTER CANCELMIGRATION LINK $linkname
        wait_for_migration_field 0 $linkname state failed

        assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16382 16382 NODE $node0_id]
        set linkname [get_link_name 2 16382]
        assert_error "*I am already migrating slot 16382*" {R 2 CLUSTER MIGRATE SLOTSRANGE 16381 16383 NODE $node0_id}
        R 2 CLUSTER CANCELMIGRATION LINK $linkname
        wait_for_migration_field 0 $linkname state failed
        set_debug_prevent_pause 0
    }

    test "CLUSTER MIGRATIONS command config enforced on update" {
        # Clear the migrations and ensure there are none
        assert_match "OK" [R 0 CONFIG SET cluster-slot-migration-log-max-len 0]
        assert_match "" [R 0 CLUSTER MIGRATIONS]
        assert_match "OK" [R 2 CONFIG SET cluster-slot-migration-log-max-len 0]
        assert_match "" [R 2 CLUSTER MIGRATIONS]
    }

    test "CLUSTER MIGRATIONS command reported fields" {
        assert_match "OK" [R 0 CONFIG SET cluster-slot-migration-log-max-len 1]
        assert_match "OK" [R 2 CONFIG SET cluster-slot-migration-log-max-len 1]
        set_debug_prevent_pause 1

        assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
        set linkname [get_link_name 2 16383]
        wait_for_migration_field 2 $linkname state waiting-to-pause

        set import_migration [get_migration_by_linkname 0 $linkname]
        set export_migration [get_migration_by_linkname 2 $linkname]

        assert_equal [dict get $import_migration operation] IMPORT
        assert_equal [dict get $export_migration operation] EXPORT

        assert_equal [dict get $import_migration slot_ranges] 16383-16383
        assert_equal [dict get $export_migration slot_ranges] 16383-16383

        assert_equal [dict get $import_migration node] [R 2 CLUSTER MYID]
        assert_equal [dict get $export_migration node] [R 0 CLUSTER MYID]

        set import_create_time [dict get $import_migration create_time]
        assert {$import_create_time ne ""}
        set export_create_time [dict get $export_migration create_time]
        assert {$export_create_time ne ""}

        set import_last_update_time [dict get $import_migration last_update_time]
        assert {$import_last_update_time ne ""}
        set export_last_update_time [dict get $import_migration last_update_time]
        assert {$export_last_update_time ne ""}

        set import_last_ack_time [dict get $import_migration last_ack_time]
        assert {$import_last_ack_time ne ""}
        set export_last_ack_time [dict get $export_migration last_ack_time]
        assert {$export_last_ack_time ne ""}
        
        wait_for_condition 100 50 {
            [dict get [get_migration_by_linkname 0 $linkname] last_ack_time] ne $import_last_ack_time
        } else {
            fail "Import operation last ack time was not updated within 5 seconds"
        }
        wait_for_condition 100 50 {
            [dict get [get_migration_by_linkname 2 $linkname] last_ack_time] ne $export_last_ack_time
        } else {
            fail "Export operation last ack time was not updated within 5 seconds"
        }

        # Wait for some time to make sure update time will change (since it is in seconds)
        after 2000
        assert_match "OK" [R 2 CLUSTER CANCELMIGRATION ALL]
        wait_for_migration_field 0 $linkname state failed

        set import_migration [get_migration_by_linkname 0 $linkname]
        set export_migration [get_migration_by_linkname 2 $linkname]

        assert {[dict get $import_migration last_update_time] ne $import_last_update_time}
        assert {[dict get $export_migration last_update_time] ne $export_last_update_time}

        assert_equal [dict get $import_migration create_time] $import_create_time
        assert_equal [dict get $export_migration create_time] $export_create_time

        set_debug_prevent_pause 0
    }

    test "CLUSTER MIGRATIONS command log removed over max len" {
        set_debug_prevent_pause 1

        # Add a new entry and the old should get popped
        assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
        set linkname2 [get_link_name 2 16383]
        wait_for_migration_field 2 $linkname2 state waiting-to-pause
        assert_match "OK" [R 2 CLUSTER CANCELMIGRATION ALL]

        set import_migration [get_migration_by_linkname 0 $linkname2]
        set export_migration [get_migration_by_linkname 2 $linkname2]
        assert {$import_migration ne ""}
        assert {$export_migration ne ""}

        # We enforce limits only in serverCron
        wait_for_condition 100 50 {
            [get_migration_by_linkname 0 $linkname] eq "" && [get_migration_by_linkname 2 $linkname] eq ""
        } else {
            fail "Old CLUSTER MIGRATIONS entry not removed after 5 seconds of max-len reached"
        }

        # Cleanup
        set_debug_prevent_pause 0
        assert_match "OK" [R 0 CONFIG SET cluster-slot-migration-log-max-len 1000]
        assert_match "OK" [R 2 CONFIG SET cluster-slot-migration-log-max-len 1000]
    }

    test "Manual and atomic slot migration are mutually exclusive" {
        set_debug_prevent_pause 1

        # Shouldn't be able to use SETSLOT when CLUSTER MIGRATE is running
        assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
        set linkname [get_link_name 2 16383]
        wait_for_migration_field 2 $linkname state waiting-to-pause
        assert_error "*A slot is currently being imported via CLUSTER IMPORT*" {R 0 CLUSTER SETSLOT 0 MIGRATING $node1_id}
        assert_error "*A slot is currently being imported via CLUSTER IMPORT*" {R 0 CLUSTER SETSLOT 0 IMPORTING $node1_id}
        assert_error "*A slot is currently being exported via CLUSTER IMPORT*" {R 2 CLUSTER SETSLOT 0 MIGRATING $node1_id}
        assert_error "*A slot is currently being exported via CLUSTER IMPORT*" {R 2 CLUSTER SETSLOT 0 IMPORTING $node1_id}
        assert_match "OK" [R 2 CLUSTER CANCELMIGRATION ALL]
        wait_for_migration_field 0 $linkname state failed

        # Shouldn't be able to use CLUSTER MIGRATE when SETSLOT was used on source
        assert_match "OK" [R 2 CLUSTER SETSLOT 0 IMPORTING $node0_id]
        assert_error "*Some slots are being manually imported*" {R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id}
        assert_match "OK" [R 2 CLUSTER SETSLOT 0 STABLE]

        # Same for the target
        assert_match "OK" [R 0 CLUSTER SETSLOT 16383 IMPORTING $node2_id]
        assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
        set linkname [get_link_name 2 16383]
        wait_for_migration_field 2 $linkname state failed
        assert {[string match {*A slot on the target node is being manually imported or migrated*} [dict get [get_migration_by_linkname 2 $linkname] message]]}
        assert_match "OK" [R 0 CLUSTER SETSLOT 16383 STABLE]

        # Cleanup
        set_debug_prevent_pause 0
    }

    test "Test CLUSTER CANCELMIGRATION ALL" {
        set_debug_prevent_pause 1
        assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16382 16382 NODE $node0_id]
        assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
        set linkname1 [get_link_name 2 16382]
        set linkname2 [get_link_name 2 16383]
        wait_for_migration_field 2 $linkname1 state waiting-to-pause
        wait_for_migration_field 2 $linkname2 state waiting-to-pause

        # Also up on the target
        assert {[dict get [get_migration_by_linkname 0 $linkname1] state] eq "waiting-for-paused"}
        assert {[dict get [get_migration_by_linkname 0 $linkname2] state] eq "waiting-for-paused"}

        assert_match "OK" [R 2 CLUSTER CANCELMIGRATION ALL]

        # Links are no longer up, migration logs say cancelled
        assert {[dict get [get_migration_by_linkname 2 $linkname1] state] eq "cancelled"}
        assert {[dict get [get_migration_by_linkname 2 $linkname2] state] eq "cancelled"}
        wait_for_migration_field 0 $linkname1 state failed
        wait_for_migration_field 0 $linkname2 state failed

        # Cleanup
        set_debug_prevent_pause 0
    }

    test "Test CLUSTER CANCELMIGRATION LINK" {
        set_debug_prevent_pause 1
        assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16382 16382 NODE $node0_id]
        assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
        set linkname1 [get_link_name 2 16382]
        set linkname2 [get_link_name 2 16383]
        wait_for_migration_field 2 $linkname1 state waiting-to-pause
        wait_for_migration_field 2 $linkname2 state waiting-to-pause

        # Also up on the target
        assert {[dict get [get_migration_by_linkname 0 $linkname1] state] eq "waiting-for-paused"}
        assert {[dict get [get_migration_by_linkname 0 $linkname2] state] eq "waiting-for-paused"}

        assert_match "OK" [R 2 CLUSTER CANCELMIGRATION LINK $linkname1]

        # One link is closed, migration log says "cancelled"
        assert {[dict get [get_migration_by_linkname 2 $linkname1] state] eq "cancelled"}
        assert {[dict get [get_migration_by_linkname 2 $linkname2] state] eq "waiting-to-pause"}
        wait_for_migration_field 0 $linkname1 state failed
        assert {[dict get [get_migration_by_linkname 0 $linkname2] state] eq "waiting-for-paused"}

        assert_match "OK" [R 2 CLUSTER CANCELMIGRATION LINK $linkname2]

        # Now both links are closed with logs in state "cancelled"
        assert {[dict get [get_migration_by_linkname 2 $linkname1] state] eq "cancelled"}
        assert {[dict get [get_migration_by_linkname 2 $linkname2] state] eq "cancelled"}
        assert {[dict get [get_migration_by_linkname 0 $linkname1] state] eq "failed"}
        wait_for_migration_field 0 $linkname2 state failed
        set_debug_prevent_pause 0
    }

    set 0_slot_tag "{06S}"
    set 5462_slot_tag "{450}"
    set 16379_slot_tag "{YY}"
    set 16380_slot_tag "{wu}"
    set 16381_slot_tag "{0TG}"
    set 16382_slot_tag "{4oi}"
    set 16383_slot_tag "{6ZJ}"

    test "Single source import - one shot" {
        assert_does_not_resync {
            # Populate data before migration
            populate 1000 "$16383_slot_tag:" 1000 -2

            # Perform one-shot import
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
            set linkname [get_link_name 2 16383]
            wait_for_migration 0 16383

            # Keys successfully migrated
            assert_match "1000" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

            # Also eventually reflected in replicas
            wait_for_countkeysinslot 3 16383 1000
            wait_for_countkeysinslot 5 16383 0

            # Migration log shows success on both ends
            assert {[dict get [get_migration_by_linkname 0 $linkname] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 2 $linkname] state] eq "success"}

            # Cleanup for next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node2_id]
            wait_for_migration 2 16383
        }
    }

    test "Single source import - two phase" {
        assert_does_not_resync {
            set_debug_prevent_pause 1

            # Load data before the snapshot
            populate 333 "$16383_slot_tag:1:" 1000 -2

            # Load data while the snapshot is ongoing
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
            set linkname [get_link_name 2 16383]
            populate 333 "$16383_slot_tag:2:" 1000 -2

            # Load data after the snapshot
            wait_for_migration_field 2 $linkname state waiting-to-pause
            populate 334 "$16383_slot_tag:3:" 1000 -2

            # Allow migration to complete and verify
            set_debug_prevent_pause 0
            wait_for_migration 0 16383
            assert_match "1000" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

            # Also eventually reflected in replicas
            wait_for_countkeysinslot 3 16383 1000
            wait_for_countkeysinslot 5 16383 0

            # Migration log shows success on both ends
            assert {[dict get [get_migration_by_linkname 0 $linkname] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 2 $linkname] state] eq "success"}

            # Cleanup for the next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node2_id]
            wait_for_migration 2 16383
        }
    }

    # Catch-all test for covering commands sent during incremental replication
    test "Single source import - Incremental Command Coverage" {
        assert_does_not_resync {
            set_debug_prevent_pause 1

            # Do the snapshot
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
            set linkname [get_link_name 2 16383]
            wait_for_migration_field 2 $linkname state waiting-to-pause

            # Multi/Exec should propagate without issue
            assert_match "OK" [R 2 MULTI]
            assert_match "QUEUED" [R 2 SET $16383_slot_tag:key1 my_value1]
            assert_match "QUEUED" [R 2 SET $16383_slot_tag:key2 my_value2]
            assert_match "QUEUED" [R 2 SET $16383_slot_tag:key3 my_value3]
            assert_match "OK OK OK" [R 2 EXEC]
            wait_for_countkeysinslot 0 16383 3
            wait_for_countkeysinslot 3 16383 3

            # Other databases should SELECT and propagate as expected
            assert_match "OK" [R 2 SELECT 15]
            assert_match "OK" [R 2 SET $16383_slot_tag:key1 my_value1]
            assert_match "OK" [R 2 SELECT 0]

            # Allow migration to complete
            set_debug_prevent_pause 0
            wait_for_migration 0 16383

            # Validate MULTI/EXEC
            assert_match "3" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
            assert_match "my_value1" [R 0 GET $16383_slot_tag:key1]
            assert_match "my_value2" [R 0 GET $16383_slot_tag:key2]
            assert_match "my_value3" [R 0 GET $16383_slot_tag:key3]
            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

            # Validate Select
            assert_match "OK" [R 0 SELECT 15]
            assert_match "1" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
            assert_match "my_value1" [R 0 GET $16383_slot_tag:key1]
            assert_match "OK" [R 0 SELECT 0]
            assert_match "OK" [R 2 SELECT 15]
            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
            assert_match "OK" [R 2 SELECT 0]

            # Also eventually reflected in replicas
            wait_for_countkeysinslot 3 16383 3
            wait_for_countkeysinslot 5 16383 0
            assert_match "OK" [R 3 SELECT 15]
            assert_match "1" [R 3 CLUSTER COUNTKEYSINSLOT 16383]
            assert_match "OK" [R 3 SELECT 0]
            assert_match "OK" [R 5 SELECT 15]
            assert_match "0" [R 5 CLUSTER COUNTKEYSINSLOT 16383]
            assert_match "OK" [R 5 SELECT 0]

            # Migration log shows success on both ends
            assert {[dict get [get_migration_by_linkname 0 $linkname] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 2 $linkname] state] eq "success"}

            # Cleanup for the next test
            assert_match "OK" [R 0 FLUSHALL SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node2_id]
            wait_for_migration 2 16383
        }
    }

    test "Simultaneous imports" {
        assert_does_not_resync {
            # Populate data before migration
            populate 100 "$5462_slot_tag:1:" 1000 -1
            populate 100 "$16383_slot_tag:1:" 1000 -2

            # Prepare imports
            set_debug_prevent_pause 1
            assert_match "OK" [R 1 CLUSTER MIGRATE SLOTSRANGE 5462 5462 NODE $node0_id]
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
            set linkname1 [get_link_name 1 5462]
            set linkname2 [get_link_name 2 16383]
            populate 100 "$5462_slot_tag:2:" 1000 -1
            populate 100 "$16383_slot_tag:2:" 1000 -2
            wait_for_migration_field 1 $linkname1 state waiting-to-pause
            wait_for_migration_field 2 $linkname2 state waiting-to-pause
            populate 100 "$5462_slot_tag:3:" 1000 -1
            populate 100 "$16383_slot_tag:3:" 1000 -2

            # Do the imports
            set_debug_prevent_pause 0
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
            assert {[dict get [get_migration_by_linkname 0 $linkname1] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 1 $linkname1] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 0 $linkname2] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 2 $linkname2] state] eq "success"}

            # Cleanup for next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATE SLOTSRANGE 5462 5462 NODE $node1_id]
            assert_match "OK" [R 0 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node2_id]
            wait_for_migration 1 5462
            wait_for_migration 2 16383
        }
    }

    test "Simultaneous exports" {
        assert_does_not_resync {
            # Populate data before migration
            populate 100 "$16382_slot_tag:1:" 1000 -2
            populate 100 "$16383_slot_tag:1:" 1000 -2

            # Prepare imports
            set_debug_prevent_pause 1
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16382 16382 NODE $node0_id]
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node1_id]
            set linkname1 [get_link_name 2 16382]
            set linkname2 [get_link_name 2 16383]
            populate 100 "$16382_slot_tag:2:" 1000 -2
            populate 100 "$16383_slot_tag:2:" 1000 -2
            wait_for_migration_field 2 $linkname1 state waiting-to-pause
            wait_for_migration_field 2 $linkname2 state waiting-to-pause
            populate 100 "$16382_slot_tag:3:" 1000 -2
            populate 100 "$16383_slot_tag:3:" 1000 -2

            # Finish the migrations
            set_debug_prevent_pause 0
            wait_for_migration 0 16382
            assert_match "300" [R 0 CLUSTER COUNTKEYSINSLOT 16382]
            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16382]

            wait_for_migration 1 16383
            assert_match "300" [R 1 CLUSTER COUNTKEYSINSLOT 16383]
            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

            # Also eventually reflected in replicas
            wait_for_countkeysinslot 3 16382 300
            wait_for_countkeysinslot 4 16383 300
            wait_for_countkeysinslot 5 16382 0
            wait_for_countkeysinslot 5 16383 0

            # Migration logs shows success on both ends
            assert {[dict get [get_migration_by_linkname 0 $linkname1] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 2 $linkname1] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 1 $linkname2] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 2 $linkname2] state] eq "success"}

            # Cleanup for next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 1 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATE SLOTSRANGE 16382 16382 NODE $node2_id]
            wait_for_migration 2 16382
            assert_match "OK" [R 1 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node2_id]
            wait_for_migration 2 16383
        }
    }

    test "Multiple slot ranges from same source" {
        assert_does_not_resync {
            # Populate data before migration
            populate 100 "$16382_slot_tag:1:" 1000 -2
            populate 100 "$16383_slot_tag:1:" 1000 -2

            # Prepare imports
            set_debug_prevent_pause 1
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16382 16382 NODE $node0_id]
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
            set linkname1 [get_link_name 2 16382]
            set linkname2 [get_link_name 2 16383]
            populate 100 "$16382_slot_tag:2:" 1000 -2
            populate 100 "$16383_slot_tag:2:" 1000 -2
            wait_for_migration_field 2 $linkname1 state waiting-to-pause
            wait_for_migration_field 2 $linkname2 state waiting-to-pause
            populate 100 "$16382_slot_tag:3:" 1000 -2
            populate 100 "$16383_slot_tag:3:" 1000 -2

            # Do the imports
            set_debug_prevent_pause 0
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
            assert {[dict get [get_migration_by_linkname 0 $linkname1] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 2 $linkname1] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 0 $linkname2] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 2 $linkname2] state] eq "success"}

            # Cleanup for next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATE SLOTSRANGE 16382 16383 NODE $node2_id]
            wait_for_migration 2 16383
        }
    }

    test "Import slot range with multiple slots" {
        assert_does_not_resync {
            # Populate data before migration
            populate 500 "$16382_slot_tag:" 1000 -2
            populate 500 "$16383_slot_tag:" 1000 -2

            # Perform one-shot import
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16382 16383 NODE $node0_id]
            set linkname [get_link_name 2 16382]
            wait_for_migration 0 16382

            # Keys successfully migrated
            assert_match "500" [R 0 CLUSTER COUNTKEYSINSLOT 16382]
            assert_match "500" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16382]
            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

            # Also eventually reflected in replicas
            wait_for_countkeysinslot 3 16382 500
            wait_for_countkeysinslot 3 16383 500
            wait_for_countkeysinslot 5 16382 0
            wait_for_countkeysinslot 5 16383 0

            # Migration logs shows success on both ends
            assert {[dict get [get_migration_by_linkname 0 $linkname] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 2 $linkname] state] eq "success"}

            # Cleanup for next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATE SLOTSRANGE 16382 16383 NODE $node2_id]
            wait_for_migration 2 16383
        }
    }

    test "Import multiple slot ranges with multiple slots" {
        assert_does_not_resync {
            # Populate data before migration
            populate 250 "$16379_slot_tag:" 1000 -2
            populate 250 "$16380_slot_tag:" 1000 -2
            populate 250 "$16382_slot_tag:" 1000 -2
            populate 250 "$16383_slot_tag:" 1000 -2

            # Perform one-shot import
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16379 16380 16382 16383 NODE $node0_id]
            set linkname [get_link_name 2 16383]
            wait_for_migration 0 16383

            # Keys successfully migrated
            foreach slot {16379 16380 16382 16383} {
                assert_match "250" [R 0 CLUSTER COUNTKEYSINSLOT $slot]
                assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT $slot]
                wait_for_countkeysinslot 3 $slot 250
                wait_for_countkeysinslot 5 $slot 0
            }

            # Migration logs shows success on both ends
            assert {[dict get [get_migration_by_linkname 0 $linkname] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 2 $linkname] state] eq "success"}

            # Cleanup for next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATE SLOTSRANGE 16379 16380 16382 16383 NODE $node2_id]
            wait_for_migration 2 16383
        }
    }

    test "Export all slots from node" {
        assert_does_not_resync {
            # Populate data before migration
            populate 1000 "$16383_slot_tag:" 1000 -2

            # Perform one-shot import
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 10924 16383 NODE $node0_id]
            set linkname [get_link_name 2 10924]
            wait_for_migration 0 10924

            # Keys successfully migrated
            assert_match "1000" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

            # Also eventually reflected in replicas
            wait_for_countkeysinslot 3 16383 1000
            wait_for_countkeysinslot 5 16383 0

            # Migration logs shows success on both ends
            assert {[dict get [get_migration_by_linkname 0 $linkname] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 2 $linkname] state] eq "success"}

            # Cleanup for next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            # Leave the slots in place for next test
        }
    }

    test "Import slots to node with no slots" {
        assert_does_not_resync {
            # Populate data before migration
            populate 1000 "$16383_slot_tag:" 1000 -0

            # Perform one-shot import
            assert_match "OK" [R 0 CLUSTER MIGRATE SLOTSRANGE 10924 16383 NODE $node2_id]
            set linkname [get_link_name 0 10924]
            wait_for_migration 2 10924

            # Keys successfully migrated
            assert_match "1000" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
            assert_match "0" [R 0 CLUSTER COUNTKEYSINSLOT 16383]

            # Also eventually reflected in replicas
            wait_for_countkeysinslot 5 16383 1000
            wait_for_countkeysinslot 3 16383 0

            # Migration logs shows success on both ends
            assert {[dict get [get_migration_by_linkname 0 $linkname] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 2 $linkname] state] eq "success"}

            # Cleanup for next test
            assert_match "OK" [R 2 FLUSHDB SYNC]
        }
    }

    test "Partial data removed on cancel" {
        assert_does_not_resync {
            # Load data before the snapshot
            populate 333 "$16383_slot_tag:1:" 1000 -2

            # Load data while the snapshot is ongoing
            set_debug_prevent_pause 1
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
            set linkname [get_link_name 2 16383]
            populate 333 "$16383_slot_tag:2:" 1000 -2

            # Load data after the snapshot
            wait_for_migration_field 2 $linkname state waiting-to-pause
            populate 334 "$16383_slot_tag:3:" 1000 -2

            # Cancel and the data should be dropped
            assert_match "OK" [R 2 CLUSTER CANCELMIGRATION LINK $linkname]
            assert {[dict get [get_migration_by_linkname 2 $linkname] state] eq "cancelled"}
            wait_for_migration_field 0 $linkname state failed
            assert_match "0" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
            assert_match "1000" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

            # Also eventually reflected in replicas
            wait_for_countkeysinslot 3 16383 0
            wait_for_countkeysinslot 5 16383 1000


            # Cleanup for the next test
            assert_match "OK" [R 2 FLUSHDB SYNC]
            set_debug_prevent_pause 0
        }
    }

    test "OOM on target aborts migration" {
        assert_does_not_resync {
            # Load some data before the snapshot
            populate 500 "$16383_slot_tag:1:" 1000 -2
            set_debug_prevent_pause 1
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
            set linkname [get_link_name 2 16383]
            wait_for_migration_field 2 $linkname state waiting-to-pause

            # Set maxmemory to simulate OOM
            assert_match "OK" [R 0 CONFIG SET maxmemory 1]

            # Loading more data should cause a failure
            populate 500 "$16383_slot_tag:3:" 1000 -2
            wait_for_migration_field 2 $linkname state failed
            wait_for_migration_field 0 $linkname state failed

            # Verify the keys are eventually dropped on target
            assert_match "1000" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
            wait_for_countkeysinslot 0 16383 0

            # Also eventually reflected in replicas
            wait_for_countkeysinslot 5 16383 1000
            wait_for_countkeysinslot 3 16383 0

            # Migration logs shows failure on both ends
            assert {[string match {*OOM*} [dict get [get_migration_by_linkname 0 $linkname] message]]}
            assert {[string match {*Connection lost to target*} [dict get [get_migration_by_linkname 2 $linkname] message]]}

            # Cleanup for the next test
            assert_match "OK" [R 0 CONFIG SET maxmemory 0]
            assert_match "OK" [R 2 FLUSHDB SYNC]
            set_debug_prevent_pause 0
        }
    }

    test "Partial data in replica removed on failover" {
        # Load some data before the snapshot
        populate 500 "$16383_slot_tag:1:" 1000 -2

        # Prepare and wait for ready
        set_debug_prevent_pause 1
        assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
        set linkname [get_link_name 2 16383]
        wait_for_migration_field 2 $linkname state waiting-to-pause

        # Make sure the replica has it
        wait_for_countkeysinslot 3 16383 500

        # Trigger failover
        assert_match "OK" [R 3 CLUSTER FAILOVER]

        # Links should be dropped on both ends
        wait_for_migration_field 2 $linkname state failed
        wait_for_migration_field 0 $linkname state failed

        # Keys should be dropped in target shard
        assert_match "0" [R 3 CLUSTER COUNTKEYSINSLOT 16383]
        assert_match "0" [R 0 CLUSTER COUNTKEYSINSLOT 16383]

        # Keys on existing shard are untouched
        assert_match "500" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
        assert_match "500" [R 5 CLUSTER COUNTKEYSINSLOT 16383]

        # Expect error messages
        assert {[string match {*I was demoted to a replica*} [dict get [get_migration_by_linkname 0 $linkname] message]]}
        assert {[string match {*Connection lost to target*} [dict get [get_migration_by_linkname 2 $linkname] message]]}

        # Cleanup for the next test
        assert_match "OK" [R 2 FLUSHDB SYNC]
        set_debug_prevent_pause 0
    }

    test "Slot export failed on failover" {
        # Load some data before the snapshot
        populate 500 "$0_slot_tag:1:" 1000 -3

        # Prepare and wait for ready
        set_debug_prevent_pause 1
        assert_match "OK" [R 3 CLUSTER MIGRATE SLOTSRANGE 0 0 NODE $node2_id]
        set linkname [get_link_name 3 0]
        wait_for_migration_field 3 $linkname state waiting-to-pause

        # Trigger failover
        assert_match "OK" [R 0 CLUSTER FAILOVER]

        # Links should be dropped on both ends
        wait_for_migration_field 3 $linkname state failed
        wait_for_migration_field 2 $linkname state failed

        # Keys should be dropped in target shard
        assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 0]
        wait_for_countkeysinslot 5 0 0

        # Keys on existing shard are untouched
        assert_match "500" [R 3 CLUSTER COUNTKEYSINSLOT 0]
        assert_match "500" [R 0 CLUSTER COUNTKEYSINSLOT 0]

        # Expect error messages. There are two error messages we could see, depending on the order of events.
        assert {
            [string match {*Slots are no longer owned by myself*} [dict get [get_migration_by_linkname 3 $linkname] message]] ||
            [string match {*Connection lost to target*} [dict get [get_migration_by_linkname 3 $linkname] message]]
        }
        assert {
            [string match {*Slots are no longer owned by source node*} [dict get [get_migration_by_linkname 2 $linkname] message]] ||
            [string match {*Connection lost to source*} [dict get [get_migration_by_linkname 2 $linkname] message]]
        }

        # Cleanup for the next test
        assert_match "OK" [R 0 FLUSHDB SYNC]
        set_debug_prevent_pause 0
    }

    test "Slots split across shards during import" {
        assert_does_not_resync {
            # Load some data before the snapshot
            populate 500 "$0_slot_tag:1:" 1000 -0

            # Prepare and wait for ready
            set_debug_prevent_pause 1
            assert_match "OK" [R 0 CLUSTER MIGRATE SLOTSRANGE 0 1 NODE $node2_id]
            set linkname [get_link_name 0 0]
            wait_for_migration_field 0 $linkname state waiting-to-pause

            # Force slot takeover
            assert_match "*BUMPED*" [R 1 CLUSTER BUMPEPOCH]
            assert_match "OK" [R 1 CLUSTER SETSLOT 0 NODE $node1_id]

            # Second link should get dropped on either end
            wait_for_migration_field 0 $linkname state failed
            wait_for_migration_field 2 $linkname state failed

            # Keys should be dropped on cancelled link
            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 0]
            assert_match "0" [R 5 CLUSTER COUNTKEYSINSLOT 0]

            # Migration logs for linkname shows failure on both ends
            # assert {[string match {*Slots are no longer owned by myself*} [dict get [get_migration_by_linkname 0 $linkname] message]]}
            # assert {[string match {*Slots are no longer owned by source node*} [dict get [get_migration_by_linkname 2 $linkname] message]]}

            # Cleanup for the next test
            set_debug_prevent_pause 0
            assert_match "*BUMPED*" [R 0 CLUSTER BUMPEPOCH]
            assert_match "OK" [R 0 CLUSTER SETSLOT 0 NODE $node0_id]
            wait_for_migration 0 0
        }
    }

    test "Export unpauses itself even if slot failover doesn't occur" {
        assert_does_not_resync {
            # Lower manual failover timeout for this test
            set mf_timeout_old [lindex [R 0 CONFIG GET cluster-manual-failover-timeout] 1]
            R 0 CONFIG SET cluster-manual-failover-timeout 100

            # Use debug command to prevent failover
            set_debug_prevent_failover 1
            assert_match "OK" [R 0 CLUSTER MIGRATE SLOTSRANGE 0 0 NODE $node2_id]
            set linkname [get_link_name 0 0]

            # Our link should get dropped after pause timeout expires
            wait_for_migration_field 0 $linkname state failed
            wait_for_migration_field 2 $linkname state failed

            # Migration logs for link shows failure
            assert {[string match {*Unpaused before migration completed*} [dict get [get_migration_by_linkname 0 $linkname] message]]}
            assert {[string match {*Connection lost to source*} [dict get [get_migration_by_linkname 2 $linkname] message]]}

            # Validate no longer paused
            populate 1 "$0_slot_tag:" 1000 -0

            # Reset manual failover timeout
            R 0 CONFIG SET cluster-manual-failover-timeout $mf_timeout_old

            # Cleanup for the next test
            set_debug_prevent_failover 0
            assert_match "OK" [R 0 FLUSHDB SYNC]
        }
    }

    test "CLUSTER SYNCSLOTS invalid state machine traversal" {
        assert_does_not_resync {
            # Invalid state machine traversal on export node will send SYNCSLOTS FAIL
            assert_causes_conn_drop 0 CLUSTER SYNCSLOTS PAUSE
            assert_causes_conn_drop 0 CLUSTER SYNCSLOTS REQUEST-FAILOVER

            # Invalid state machine traversal on import node just drops connection
            assert_causes_conn_drop 0 CLUSTER SYNCSLOTS SNAPSHOT-EOF
            assert_causes_conn_drop 0 CLUSTER SYNCSLOTS PAUSED
            assert_causes_conn_drop 0 CLUSTER SYNCSLOTS FAILOVER-GRANTED
            assert_causes_conn_drop 0 CLUSTER SYNCSLOTS ACK
        }
    }

    test "CLUSTER SYNCSLOTS ESTABLISH command interface" {
        assert_does_not_resync {
            # No arguments
            assert_error "*syntax error*" {R 0 CLUSTER SYNCSLOTS ESTABLISH}

            # No target
            assert_error "*syntax error*" {R 0 CLUSTER SYNCSLOTS ESTABLISH LINKNAME $fake_linkname SLOTSRANGE 0 0}

            # No linkname
            assert_error "*syntax error*" {R 0  CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id SLOTSRANGE 0 0}

            # No slotsrange
            assert_error "*syntax error*" {R 0 CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id LINKNAME $fake_linkname}

            # No end slot
            assert_error "*No end slot for final slot range*" {R 0 CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id LINKNAME $fake_linkname SLOTSRANGE 0}

            # Unknown target
            assert_error "*Target node does not know the source node*" {R 0 CLUSTER SYNCSLOTS ESTABLISH SOURCE $fake_linkname LINKNAME $fake_linkname SLOTSRANGE 0 0}

            # Unowned slotsrange
            assert_error "*Target node does not agree about current slot ownership*" {R 0 CLUSTER SYNCSLOTS ESTABLISH SOURCE $node1_id LINKNAME $fake_linkname SLOTSRANGE 16383 16383}

            # Not primary
            assert_error "*Target node is not a primary*" {R 3 CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id LINKNAME $fake_linkname SLOTSRANGE 0 0}

            # Invalid target name
            assert_error "*syntax error*" {R 0 CLUSTER SYNCSLOTS ESTABLISH SOURCE invalid LINKNAME $fake_linkname SLOTSRANGE 0 0}

            # Invalid link name
            assert_error "*syntax error*" {R 0 CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id LINKNAME invalid SLOTSRANGE 0 0}
        }
    }

    test "FLUSHDB on target during import" {
        assert_does_not_resync {
            # Load data before the snapshot
            populate 1000 "$16383_slot_tag:1:" 1000 -2

            # Do the import
            set_debug_prevent_pause 1
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
            set linkname [get_link_name 2 16383]

            # Keys should be on both source and destination
            assert_match "1000" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
            wait_for_countkeysinslot 0 16383 1000

            # Now run FLUSHDB SYNC on the target
            assert_match "OK" [R 0 FLUSHDB SYNC]

            # Target should fail the migration
            wait_for_migration_field 2 $linkname state failed
            wait_for_migration_field 0 $linkname state failed
            assert {[string match {*Data was flushed*} [dict get [get_migration_by_linkname 0 $linkname] message]]}
            assert {[string match {*Connection lost to target*} [dict get [get_migration_by_linkname 2 $linkname] message]]}
            wait_for_countkeysinslot 0 16383 0
            wait_for_countkeysinslot 3 16383 0
            
            # Same for FLUSHDB ASYNC
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
            assert_match "1000" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
            wait_for_countkeysinslot 0 16383 1000
            assert_match "OK" [R 0 FLUSHDB ASYNC]
            wait_for_migration_field 2 $linkname state failed
            wait_for_migration_field 0 $linkname state failed
            assert {[string match {*Data was flushed*} [dict get [get_migration_by_linkname 0 $linkname] message]]}
            assert {[string match {*Connection lost to target*} [dict get [get_migration_by_linkname 2 $linkname] message]]}
            wait_for_countkeysinslot 0 16383 0
            wait_for_countkeysinslot 3 16383 0

            # Cleanup
            assert_match "OK" [R 2 FLUSHDB SYNC]
            set_debug_prevent_pause 0
        }
    }

    test "FLUSHDB on source during export" {
        assert_does_not_resync {
            # Load data before the snapshot
            populate 1000 "$16383_slot_tag:1:" 1000 -2

            # Do the import
            set_debug_prevent_pause 1
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
            set linkname [get_link_name 2 16383]

            # Keys should be on both source and destination
            assert_match "1000" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
            wait_for_countkeysinslot 0 16383 1000

            # FLUSHDB on the source should fail the import
            assert_match "OK" [R 2 FLUSHDB SYNC]
            wait_for_migration_field 2 $linkname state failed
            wait_for_migration_field 0 $linkname state failed
            assert {[string match {*Data was flushed*} [dict get [get_migration_by_linkname 2 $linkname] message]]}
            assert {[string match {*Connection lost to source*} [dict get [get_migration_by_linkname 0 $linkname] message]]}

            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
            wait_for_countkeysinslot 0 16383 0
            wait_for_countkeysinslot 3 16383 0

            # Cleanup
            set_debug_prevent_pause 0
        }
    }

    test "Import cancelled when source hangs" {
        assert_does_not_resync {
            R 2 CONFIG SET repl-timeout 1

            # Load data before the snapshot
            populate 333 "$0_slot_tag:1:" 1000 -0

            # Load data while the snapshot is ongoing
            set_debug_prevent_pause 1
            assert_match "OK" [R 0 CLUSTER MIGRATE SLOTSRANGE 0 0 NODE $node2_id]
            set linkname [get_link_name 0 0]
            populate 333 "$0_slot_tag:2:" 1000 -0

            # Load data after the snapshot
            wait_for_migration_field 0 $linkname state waiting-to-pause
            populate 334 "$0_slot_tag:3:" 1000 -0

            # Now pause source
            set node0_pid  [srv 0 pid]
            pause_process $node0_pid

            # The import should eventually fail due to no ACKs
            wait_for_migration_field 2 $linkname state failed
            assert {[string match {*Timed out after too long with no interaction*} [dict get [get_migration_by_linkname 2 $linkname] message]]}

            # After resuming, it should be reflected on source
            resume_process $node0_pid
            wait_for_migration_field 0 $linkname state failed
            assert {[string match {*Connection lost to target*} [dict get [get_migration_by_linkname 0 $linkname] message]]}

            # Cleanup for the next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            R 2 CONFIG SET repl-timeout 60
            set_debug_prevent_pause 0
        }
    }

    test "Export cancelled when target hangs" {
        assert_does_not_resync {
            R 0 CONFIG SET repl-timeout 1

            # Load data before the snapshot
            populate 333 "$0_slot_tag:1:" 1000 -0

            # Load data while the snapshot is ongoing
            set_debug_prevent_pause 1
            assert_match "OK" [R 0 CLUSTER MIGRATE SLOTSRANGE 0 0 NODE $node2_id]
            set linkname [get_link_name 0 0]
            populate 333 "$0_slot_tag:2:" 1000 -0

            # Load data after the snapshot
            wait_for_migration_field 0 $linkname state waiting-to-pause
            populate 334 "$0_slot_tag:3:" 1000 -0

            # Now pause target
            set node2_pid [srv -2 pid]
            pause_process $node2_pid

            # The export should eventually fail due to no ACKs
            wait_for_migration_field 0 $linkname state failed
            assert {[string match {*Timed out after too long with no interaction*} [dict get [get_migration_by_linkname 0 $linkname] message]]}

            # After resuming, it should be reflected on target
            resume_process $node2_pid
            wait_for_migration_field 2 $linkname state failed
            assert {[string match {*Connection lost to source*} [dict get [get_migration_by_linkname 2 $linkname] message]]}

            # Cleanup for the next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            R 0 CONFIG SET repl-timeout 60
            set_debug_prevent_pause 0
        }
    }

    test "Import with AUTH on" {
        assert_does_not_resync {
            R 2 CONFIG SET requirepass "mypassword"
            R 0 CONFIG SET primaryauth "mypassword"

            # Populate data before migration
            populate 1000 "$16383_slot_tag:" 1000 -2

            # Perform one-shot import
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
            set linkname [get_link_name 2 16383]
            wait_for_migration 0 16383

            # Keys successfully migrated
            assert_match "1000" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

            # Also eventually reflected in replicas
            wait_for_countkeysinslot 3 16383 1000
            wait_for_countkeysinslot 5 16383 0

            # Migration log shows success on both ends
            assert {[dict get [get_migration_by_linkname 0 $linkname] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 2 $linkname] state] eq "success"}

            # Cleanup for next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node2_id]
            wait_for_migration 2 16383
            R 2 CONFIG SET requirepass ""
            R 0 CONFIG SET primaryauth ""
        }
    }

    test "Import AUTH with WRONGPASS" {
        assert_does_not_resync {
            R 0 CONFIG SET requirepass "mypassword"
            R 2 CONFIG SET primaryauth "mypassword-different"

            # Perform one-shot import
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
            set linkname [get_link_name 2 16383]

            # Should be denied
            wait_for_migration_field 2 $linkname state failed
            assert {[string match {*Failed to AUTH to target node: -WRONGPASS*} [dict get [get_migration_by_linkname 2 $linkname] message]]}

            # Cleanup for next test
            R 0 CONFIG SET requirepass ""
            R 2 CONFIG SET primaryauth ""
        }
    }

    test "Connection drop during import causes failure" {
        assert_does_not_resync {
            # Start an import
            set_debug_prevent_pause 1
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
            set linkname [get_link_name 2 16383]
            wait_for_migration_field 2 $linkname state waiting-to-pause
            set import_client_id [get_client_id_by_last_cmd [srv -0 client] "cluster|syncslots"]

            # Use CLIENT KILL to drop the connection
            R 0 CLIENT KILL ID $import_client_id

            # Migration should be failed
            wait_for_migration_field 2 $linkname state failed
            wait_for_migration_field 0 $linkname state failed
            assert {[string match {*Connection lost to source*} [dict get [get_migration_by_linkname 0 $linkname] message]]}
            assert {[string match {*Connection lost to target*} [dict get [get_migration_by_linkname 2 $linkname] message]]}

            # Cleanup for next test
            set_debug_prevent_pause 0
        }
    }

    test "Export client buffer enforcement" {
        assert_does_not_resync {
            set_debug_prevent_pause 1
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16383 16383 NODE $node0_id]
            set linkname [get_link_name 2 16383]
            wait_for_migration_field 2 $linkname state waiting-to-pause
            set old_cob [lindex [R 2 config get client-output-buffer-limit] 1]
            R 2 config set client-output-buffer-limit "replica 10k 0 0"

            # Pause the target
            set node0_pid [srv -0 pid]
            pause_process $node0_pid

            set migration [get_migration_by_linkname 2 $linkname]
            # Accumulate a large backlog on the source, it should eventually kill the client
            for {set i 0} {$i < 100} {incr i} {
                populate 1000 "$16383_slot_tag:" 1000 -2
                set migration [get_migration_by_linkname 2 $linkname]
                if {[dict get $migration state] eq "failed"} {
                    break
                }
            }
            if {[dict get $migration state] ne "failed"} {
                fail "Export was not failed after writing 100 MiB of changes, current state: $migration"
            }

            resume_process $node0_pid

            # Migration should be failed
            wait_for_migration_field 2 $linkname state failed
            wait_for_migration_field 0 $linkname state failed
            assert {[string match {*Connection lost to source*} [dict get [get_migration_by_linkname 0 $linkname] message]]}
            assert {[string match {*Connection lost to target*} [dict get [get_migration_by_linkname 2 $linkname] message]]}

            # Cleanup for the next test
            assert_match "OK" [R 2 FLUSHDB SYNC]
            R 2 config set client-output-buffer-limit "$old_cob"
            set_debug_prevent_pause 0
        }
    }

    test "Slot importing with some non-importing data" {
        assert_does_not_resync {
            # Load data before the snapshot
            set tags [list $16381_slot_tag $16382_slot_tag $16383_slot_tag]
            foreach tag $tags {
                populate 333 "$tag:1:" 1000 -2
            }

            # Load data while the snapshot is ongoing
            set_debug_prevent_pause 1
            assert_match "OK" [R 2 CLUSTER MIGRATE SLOTSRANGE 16381 16381 16383 16383 NODE $node0_id]
            set linkname [get_link_name 2 16381]
            foreach tag $tags {
                populate 333 "$tag:2:" 1000 -2
            }

            # Load data after the snapshot
            wait_for_migration_field 2 $linkname state waiting-to-pause
            foreach tag $tags {
                populate 334 "$tag:3:" 1000 -2
            }

            # We should see only those keys sent
            wait_for_countkeysinslot 0 16381 1000
            assert_match "0" [R 0 CLUSTER COUNTKEYSINSLOT 16382]
            wait_for_countkeysinslot 0 16383 1000

            wait_for_countkeysinslot 3 16381 1000
            assert_match "0" [R 3 CLUSTER COUNTKEYSINSLOT 16382]
            wait_for_countkeysinslot 3 16383 1000

            # Commit and verify
            set_debug_prevent_pause 0
            wait_for_migration 0 16381
            wait_for_migration 0 16383

            assert_match "1000" [R 0 CLUSTER COUNTKEYSINSLOT 16381]
            assert_match "0" [R 0 CLUSTER COUNTKEYSINSLOT 16382]
            assert_match "1000" [R 0 CLUSTER COUNTKEYSINSLOT 16383]

            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16381]
            assert_match "1000" [R 2 CLUSTER COUNTKEYSINSLOT 16382]
            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

            # Also reflected in replicas
            assert_match "1000" [R 3 CLUSTER COUNTKEYSINSLOT 16381]
            assert_match "0" [R 3 CLUSTER COUNTKEYSINSLOT 16382]
            assert_match "1000" [R 3 CLUSTER COUNTKEYSINSLOT 16383]

            assert_match "0" [R 5 CLUSTER COUNTKEYSINSLOT 16381]
            assert_match "1000" [R 5 CLUSTER COUNTKEYSINSLOT 16382]
            assert_match "0" [R 5 CLUSTER COUNTKEYSINSLOT 16383]

            # Migration log shows success on both ends
            assert {[dict get [get_migration_by_linkname 0 $linkname] state] eq "success"}
            assert {[dict get [get_migration_by_linkname 2 $linkname] state] eq "success"}

            # Cleanup for the next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 2 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATE SLOTSRANGE 16381 16381 16383 16383 NODE $node2_id]
            wait_for_migration 2 16381
        }
    }
}

start_cluster 3 0 {tags {external:skip cluster}} {

    set node0_id [R 0 CLUSTER MYID]
    set node1_id [R 1 CLUSTER MYID]
    set node2_id [R 2 CLUSTER MYID]

    test "Migration cannot connect to target" {
        # Shutdown to prevent connection success
        catch {R 2 shutdown nosave}
        assert_match "OK" [R 0 CLUSTER MIGRATE SLOTSRANGE 0 0 NODE $node2_id]
        set linkname [get_link_name 0 0]

        # Connecting will fail
        wait_for_migration_field 0 $linkname state failed
        assert {[string match {*Unable to connect to target node: Connection refused*} [dict get [get_migration_by_linkname 0 $linkname] message]]}
    }

}

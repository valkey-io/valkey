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

proc get_job_name {node_idx slot} {
    set migrations [R $node_idx CLUSTER GETSLOTMIGRATIONS]
    foreach migration $migrations {
        set slot_ranges [dict get $migration slot_ranges]
        if {[slot_ranges_contains_slot $slot_ranges $slot]} {
            return [dict get $migration name]
        }
    }
    return ""
}

proc get_migration_by_name {node_idx name} {
    set migrations [R $node_idx CLUSTER GETSLOTMIGRATIONS]
    foreach migration $migrations {
        if {[dict get $migration name] eq $name} {
            return $migration
        }
    }
    return ""
}

proc wait_for_migration_field {node_idx jobname field value} {
    wait_for_condition 100 100 {
        [get_migration_by_name $node_idx $jobname] ne "" && [dict get [get_migration_by_name $node_idx $jobname] $field] eq $value
    } else {
        set curr_state [get_migration_by_name $node_idx $jobname]
        fail "Migration $jobname on node $node_idx did not have $field == $value (currently $curr_state) within 10000 ms"
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

# Ensure that a given hash slot is served by the specified node.
# If the slot is currently owned by a different node, move it to the target
# and wait for the migration to complete.
proc ensure_slot_on_node {node_idx slot} {
    if {[is_slot_migrated $node_idx $slot]} {return}
    set target_id [R $node_idx CLUSTER MYID]
    for {set i 0} {$i < 3} {incr i} {
        if {[is_slot_migrated $i $slot]} {
            assert_match "OK" [R $i CLUSTER MIGRATESLOTS SLOTSRANGE $slot $slot NODE $target_id]
            wait_for_migration $node_idx $slot
            return
        }
    }
    fail "Unable to find owner for slot $slot"
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

# Helper to perform eviction related tests. It sets up the configs on idx so
# that the node will begin evicting with the given policy once full. It will
# require ~1 MiB of data to be added to fill up the repl-backlog first.
proc setup_eviction_test {idx policy body} {
    R $idx CONFIG SET maxmemory-policy $policy
    set old_cob_limit [lindex [R $idx config get client-output-buffer-limit] 1]
    R $idx CONFIG SET client-output-buffer-limit "replica 10k 0 0"
    R $idx CONFIG SET lazyfree-lazy-eviction no
    R $idx CONFIG SET maxmemory-eviction-tenacity 100
    set old_repl_backlog [lindex [R $idx config get repl-backlog-size] 1]
    R $idx CONFIG SET repl-backlog-size 1mb

    set used [s -$idx used_memory]
    # limit = current used + repl backlog max size + 100kiB for keys
    set limit [expr {$used+1024*1024+100*1024}]
    R $idx config set maxmemory $limit

    uplevel 1 $body

    R $idx CONFIG SET client-output-buffer-limit $old_cob_limit
    R $idx CONFIG SET maxmemory-policy volatile-lru
    R $idx CONFIG SET maxmemory 0
    R $idx CONFIG SET lazyfree-lazy-eviction yes
    R $idx CONFIG SET maxmemory-eviction-tenacity 10
    R $idx CONFIG SET repl-backlog-size $old_repl_backlog

}

proc assert_causes_conn_drop {node_idx body} {
    upvar 1 client vc
    set vc [valkey_client_by_addr [srv -$node_idx host] [srv -$node_idx port]]
    catch {
        uplevel 1 $body
    } result
    $vc close
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
start_cluster 3 3 {tags {logreqres:skip external:skip cluster} overrides {cluster-allow-replica-migration no cluster-node-timeout 15000 cluster-databases 16}} {

    set node0_id [R 0 CLUSTER MYID]
    set node1_id [R 1 CLUSTER MYID]
    set node2_id [R 2 CLUSTER MYID]
    set fake_jobname "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

    test "General command interface" {
        assert_error "*wrong number of arguments*" {R 0 CLUSTER MIGRATESLOTS}
        assert_error "*syntax error*" {R 0 CLUSTER MIGRATESLOTS INVALID 0 1}
        assert_error "*wrong number of arguments*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE}
        assert_error "*No end slot for final slot range*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0}
        assert_error "*Invalid or out of range slot*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 16385 16388}
        assert_error "*Invalid or out of range slot*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 16380 16388}
        assert_error "*No slot ranges specified*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE a 0}
        assert_error "*Invalid or out of range slot*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 a}
        assert_error "*Start slot number 1 is greater than end slot number 0*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 1 0}
        assert_error "*Requested slots span multiple shards*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 16383}
        assert_error "*Slot range 3-6 overlaps with previous range 0-5*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 5 3 6}
        assert_error "*Slot range 0-5 overlaps with previous range 3-6*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 3 6 0 5}
        assert_error "*syntax error*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0}
        assert_error "*syntax error*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 NODE}
        assert_error "*Invalid node name*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 NODE blah}
        assert_error "*Unknown node name*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 NODE $fake_jobname}
        assert_error "*Slot ranges in migrations overlap*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 1 1 NODE $node1_id SLOTSRANGE 0 2 NODE $node2_id}
        assert_error "*Slot ranges in migrations overlap*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 2 2 NODE $node1_id SLOTSRANGE 2 2 NODE $node2_id}

        set source_node_id [R 0 CLUSTER MYID]
        set target_node_id [R 1 CLUSTER MYID]
        R 0 CLUSTER SETSLOT 0 MIGRATING $target_node_id
        R 1 CLUSTER SETSLOT 0 IMPORTING $source_node_id
        assert_error "*Slots are being manually migrated*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383}
        assert_error "*Slots are being manually imported*" {R 1 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383}
        R 0 CLUSTER SETSLOT 0 STABLE
        R 1 CLUSTER SETSLOT 0 STABLE

        R 0 CLUSTER DELSLOTS 0
        assert_error "*Slot 0 has no node served*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0}
        R 0 CLUSTER ADDSLOTS 0

        assert_error "*Slot migration can only be used on primary nodes*" {R 3 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0}
        assert_error "*Slots are not served by this node*" {R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 NODE $node0_id}
        assert_error "*Target node can not be this node*" {R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 NODE $node0_id}

        assert_error "*wrong number of arguments*" {R 0 CLUSTER CANCELSLOTMIGRATIONS ARG}
        assert_error "*No migrations ongoing*" {R 0 CLUSTER CANCELSLOTMIGRATIONS}
    }

    test "CLUSTER MIGRATESLOTS already migrating" {
        set_debug_prevent_pause 1
        assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
        set jobname [get_job_name 2 16383]
        wait_for_migration_field 2 $jobname state waiting-to-pause
        assert_error "*I am already migrating slot 16383*" {R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id}
        R 2 CLUSTER CANCELSLOTMIGRATIONS
        wait_for_migration_field 0 $jobname state failed

        assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16381 16383 NODE $node0_id]
        set jobname [get_job_name 2 16381]
        wait_for_migration_field 2 $jobname state waiting-to-pause
        assert_error "*I am already migrating slot 16382*" {R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16382 16382 NODE $node0_id}
        R 2 CLUSTER CANCELSLOTMIGRATIONS
        wait_for_migration_field 0 $jobname state failed

        assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16382 16382 NODE $node0_id]
        set jobname [get_job_name 2 16382]
        wait_for_migration_field 2 $jobname state waiting-to-pause
        assert_error "*I am already migrating slot 16382*" {R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16381 16383 NODE $node0_id}
        R 2 CLUSTER CANCELSLOTMIGRATIONS
        wait_for_migration_field 0 $jobname state failed
        set_debug_prevent_pause 0
    }

    test "CLUSTER GETSLOTMIGRATIONS command config enforced" {
        # Clear the migrations and ensure there are none
        assert_match "OK" [R 0 CONFIG SET cluster-slot-migration-log-max-len 0]
        wait_for_condition 100 100 {
            [R 0 CLUSTER GETSLOTMIGRATIONS] eq ""
        } else {
            fail "GETSLOTMIGRATIONS was not cleared within 10 seconds"
        }
        assert_match "OK" [R 2 CONFIG SET cluster-slot-migration-log-max-len 0]
        wait_for_condition 100 100 {
            [R 2 CLUSTER GETSLOTMIGRATIONS] eq ""
        } else {
            fail "GETSLOTMIGRATIONS was not cleared within 10 seconds"
        }
    }

    test "CLUSTER GETSLOTMIGRATIONS command reported fields" {
        assert_match "OK" [R 0 CONFIG SET cluster-slot-migration-log-max-len 1]
        assert_match "OK" [R 2 CONFIG SET cluster-slot-migration-log-max-len 1]
        set_debug_prevent_pause 1

        assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
        set jobname [get_job_name 2 16383]
        wait_for_migration_field 2 $jobname state waiting-to-pause

        set import_migration [get_migration_by_name 0 $jobname]
        set export_migration [get_migration_by_name 2 $jobname]

        assert_equal [dict get $import_migration operation] IMPORT
        assert_equal [dict get $export_migration operation] EXPORT

        assert_equal [dict get $import_migration slot_ranges] 16383-16383
        assert_equal [dict get $export_migration slot_ranges] 16383-16383

        assert_equal [dict get $import_migration source_node] [R 2 CLUSTER MYID]
        assert_equal [dict get $import_migration target_node] [R 0 CLUSTER MYID]
        assert_equal [dict get $export_migration source_node] [R 2 CLUSTER MYID]
        assert_equal [dict get $export_migration target_node] [R 0 CLUSTER MYID]

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
            [dict get [get_migration_by_name 0 $jobname] last_ack_time] ne $import_last_ack_time
        } else {
            fail "Import operation last ack time was not updated within 5 seconds"
        }
        wait_for_condition 100 50 {
            [dict get [get_migration_by_name 2 $jobname] last_ack_time] ne $export_last_ack_time
        } else {
            fail "Export operation last ack time was not updated within 5 seconds"
        }

        # Wait for some time to make sure update time will change (since it is in seconds)
        after 2000
        assert_match "OK" [R 2 CLUSTER CANCELSLOTMIGRATIONS]
        wait_for_migration_field 0 $jobname state failed

        set import_migration [get_migration_by_name 0 $jobname]
        set export_migration [get_migration_by_name 2 $jobname]

        assert {[dict get $import_migration last_update_time] ne $import_last_update_time}
        assert {[dict get $export_migration last_update_time] ne $export_last_update_time}

        assert_equal [dict get $import_migration create_time] $import_create_time
        assert_equal [dict get $export_migration create_time] $export_create_time

        set_debug_prevent_pause 0
    }

    test "CLUSTER GETSLOTMIGRATIONS command log removed over max len" {
        set_debug_prevent_pause 1

        # Add a new entry and the old should get popped
        assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
        set jobname2 [get_job_name 2 16383]
        wait_for_migration_field 2 $jobname2 state waiting-to-pause
        assert_match "OK" [R 2 CLUSTER CANCELSLOTMIGRATIONS]

        set import_migration [get_migration_by_name 0 $jobname2]
        set export_migration [get_migration_by_name 2 $jobname2]
        assert {$import_migration ne ""}
        assert {$export_migration ne ""}

        # We enforce limits only in serverCron
        wait_for_condition 100 50 {
            [get_migration_by_name 0 $jobname] eq "" && [get_migration_by_name 2 $jobname] eq ""
        } else {
            fail "Old CLUSTER GETSLOTMIGRATIONS entry not removed after 5 seconds of max-len reached"
    }

        # Cleanup
        set_debug_prevent_pause 0
        assert_match "OK" [R 0 CONFIG SET cluster-slot-migration-log-max-len 1000]
        assert_match "OK" [R 2 CONFIG SET cluster-slot-migration-log-max-len 1000]
    }

    test "Manual and atomic slot migration are mutually exclusive" {
        set_debug_prevent_pause 1

        # Shouldn't be able to use SETSLOT when CLUSTER MIGRATESLOTS is running
        assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
        set jobname [get_job_name 2 16383]
        wait_for_migration_field 2 $jobname state waiting-to-pause
        assert_error "*Slot import in progress*" {R 0 CLUSTER SETSLOT 0 MIGRATING $node1_id}
        assert_error "*Slot import in progress*" {R 0 CLUSTER SETSLOT 0 IMPORTING $node1_id}
        assert_error "*Slot export in progress*" {R 2 CLUSTER SETSLOT 0 MIGRATING $node1_id}
        assert_error "*Slot export in progress*" {R 2 CLUSTER SETSLOT 0 IMPORTING $node1_id}
        assert_match "OK" [R 2 CLUSTER CANCELSLOTMIGRATIONS]
        wait_for_migration_field 0 $jobname state failed

        # Shouldn't be able to use CLUSTER MIGRATESLOTS when SETSLOT was used on source
        assert_match "OK" [R 2 CLUSTER SETSLOT 0 IMPORTING $node0_id]
        assert_error "*Slots are being manually imported*" {R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id}
        assert_match "OK" [R 2 CLUSTER SETSLOT 0 STABLE]

        # Same for the target
        assert_match "OK" [R 0 CLUSTER SETSLOT 16383 IMPORTING $node2_id]
        assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
        set jobname [get_job_name 2 16383]
        wait_for_migration_field 2 $jobname state failed
        assert {[string match {*A slot on the target node is being manually imported or migrated*} [dict get [get_migration_by_name 2 $jobname] message]]}
        assert_match "OK" [R 0 CLUSTER SETSLOT 16383 STABLE]

        # Cleanup
        set_debug_prevent_pause 0
    }

    test "Test CLUSTER CANCELSLOTMIGRATIONS" {
        set_debug_prevent_pause 1
        assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16382 16382 NODE $node0_id]
        assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
        set jobname1 [get_job_name 2 16382]
        set jobname2 [get_job_name 2 16383]
        wait_for_migration_field 2 $jobname1 state waiting-to-pause
        wait_for_migration_field 2 $jobname2 state waiting-to-pause

        # Also up on the target
        assert {[dict get [get_migration_by_name 0 $jobname1] state] eq "waiting-for-paused"}
        assert {[dict get [get_migration_by_name 0 $jobname2] state] eq "waiting-for-paused"}

        assert_match "OK" [R 2 CLUSTER CANCELSLOTMIGRATIONS]

        # Jobs are no longer up, migration logs say cancelled
        assert {[dict get [get_migration_by_name 2 $jobname1] state] eq "cancelled"}
        assert {[dict get [get_migration_by_name 2 $jobname2] state] eq "cancelled"}
        wait_for_migration_field 0 $jobname1 state failed
        wait_for_migration_field 0 $jobname2 state failed

        # Cleanup
        set_debug_prevent_pause 0
    }

    set 0_slot_tag "{06S}"
    set 5462_slot_tag "{450}"
    set 16379_slot_tag "{YY}"
    set 16380_slot_tag "{wu}"
    set 16381_slot_tag "{0TG}"
    set 16382_slot_tag "{4oi}"
    set 16383_slot_tag "{6ZJ}"

    test "Slot migration won't migrate the functions" {
        assert_does_not_resync {
            # R 2 load a function then trigger a slot migration to R 0
            set_debug_prevent_pause 1
            populate 1 "$16383_slot_tag:" 1000 -2
            R 2 function load {#!lua name=test1
                server.register_function('test1', function() return 'hello1' end)
            }
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
            set jobname [get_job_name 2 16383]

            # Load a function after the snapshot
            wait_for_migration_field 2 $jobname state waiting-to-pause
            populate 1 "$16383_slot_tag:" 1000 -2
            R 2 function load {#!lua name=test2
                server.register_function('test2', function() return 'hello2' end)
            }
            set_debug_prevent_pause 0
            wait_for_migration 0 16383

            # Make sure R 0 does not have any function.
            assert_match {} [R 0 function list]

            # R 0 load a function then migrate the slot back to R 2
            set_debug_prevent_pause 1
            populate 1 "$16383_slot_tag:" 1000 0
            R 0 function load {#!lua name=test1
                server.register_function('test1', function() return 'hello1' end)
            }
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node2_id]
            set jobname [get_job_name 0 16383]

            # Load a function after the snapshot
            wait_for_migration_field 0 $jobname state waiting-to-pause
            populate 1 "$16383_slot_tag:" 1000 0
            R 0 function load {#!lua name=test2
                server.register_function('test2', function() return 'hello2' end)
            }
            set_debug_prevent_pause 0
            wait_for_migration 2 16383

            # Cleanup for next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 0 FUNCTION FLUSH SYNC]
            assert_match "OK" [R 2 FLUSHDB SYNC]
            assert_match "OK" [R 2 FUNCTION FLUSH SYNC]
        }
    }

    test "Single source import - one shot" {
        assert_does_not_resync {
            # Populate data before migration
            populate 1000 "$16383_slot_tag:" 1000 -2

            # Perform one-shot import
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
            set jobname [get_job_name 2 16383]
            wait_for_migration 0 16383

            # Keys successfully migrated
            assert_match "1000" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

            # Also eventually reflected in replicas
            wait_for_countkeysinslot 3 16383 1000
            wait_for_countkeysinslot 5 16383 0

            # Migration log shows success on both ends
            assert {[dict get [get_migration_by_name 0 $jobname] state] eq "success"}
            assert {[dict get [get_migration_by_name 2 $jobname] state] eq "success"}

            # Cleanup for next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node2_id]
            wait_for_migration 2 16383
        }
    }

    test "Single source import - two phase" {
        assert_does_not_resync {
            set_debug_prevent_pause 1

            # Load data before the snapshot
            populate 333 "$16383_slot_tag:1:" 1000 -2

            # Load data while the snapshot is ongoing
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
            set jobname [get_job_name 2 16383]
            populate 333 "$16383_slot_tag:2:" 1000 -2

            # Load data after the snapshot
            wait_for_migration_field 2 $jobname state waiting-to-pause
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
            assert {[dict get [get_migration_by_name 0 $jobname] state] eq "success"}
            assert {[dict get [get_migration_by_name 2 $jobname] state] eq "success"}

            # Cleanup for the next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node2_id]
            wait_for_migration 2 16383
        }
    }

    # Catch-all test for covering commands sent during incremental replication
    test "Single source import - Incremental Command Coverage" {
        assert_does_not_resync {
            set_debug_prevent_pause 1

            # Do the snapshot
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
            set jobname [get_job_name 2 16383]
            wait_for_migration_field 2 $jobname state waiting-to-pause

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
            assert {[dict get [get_migration_by_name 0 $jobname] state] eq "success"}
            assert {[dict get [get_migration_by_name 2 $jobname] state] eq "success"}

            # Cleanup for the next test
            assert_match "OK" [R 0 FLUSHALL SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node2_id]
            wait_for_migration 2 16383
        }
    }

    # Test with both migrating slot < existing slot and vice versa, since a lot of the
    # kvstore logic is ordering dependent
    foreach testcase [list \
        [list 0 2 $node2_id 0 $0_slot_tag 16383 $16383_slot_tag] \
        [list 2 0 $node0_id 16383 $16383_slot_tag 0 $0_slot_tag] \
    ] {
        lassign $testcase source_idx target_idx target_id slot_to_migrate slot_to_migrate_tag slot_to_test slot_to_test_tag
        set_debug_prevent_pause 1
        test "Importing key containment (slot $slot_to_migrate from node $source_idx to $target_idx) - start migration" {
            populate 1000 "$slot_to_migrate_tag:1:" 1000 -$source_idx false 1000
            assert_match "OK" [R $source_idx CLUSTER MIGRATESLOTS SLOTSRANGE $slot_to_migrate $slot_to_migrate NODE $target_id]
            set jobname [get_job_name $source_idx $slot_to_migrate]
            wait_for_migration_field $source_idx $jobname state waiting-to-pause

            assert_match "1000" [R $target_idx CLUSTER COUNTKEYSINSLOT $slot_to_migrate]
        }
        test "Importing key containment (slot $slot_to_migrate from node $source_idx to $target_idx) - DBSIZE command excludes importing keys" {
            assert_match "0" [R $target_idx DBSIZE]
            assert_match "OK" [R $target_idx SET $slot_to_test_tag:my_key my_value]
            assert_match "1" [R $target_idx DBSIZE]
            assert_match "1" [R $target_idx DEL $slot_to_test_tag:my_key]
        }
        test "Importing key containment (slot $slot_to_migrate from node $source_idx to $target_idx) - KEYS command excludes importing keys" {
            assert_match "" [R $target_idx KEYS *]
            assert_match "" [R $target_idx KEYS $slot_to_migrate_tag:*]
            assert_match "OK" [R $target_idx SET $slot_to_test_tag:my_key my_value]
            assert_match "{$slot_to_test_tag:my_key}" [R $target_idx KEYS *]
            assert_match "" [R $target_idx KEYS $slot_to_migrate_tag:*]
            assert_match "1" [R $target_idx DEL $slot_to_test_tag:my_key]
        }

        test "Importing key containment (slot $slot_to_migrate from node $source_idx to $target_idx) - SCAN command excludes importing keys" {
            assert_match "0 {}" [R $target_idx SCAN 0]
            assert_match "OK" [R $target_idx SET $slot_to_test_tag:my_key my_value]
            assert_match "0 {{$slot_to_test_tag:my_key}}" [R $target_idx SCAN 0]
            assert_match "1" [R $target_idx DEL $slot_to_test_tag:my_key]
        }

        test "Importing key containment (slot $slot_to_migrate from node $source_idx to $target_idx) - RANDOMKEY command excludes importing keys" {
            assert_match "" [R $target_idx RANDOMKEY]
            assert_match "OK" [R $target_idx SET $slot_to_test_tag:my_key my_value]
            assert_match "$slot_to_test_tag:my_key" [R $target_idx RANDOMKEY]
            assert_match "1" [R $target_idx DEL $slot_to_test_tag:my_key]
        }

        foreach eviction_policy {
            allkeys-random
            allkeys-lru
            allkeys-lfu
            volatile-random
            volatile-lru
            volatile-lfu
            volatile-ttl
        } {
            test "Importing key containment (slot $slot_to_migrate from node $source_idx to $target_idx) - $eviction_policy eviction excludes importing keys" {
                # Eviction should only touch non-importing keys
                setup_eviction_test $target_idx $eviction_policy {
                    # Do 1000 evictions
                    set old_evictions [s -$target_idx evicted_keys]
                    set batch 1
                    while 1 {
                        populate 1000 "$slot_to_test_tag:$batch:" 1000 -$target_idx false 1000
                        incr batch
                        if {[s -$target_idx evicted_keys] > [expr $old_evictions + 1000]} {
                            break
                        }
                    }
                    # Validate keys are there
                    assert_match "1000" [R $target_idx CLUSTER COUNTKEYSINSLOT $slot_to_migrate]
                }
            }
        }

        test "Importing key containment (slot $slot_to_migrate from node $source_idx to $target_idx) - active expiration excludes importing keys" {
            # Populate 1000 keys with 1 second timeout, and do the snapshot
            assert_match "OK" [R $source_idx FLUSHALL SYNC]
            assert_match "OK" [R $target_idx FLUSHALL SYNC]
            assert_match "OK" [R $source_idx DEBUG SET-ACTIVE-EXPIRE 0]
            assert_match "OK" [R $target_idx DEBUG SET-ACTIVE-EXPIRE 0]
            populate 1000 "$slot_to_migrate_tag:1:" 1000 -$source_idx false 0.5
            populate 1000 "$slot_to_test_tag:1:" 1000 -$target_idx false 0.5
            assert_match "1" [R $source_idx HSETEX "$slot_to_migrate_tag:hfe" PX 500 FIELDS 1 field value]
            assert_match "1" [R $target_idx HSETEX "$slot_to_test_tag:hfe" PX 500 FIELDS 1 field value]
            assert_match "OK" [R $source_idx CLUSTER MIGRATESLOTS SLOTSRANGE $slot_to_migrate $slot_to_migrate NODE $target_id]
            set jobname [get_job_name $source_idx $slot_to_migrate]
            wait_for_migration_field $source_idx $jobname state waiting-to-pause
            assert_match "1001" [R $target_idx CLUSTER COUNTKEYSINSLOT $slot_to_migrate]

            # Unpause target expirations
            assert_match "OK" [R $target_idx DEBUG SET-ACTIVE-EXPIRE 1]

            # Wait for active expiration
            wait_for_countkeysinslot $target_idx $slot_to_test 0

            # Validate keys are still there
            assert_match "1001" [R $target_idx CLUSTER COUNTKEYSINSLOT $slot_to_migrate]

            # Resume source expirations
            assert_match "OK" [R $source_idx DEBUG SET-ACTIVE-EXPIRE 1]

            # Wait for the expirations to be propagated
            wait_for_countkeysinslot $target_idx $slot_to_migrate 0

            # Cleanup for the next test
            assert_match "OK" [R $source_idx FLUSHALL SYNC]
            assert_match "OK" [R $target_idx FLUSHALL SYNC]
            wait_for_migration_field $source_idx $jobname state failed
            wait_for_migration_field $target_idx $jobname state failed
        }
        set_debug_prevent_pause 0
    }

    test "Simultaneous imports" {
        assert_does_not_resync {
            # Populate data before migration
            populate 100 "$5462_slot_tag:1:" 1000 -1
            populate 100 "$16383_slot_tag:1:" 1000 -2

            # Prepare imports
            set_debug_prevent_pause 1
            assert_match "OK" [R 1 CLUSTER MIGRATESLOTS SLOTSRANGE 5462 5462 NODE $node0_id]
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
            set jobname1 [get_job_name 1 5462]
            set jobname2 [get_job_name 2 16383]
            populate 100 "$5462_slot_tag:2:" 1000 -1
            populate 100 "$16383_slot_tag:2:" 1000 -2
            wait_for_migration_field 1 $jobname1 state waiting-to-pause
            wait_for_migration_field 2 $jobname2 state waiting-to-pause
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
            assert {[dict get [get_migration_by_name 0 $jobname1] state] eq "success"}
            assert {[dict get [get_migration_by_name 1 $jobname1] state] eq "success"}
            assert {[dict get [get_migration_by_name 0 $jobname2] state] eq "success"}
            assert {[dict get [get_migration_by_name 2 $jobname2] state] eq "success"}

            # Cleanup for next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 5462 5462 NODE $node1_id]
            wait_for_migration 1 5462
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node2_id]
            wait_for_migration 2 16383
        }
    }

    test "Simultaneous exports" {
        ensure_slot_on_node 2 16382
        ensure_slot_on_node 2 16383
        assert_does_not_resync {
            # Populate data before migration
            populate 100 "$16382_slot_tag:1:" 1000 -2
            populate 100 "$16383_slot_tag:1:" 1000 -2

            # Prepare imports
            set_debug_prevent_pause 1
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16382 16382 NODE $node0_id]
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node1_id]
            set jobname1 [get_job_name 2 16382]
            set jobname2 [get_job_name 2 16383]
            populate 100 "$16382_slot_tag:2:" 1000 -2
            populate 100 "$16383_slot_tag:2:" 1000 -2
            wait_for_migration_field 2 $jobname1 state waiting-to-pause
            wait_for_migration_field 2 $jobname2 state waiting-to-pause
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
            assert {[dict get [get_migration_by_name 0 $jobname1] state] eq "success"}
            assert {[dict get [get_migration_by_name 2 $jobname1] state] eq "success"}
            assert {[dict get [get_migration_by_name 1 $jobname2] state] eq "success"}
            assert {[dict get [get_migration_by_name 2 $jobname2] state] eq "success"}

            # Cleanup for next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 1 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 16382 16382 NODE $node2_id]
            wait_for_migration 2 16382
            assert_match "OK" [R 1 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node2_id]
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
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16382 16382 NODE $node0_id]
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
            set jobname1 [get_job_name 2 16382]
            set jobname2 [get_job_name 2 16383]
            populate 100 "$16382_slot_tag:2:" 1000 -2
            populate 100 "$16383_slot_tag:2:" 1000 -2
            wait_for_migration_field 2 $jobname1 state waiting-to-pause
            wait_for_migration_field 2 $jobname2 state waiting-to-pause
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
            assert {[dict get [get_migration_by_name 0 $jobname1] state] eq "success"}
            assert {[dict get [get_migration_by_name 2 $jobname1] state] eq "success"}
            assert {[dict get [get_migration_by_name 0 $jobname2] state] eq "success"}
            assert {[dict get [get_migration_by_name 2 $jobname2] state] eq "success"}

            # Cleanup for next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 16382 16383 NODE $node2_id]
            wait_for_migration 2 16383
        }
    }

    test "Import slot range with multiple slots" {
        ensure_slot_on_node 2 16382
        ensure_slot_on_node 2 16383
        assert_does_not_resync {
            # Populate data before migration
            populate 500 "$16382_slot_tag:" 1000 -2
            populate 500 "$16383_slot_tag:" 1000 -2

            # Perform one-shot import
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16382 16383 NODE $node0_id]
            set jobname [get_job_name 2 16382]
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
            assert {[dict get [get_migration_by_name 0 $jobname] state] eq "success"}
            assert {[dict get [get_migration_by_name 2 $jobname] state] eq "success"}

            # Cleanup for next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 16382 16383 NODE $node2_id]
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
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16379 16380 16382 16383 NODE $node0_id]
            set jobname [get_job_name 2 16383]
            wait_for_migration 0 16383

            # Keys successfully migrated
            foreach slot {16379 16380 16382 16383} {
                assert_match "250" [R 0 CLUSTER COUNTKEYSINSLOT $slot]
                assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT $slot]
                wait_for_countkeysinslot 3 $slot 250
                wait_for_countkeysinslot 5 $slot 0
            }

            # Migration logs shows success on both ends
            assert {[dict get [get_migration_by_name 0 $jobname] state] eq "success"}
            assert {[dict get [get_migration_by_name 2 $jobname] state] eq "success"}

            # Cleanup for next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 16379 16380 16382 16383 NODE $node2_id]
            wait_for_migration 2 16383
        }
    }

    test "Export all slots from node" {
        assert_does_not_resync {
            # Populate data before migration
            populate 1000 "$16383_slot_tag:" 1000 -2

            # Perform one-shot import
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 10924 16383 NODE $node0_id]
            set jobname [get_job_name 2 10924]
            wait_for_migration 0 10924

            # Keys successfully migrated
            assert_match "1000" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

            # Also eventually reflected in replicas
            wait_for_countkeysinslot 3 16383 1000
            wait_for_countkeysinslot 5 16383 0

            # Migration logs shows success on both ends
            assert {[dict get [get_migration_by_name 0 $jobname] state] eq "success"}
            assert {[dict get [get_migration_by_name 2 $jobname] state] eq "success"}

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
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 10924 16383 NODE $node2_id]
            set jobname [get_job_name 0 10924]
            wait_for_migration 2 10924

            # Keys successfully migrated
            assert_match "1000" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
            assert_match "0" [R 0 CLUSTER COUNTKEYSINSLOT 16383]

            # Also eventually reflected in replicas
            wait_for_countkeysinslot 5 16383 1000
            wait_for_countkeysinslot 3 16383 0

            # Migration logs shows success on both ends
            assert {[dict get [get_migration_by_name 0 $jobname] state] eq "success"}
            assert {[dict get [get_migration_by_name 2 $jobname] state] eq "success"}

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
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
            set jobname [get_job_name 2 16383]
            populate 333 "$16383_slot_tag:2:" 1000 -2

            # Load data after the snapshot
            wait_for_migration_field 2 $jobname state waiting-to-pause
            populate 334 "$16383_slot_tag:3:" 1000 -2

            # Cancel and the data should be dropped
            assert_match "OK" [R 2 CLUSTER CANCELSLOTMIGRATIONS]
            assert {[dict get [get_migration_by_name 2 $jobname] state] eq "cancelled"}
            wait_for_migration_field 0 $jobname state failed
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
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
            set jobname [get_job_name 2 16383]
            wait_for_migration_field 2 $jobname state waiting-to-pause

            # Set maxmemory to simulate OOM
            assert_match "OK" [R 0 CONFIG SET maxmemory 1]

            # Loading more data should cause a failure
            populate 500 "$16383_slot_tag:3:" 1000 -2
            wait_for_migration_field 2 $jobname state failed
            wait_for_migration_field 0 $jobname state failed

            # Verify the keys are eventually dropped on target
            assert_match "1000" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
            wait_for_countkeysinslot 0 16383 0

            # Also eventually reflected in replicas
            wait_for_countkeysinslot 5 16383 1000
            wait_for_countkeysinslot 3 16383 0

            # Migration logs shows failure on both ends
            assert {[string match {*OOM*} [dict get [get_migration_by_name 0 $jobname] message]]}
            assert {[string match {*Connection lost to target*} [dict get [get_migration_by_name 2 $jobname] message]]}

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
        assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
        set jobname [get_job_name 2 16383]
        wait_for_migration_field 2 $jobname state waiting-to-pause

        # Make sure the replica has it
        wait_for_countkeysinslot 3 16383 500

        # Trigger failover
        assert_match "OK" [R 3 CLUSTER FAILOVER]

        # Jobs should be dropped on both ends
        wait_for_migration_field 2 $jobname state failed
        wait_for_migration_field 0 $jobname state failed

        # Keys should be dropped in target shard
        assert_match "0" [R 3 CLUSTER COUNTKEYSINSLOT 16383]
        assert_match "0" [R 0 CLUSTER COUNTKEYSINSLOT 16383]

        # Keys on existing shard are untouched
        assert_match "500" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
        assert_match "500" [R 5 CLUSTER COUNTKEYSINSLOT 16383]

        # Expect error messages
        assert {[string match {*I was demoted to a replica*} [dict get [get_migration_by_name 0 $jobname] message]]}
        assert {[string match {*Connection lost to target*} [dict get [get_migration_by_name 2 $jobname] message]]}

        # Cleanup for the next test
        assert_match "OK" [R 2 FLUSHDB SYNC]
        set_debug_prevent_pause 0
    }

    test "Slot export failed on failover" {
        # Load some data before the snapshot
        populate 500 "$0_slot_tag:1:" 1000 -3

        # Prepare and wait for ready
        set_debug_prevent_pause 1
        assert_match "OK" [R 3 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 NODE $node2_id]
        set jobname [get_job_name 3 0]
        wait_for_migration_field 3 $jobname state waiting-to-pause

        # Trigger failover
        assert_match "OK" [R 0 CLUSTER FAILOVER]

        # Jobs should be dropped on both ends
        wait_for_migration_field 3 $jobname state failed
        wait_for_migration_field 2 $jobname state failed

        # Keys should be dropped in target shard
        assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 0]
        wait_for_countkeysinslot 5 0 0

        # Keys on existing shard are untouched
        assert_match "500" [R 3 CLUSTER COUNTKEYSINSLOT 0]
        assert_match "500" [R 0 CLUSTER COUNTKEYSINSLOT 0]

        # Expect error messages. There are two error messages we could see, depending on the order of events.
        assert {
            [string match {*Slots are no longer owned by myself*} [dict get [get_migration_by_name 3 $jobname] message]] ||
            [string match {*Connection lost to target*} [dict get [get_migration_by_name 3 $jobname] message]]
        }
        assert {
            [string match {*Slots are no longer owned by source node*} [dict get [get_migration_by_name 2 $jobname] message]] ||
            [string match {*Connection lost to source*} [dict get [get_migration_by_name 2 $jobname] message]]
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
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 1 NODE $node2_id]
            set jobname [get_job_name 0 0]
            wait_for_migration_field 0 $jobname state waiting-to-pause

            # Force slot takeover
            assert_match "*BUMPED*" [R 1 CLUSTER BUMPEPOCH]
            assert_match "OK" [R 1 CLUSTER SETSLOT 0 NODE $node1_id]

            # Second job should get dropped on either end
            wait_for_migration_field 0 $jobname state failed
            wait_for_migration_field 2 $jobname state failed

            # Keys should be dropped on cancelled job
            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 0]
            assert_match "0" [R 5 CLUSTER COUNTKEYSINSLOT 0]

            # Migration logs for jobname shows failure on both ends
            # assert {[string match {*Slots are no longer owned by myself*} [dict get [get_migration_by_name 0 $jobname] message]]}
            # assert {[string match {*Slots are no longer owned by source node*} [dict get [get_migration_by_name 2 $jobname] message]]}

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
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 NODE $node2_id]
            set jobname [get_job_name 0 0]
            wait_for_migration_field 0 $jobname state failover-granted

            assert_match "slot_migration_in_progress" [s -0 paused_reason]

            # Our job should get dropped after pause timeout expires
            wait_for_migration_field 0 $jobname state failed
            wait_for_migration_field 2 $jobname state failed

            # Migration logs for job shows failure
            assert {[string match {*Unpaused before migration completed*} [dict get [get_migration_by_name 0 $jobname] message]]}
            assert {[string match {*Connection lost to source*} [dict get [get_migration_by_name 2 $jobname] message]]}

            # Validate no longer paused
            assert_match "none" [s -0 paused_reason]

            # Reset manual failover timeout
            R 0 CONFIG SET cluster-manual-failover-timeout $mf_timeout_old

            # Cleanup for the next test
            set_debug_prevent_failover 0
            assert_match "OK" [R 0 FLUSHDB SYNC]
        }
    }

    test "Export unpauses when cancelled" {
        assert_does_not_resync {
            # Lower manual failover timeout for this test
            set mf_timeout_old [lindex [R 0 CONFIG GET cluster-manual-failover-timeout] 1]
            R 0 CONFIG SET cluster-manual-failover-timeout 100

            # Use debug command to prevent failover
            set_debug_prevent_failover 1
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 NODE $node2_id]
            set jobname [get_job_name 0 0]
            wait_for_migration_field 0 $jobname state failover-granted

            assert_match "slot_migration_in_progress" [s -0 paused_reason]
            assert_match "write" [s -0 paused_actions]

            # Cancel the job
            assert_match "OK" [R 0 CLUSTER CANCELSLOTMIGRATIONS]
            wait_for_migration_field 0 $jobname state cancelled
            wait_for_migration_field 2 $jobname state failed

            assert_match "none" [s -0 paused_reason]

            # Reset manual failover timeout
            R 0 CONFIG SET cluster-manual-failover-timeout $mf_timeout_old

            # Cleanup for the next test
            set_debug_prevent_failover 0
            assert_match "OK" [R 0 FLUSHDB SYNC]
        }
    }

    test "CLUSTER SYNCSLOTS invalid state machine traversal" {
        assert_does_not_resync {
            assert_error "*ERR CLUSTER SYNCSLOTS REQUEST-PAUSE should only be used by slot migration clients*" {R 0 CLUSTER SYNCSLOTS REQUEST-PAUSE}
            assert_error "*ERR CLUSTER SYNCSLOTS REQUEST-FAILOVER should only be used by slot migration clients*" {R 0 CLUSTER SYNCSLOTS REQUEST-FAILOVER}
            assert_error "*ERR CLUSTER SYNCSLOTS SNAPSHOT-EOF should only be used by slot migration clients*" {R 0 CLUSTER SYNCSLOTS SNAPSHOT-EOF}
            assert_error "*ERR CLUSTER SYNCSLOTS PAUSED should only be used by slot migration clients*" {R 0 CLUSTER SYNCSLOTS PAUSED}
            assert_error "*ERR CLUSTER SYNCSLOTS FAILOVER-GRANTED should only be used by slot migration clients*" {R 0 CLUSTER SYNCSLOTS FAILOVER-GRANTED}
            assert_error "*ERR CLUSTER SYNCSLOTS ACK should only be used by slot migration clients*" {R 0 CLUSTER SYNCSLOTS ACK}
            assert_error "*syntax error*" {R 0 CLUSTER SYNCSLOTS UNKNOWN}

            assert_causes_conn_drop 0 {
                $client CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id NAME $fake_jobname SLOTSRANGE 16383 16383
                $client CLUSTER SYNCSLOTS SNAPSHOT-EOF
                $client CLUSTER SYNCSLOTS SNAPSHOT-EOF
            }
            assert_causes_conn_drop 0 {
                $client CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id NAME $fake_jobname SLOTSRANGE 16383 16383
                $client CLUSTER SYNCSLOTS PAUSED
            }
            assert_causes_conn_drop 0 {
                $client CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id NAME $fake_jobname SLOTSRANGE 16383 16383
                $client CLUSTER SYNCSLOTS FAILOVER-GRANTED
            }
        }
    }

    test "CLUSTER SYNCSLOTS ESTABLISH command interface" {
        assert_does_not_resync {
            # No arguments
            assert_error "*syntax error*" {R 0 CLUSTER SYNCSLOTS ESTABLISH}

            # No target
            assert_error "*syntax error*" {R 0 CLUSTER SYNCSLOTS ESTABLISH NAME $fake_jobname SLOTSRANGE 0 0}

            # No jobname
            assert_error "*syntax error*" {R 0  CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id SLOTSRANGE 0 0}

            # No slotsrange
            assert_error "*syntax error*" {R 0 CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id NAME $fake_jobname}

            # No end slot
            assert_error "*No end slot for final slot range*" {R 0 CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id NAME $fake_jobname SLOTSRANGE 0}

            # Unknown target
            assert_error "*Target node does not know the source node*" {R 0 CLUSTER SYNCSLOTS ESTABLISH SOURCE $fake_jobname NAME $fake_jobname SLOTSRANGE 16383 16383}

            # Unowned slotsrange
            assert_error "*Target node does not agree about current slot ownership*" {R 0 CLUSTER SYNCSLOTS ESTABLISH SOURCE $node1_id NAME $fake_jobname SLOTSRANGE 16383 16383}

            # Not primary
            assert_error "*Target node is not a primary*" {R 3 CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id NAME $fake_jobname SLOTSRANGE 16383 16383}

            # Invalid target name
            assert_error "*syntax error*" {R 0 CLUSTER SYNCSLOTS ESTABLISH SOURCE invalid NAME $fake_jobname SLOTSRANGE 16383 16383}

            # Invalid job name
            assert_error "*syntax error*" {R 0 CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id NAME invalid SLOTSRANGE 16383 16383}

            # Duplicated fields
            assert_error "*syntax error*" {R 0 CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id NAME $fake_jobname SLOTSRANGE 16383 16383 SLOTSRANGE 16383 16383}
            assert_error "*syntax error*" {R 0 CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id NAME $fake_jobname NAME $fake_jobname SLOTSRANGE 16383 16383}
            assert_error "*syntax error*" {R 0 CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id SOURCE $node2_id NAME $fake_jobname SLOTSRANGE 16383 16383}

            # Unknown field
            assert_error "*syntax error*" {R 0 CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id NAME $fake_jobname SLOTSRANGE 16383 16383 BAD_FIELD bad_value}

            # Already importing
            set_debug_prevent_pause 1
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
            set jobname [get_job_name 2 16383]
            wait_for_migration_field 0 $jobname state waiting-for-paused
            assert_error "*Slot is already being imported on the target by a different migration*" {R 0 CLUSTER SYNCSLOTS ESTABLISH SOURCE $node2_id NAME $fake_jobname SLOTSRANGE 16383 16383}
            assert_match "OK" [R 2 CLUSTER CANCELSLOTMIGRATIONS]
            wait_for_migration_field 0 $jobname state failed
            set_debug_prevent_pause 0
        }
    }

    test "FLUSH on target during import" {
        assert_does_not_resync {
            set_debug_prevent_pause 1

            # Load data before the snapshot
            populate 1000 "$16383_slot_tag:1:" 1000 -2

            foreach command {
                {FLUSHDB SYNC}
                {FLUSHDB ASYNC}
                {FLUSHALL SYNC}
                {FLUSHALL ASYNC}
            } {

                # Do the import
                assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
                set jobname [get_job_name 2 16383]

                # Keys should be on both source and destination
                assert_match "1000" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
                wait_for_countkeysinslot 0 16383 1000

                # Now run FLUSHDB SYNC on the target
                assert_match "OK" [eval R 0 $command]

                # Target should fail the migration
                wait_for_migration_field 2 $jobname state failed
                wait_for_migration_field 0 $jobname state failed
                assert {[string match {*Data was flushed*} [dict get [get_migration_by_name 0 $jobname] message]]}
                assert {[string match {*Connection lost to target*} [dict get [get_migration_by_name 2 $jobname] message]]}
                wait_for_countkeysinslot 0 16383 0
                wait_for_countkeysinslot 3 16383 0
            }

            # Cleanup
            assert_match "OK" [R 2 FLUSHDB SYNC]
            set_debug_prevent_pause 0
        }
    }

    test "FLUSH on source during export" {
        assert_does_not_resync {
            set_debug_prevent_pause 1

            foreach command {
                {FLUSHDB SYNC}
                {FLUSHDB ASYNC}
                {FLUSHALL SYNC}
                {FLUSHALL ASYNC}
            } {
                # Load data before the snapshot
                populate 1000 "$16383_slot_tag:1:" 1000 -2

                # Do the import
                assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
                set jobname [get_job_name 2 16383]

                # Keys should be on both source and destination
                assert_match "1000" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
                wait_for_countkeysinslot 0 16383 1000

                # FLUSH on the source should fail the import
                assert_match "OK" [eval R 2 $command]
                wait_for_migration_field 2 $jobname state failed
                wait_for_migration_field 0 $jobname state failed
                assert {[string match {*Data was flushed*} [dict get [get_migration_by_name 2 $jobname] message]]}
                assert {[string match {*Connection lost to source*} [dict get [get_migration_by_name 0 $jobname] message]]}
                assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]
                wait_for_countkeysinslot 0 16383 0
                wait_for_countkeysinslot 3 16383 0
            }

            # Cleanup
            set_debug_prevent_pause 0
        }
    }

    test "Import cancelled when source hangs" {
        assert_does_not_resync {
            R 2 CONFIG SET repl-timeout 2

            # Load data before the snapshot
            populate 333 "$0_slot_tag:1:" 1000 -0

            # Load data while the snapshot is ongoing
            set_debug_prevent_pause 1
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 NODE $node2_id]
            set jobname [get_job_name 0 0]
            populate 333 "$0_slot_tag:2:" 1000 -0

            # Load data after the snapshot
            wait_for_migration_field 0 $jobname state waiting-to-pause
            populate 334 "$0_slot_tag:3:" 1000 -0

            # Now pause source
            set node0_pid  [srv 0 pid]
            pause_process $node0_pid

            # The import should eventually fail due to no ACKs
            wait_for_migration_field 2 $jobname state failed
            assert {[string match {*Timed out after too long with no interaction*} [dict get [get_migration_by_name 2 $jobname] message]]}

            # After resuming, it should be reflected on source
            resume_process $node0_pid
            wait_for_migration_field 0 $jobname state failed
            assert {[string match {*Connection lost to target*} [dict get [get_migration_by_name 0 $jobname] message]]}

            # Cleanup for the next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            R 2 CONFIG SET repl-timeout 60
            set_debug_prevent_pause 0
        }
    }

    test "Export cancelled when target hangs" {
        assert_does_not_resync {
            R 0 CONFIG SET repl-timeout 2

            # Load data before the snapshot
            populate 333 "$0_slot_tag:1:" 1000 -0

            # Load data while the snapshot is ongoing
            set_debug_prevent_pause 1
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 NODE $node2_id]
            set jobname [get_job_name 0 0]
            populate 333 "$0_slot_tag:2:" 1000 -0

            # Load data after the snapshot
            wait_for_migration_field 0 $jobname state waiting-to-pause
            populate 334 "$0_slot_tag:3:" 1000 -0

            # Now pause target
            set node2_pid [srv -2 pid]
            pause_process $node2_pid

            # The export should eventually fail due to no ACKs
            wait_for_migration_field 0 $jobname state failed
            assert {[string match {*Timed out after too long with no interaction*} [dict get [get_migration_by_name 0 $jobname] message]]}

            # After resuming, it should be reflected on target
            resume_process $node2_pid
            wait_for_migration_field 2 $jobname state failed
            assert {[string match {*Connection lost to source*} [dict get [get_migration_by_name 2 $jobname] message]]}

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
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
            set jobname [get_job_name 2 16383]
            wait_for_migration 0 16383

            # Keys successfully migrated
            assert_match "1000" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
            assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

            # Also eventually reflected in replicas
            wait_for_countkeysinslot 3 16383 1000
            wait_for_countkeysinslot 5 16383 0

            # Migration log shows success on both ends
            assert {[dict get [get_migration_by_name 0 $jobname] state] eq "success"}
            assert {[dict get [get_migration_by_name 2 $jobname] state] eq "success"}

            # Cleanup for next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node2_id]
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
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
            set jobname [get_job_name 2 16383]

            # Should be denied
            wait_for_migration_field 2 $jobname state failed
            assert_match {*Failed to AUTH to target node*} [dict get [get_migration_by_name 2 $jobname] message]

            # Cleanup for next test
            R 0 CONFIG SET requirepass ""
            R 2 CONFIG SET primaryauth ""
        }
    }

    test "Connection drop during import causes failure" {
        assert_does_not_resync {
            # Start an import
            set_debug_prevent_pause 1
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
            set jobname [get_job_name 2 16383]
            wait_for_migration_field 2 $jobname state waiting-to-pause
            set import_client_id [get_client_id_by_last_cmd [srv -0 client] "cluster|syncslots"]

            # Use CLIENT KILL to drop the connection
            assert_match "1" [R 0 CLIENT KILL ID $import_client_id]

            # Migration should be failed
            wait_for_migration_field 2 $jobname state failed
            wait_for_migration_field 0 $jobname state failed
            assert {[string match {*Connection lost to source*} [dict get [get_migration_by_name 0 $jobname] message]]}
            assert {[string match {*Connection lost to target*} [dict get [get_migration_by_name 2 $jobname] message]]}

            # Cleanup for next test
            set_debug_prevent_pause 0
        }
    }

    test "Export client buffer enforcement" {
        assert_does_not_resync {
            set_debug_prevent_pause 1
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
            set jobname [get_job_name 2 16383]
            wait_for_migration_field 2 $jobname state waiting-to-pause
            set old_cob [lindex [R 2 config get client-output-buffer-limit] 1]
            R 2 config set client-output-buffer-limit "replica 10k 0 0"

            # Pause the target
            set node0_pid [srv -0 pid]
            pause_process $node0_pid

            set migration [get_migration_by_name 2 $jobname]
            # Accumulate a large backlog on the source, it should eventually kill the client
            for {set i 0} {$i < 100} {incr i} {
                populate 1000 "$16383_slot_tag:" 1000 -2
                set migration [get_migration_by_name 2 $jobname]
                if {[dict get $migration state] eq "failed"} {
                    break
                }
            }
            if {[dict get $migration state] ne "failed"} {
                fail "Export was not failed after writing 100 MiB of changes, current state: $migration"
            }

            resume_process $node0_pid

            # Migration should be failed
            wait_for_migration_field 2 $jobname state failed
            wait_for_migration_field 0 $jobname state failed
            assert {[string match {*Connection lost to source*} [dict get [get_migration_by_name 0 $jobname] message]]}
            assert {[string match {*Connection lost to target*} [dict get [get_migration_by_name 2 $jobname] message]]}

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
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16381 16381 16383 16383 NODE $node0_id]
            set jobname [get_job_name 2 16381]
            foreach tag $tags {
                populate 333 "$tag:2:" 1000 -2
            }

            # Load data after the snapshot
            wait_for_migration_field 2 $jobname state waiting-to-pause
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
            assert {[dict get [get_migration_by_name 0 $jobname] state] eq "success"}
            assert {[dict get [get_migration_by_name 2 $jobname] state] eq "success"}

            # Cleanup for the next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 2 FLUSHDB SYNC]
            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 16381 16381 16383 16383 NODE $node2_id]
            wait_for_migration 2 16381
            wait_for_migration 2 16383
        }
    }

    test "Export client buffer excluded from maxmemory" {
        assert_does_not_resync {
            set_debug_prevent_pause 1
            setup_eviction_test 2 allkeys-random {
                assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
                set jobname [get_job_name 2 16383]
                wait_for_migration_field 2 $jobname state waiting-to-pause

                set prev_evictions [s -2 evicted_keys]

                # Fill source with keys until full
                set batch 1
                while 1 {
                    populate 1000 "$16383_slot_tag:$batch" 1000 -2
                    incr batch
                    if {[s -2 evicted_keys] > $prev_evictions} {
                        break
                    }
                }

                # Pause the target
                set node0_pid [srv -0 pid]
                pause_process $node0_pid

                # Accumulate backlog on the source, it should not cause an eviction loop
                catch {
                    for {set i 0} {$i < 100} {incr i} {
                        # Continue to add keys
                        populate 1000 "$16383_slot_tag:" 1000 -2

                        # Eventually the output buffer should go over the limit
                        set migration [get_migration_by_name 2 $jobname]
                        if {[dict get $migration state] eq "failed"} {
                            break
                        }
                    }
                } err
                if {$err != ""} {
                    fail "error during populate $err"
                }
                if {[dict get $migration state] ne "failed"} {
                    fail "Export was not failed after writing 100 MiB of changes, current state: $migration"
                }

                # If maxmemory includes the client buffer, we would see all keys evicted
                assert {[R 2 CLUSTER COUNTKEYSINSLOT 16383] > 0}

                resume_process $node0_pid

                # Import should be failed
                wait_for_migration_field 2 $jobname state failed
                wait_for_migration_field 0 $jobname state failed
                assert_match "*Connection lost to source*" [dict get [get_migration_by_name 0 $jobname] message]
                assert_match "*Connection lost to target*" [dict get [get_migration_by_name 2 $jobname] message]

                # Cleanup for the next test
                assert_match "OK" [R 2 FLUSHDB SYNC]
            }
            set_debug_prevent_pause 0
        }
    }

    test "Migration not cancelled when snapshot takes more time than repl-timeout" {
        assert_does_not_resync {
            R 2 CONFIG SET repl-timeout 2

            # Load keys before the snapshot to target a snapshot time > 2sec
            # 50 * 100ms = 5 sec
            R 2 CONFIG SET rdb-key-save-delay 100000
            populate 50 "$0_slot_tag:1:" 1000 -0

            assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 NODE $node2_id]
            set jobname [get_job_name 0 0]

            wait_for_migration 2 0

            # Keys successfully migrated
            assert_match "50" [R 2 CLUSTER COUNTKEYSINSLOT 0]
            assert_match "0" [R 0 CLUSTER COUNTKEYSINSLOT 0]

            # Also eventually reflected in replicas
            wait_for_countkeysinslot 5 0 50
            wait_for_countkeysinslot 3 0 0

            # Migration log shows success on both ends
            assert {[dict get [get_migration_by_name 0 $jobname] state] eq "success"}
            assert {[dict get [get_migration_by_name 2 $jobname] state] eq "success"}

            # Cleanup for next test
            assert_match "OK" [R 0 FLUSHDB SYNC]
            assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 NODE $node0_id]
            wait_for_migration 0 0
            R 2 CONFIG SET repl-timeout 60
            R 2 CONFIG SET rdb-key-save-delay 0
        }
    }

    test "Resynchronization during migration" {
        set_debug_prevent_pause 1

        # Load data before the snapshot
        populate 333 "$16379_slot_tag:1:" 1000 -2

        # Load data while the snapshot is ongoing
        assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16379 16383 NODE $node0_id]
        set jobname [get_job_name 2 16383]
        populate 333 "$16381_slot_tag:2:" 1000 -2

        # Load data after the snapshot
        wait_for_migration_field 2 $jobname state waiting-to-pause
        populate 334 "$16383_slot_tag:3:" 1000 -2

        # Resync the replicas on both ends
        assert_match "OK" [R 0 SAVE]
        assert_match "OK" [R 3 CLUSTER REPLICATE NO ONE]
        assert_match "OK" [R 5 CLUSTER REPLICATE NO ONE]
        assert_match "OK" [R 3 CLUSTER REPLICATE $node0_id]
        assert_match "OK" [R 5 CLUSTER REPLICATE $node2_id]
        

        # Wait for resync
        wait_for_condition 50 1000 {
            [status [srv -3 client] master_link_status] == "up"
        } else {
            fail "Node 3 is not synced"
        }
        wait_for_condition 50 1000 {
            [status [srv -5 client] master_link_status] == "up"
        } else {
            fail "Node 5 is not synced"
        }

        # Allow migration to complete and verify
        set_debug_prevent_pause 0
        wait_for_migration 0 16383
        assert_match "333" [R 0 CLUSTER COUNTKEYSINSLOT 16379]
        assert_match "333" [R 0 CLUSTER COUNTKEYSINSLOT 16381]
        assert_match "334" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
        assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16379]
        assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16381]
        assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

        # Also eventually reflected in replicas
        wait_for_countkeysinslot 3 16379 333
        wait_for_countkeysinslot 3 16381 333
        wait_for_countkeysinslot 3 16383 334
        wait_for_countkeysinslot 5 16379 0
        wait_for_countkeysinslot 5 16381 0
        wait_for_countkeysinslot 5 16383 0

        # Migration log shows success on both ends
        assert {[dict get [get_migration_by_name 0 $jobname] state] eq "success"}
        assert {[dict get [get_migration_by_name 2 $jobname] state] eq "success"}

        # Cleanup for the next test
        assert_match "OK" [R 0 FLUSHDB SYNC]
        assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 16379 16383 NODE $node2_id]
        wait_for_migration 2 16383
    }

}

start_cluster 3 0 {tags {logreqres:skip external:skip cluster}} {

    set node0_id [R 0 CLUSTER MYID]
    set node1_id [R 1 CLUSTER MYID]
    set node2_id [R 2 CLUSTER MYID]

    set 16383_slot_tag "{6ZJ}"

    test "Migration with no replicas" {
        set_debug_prevent_pause 1

        # Load data before the snapshot
        populate 333 "$16383_slot_tag:1:" 1000 -2

        # Load data while the snapshot is ongoing
        assert_match "OK" [R 2 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node0_id]
        set jobname [get_job_name 2 16383]
        populate 333 "$16383_slot_tag:2:" 1000 -2

        # Load data after the snapshot
        wait_for_migration_field 2 $jobname state waiting-to-pause
        populate 334 "$16383_slot_tag:3:" 1000 -2

        # Allow migration to complete and verify
        set_debug_prevent_pause 0
        wait_for_migration 0 16383
        assert_match "1000" [R 0 CLUSTER COUNTKEYSINSLOT 16383]
        assert_match "0" [R 2 CLUSTER COUNTKEYSINSLOT 16383]

        # Cleanup
        assert_match "OK" [R 0 FLUSHALL SYNC]
        assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 16383 16383 NODE $node2_id]
    }

    test "Read syncslots establish response timeout" {
        R 0 CONFIG SET repl-timeout 2
        
        # Pause to prevent connection success
        set target_pid  [srv -2 pid]
        pause_process $target_pid

        assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 NODE $node2_id]
        set jobname [get_job_name 0 0]

        # Connecting will fail
        wait_for_migration_field 0 $jobname state failed
        assert_match "*Timed out after too long with no interaction*" [dict get [get_migration_by_name 0 $jobname] message]

        resume_process $target_pid
        R 0 CONFIG SET repl-timeout 60
    }

    test "Migration cannot connect to target" {
        # Shutdown to prevent connection success
        catch {R 2 shutdown nosave}
        assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 NODE $node2_id]
        set jobname [get_job_name 0 0]

        # Connecting will fail
        wait_for_migration_field 0 $jobname state failed
        assert_match "*Unable to connect to target node: Connection refused*" [dict get [get_migration_by_name 0 $jobname] message]
    }

}

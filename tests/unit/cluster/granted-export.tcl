# Once the source of an atomic slot export has replied FAILOVER-GRANTED, a
# failure of the export must not unpause it: the target may take the slots at
# any time, so a write the source accepts after unpausing would be lost.
# Each case holds the export in failover-granted (granted_export_check_trigger
# in tests/support/granted_export_util.tcl), runs one trigger on the source,
# and checks that the source stays paused until the target takes over.

source tests/support/granted_export_util.tcl

set overrides {cluster-allow-replica-migration no cluster-node-timeout 15000}
set tags {logreqres:skip external:skip needs:debug cluster}

start_cluster 3 3 [list tags $tags overrides $overrides] {
    test "Granted export: DEBUG RELOAD on the source does not unpause it" {
        granted_export_check_trigger {R 0 DEBUG RELOAD}
    }
}

start_cluster 3 3 [list tags $tags overrides $overrides] {
    test "Granted export: CLUSTER CANCELSLOTMIGRATIONS on the source does not unpause it" {
        # CANCELSLOTMIGRATIONS is not a write command, so the pause doesn't
        # hold it. Its reply is not checked: it may report the granted export
        # as not cancellable, but it must not unpause the source.
        granted_export_check_trigger {catch {R 0 CLUSTER CANCELSLOTMIGRATIONS}}
    }
}

start_cluster 3 3 [list tags $tags overrides $overrides] {
    test "Granted export: client FLUSHALL on the source is postponed" {
        # FLUSHALL is a write command, so the pause holds it until the
        # takeover and it never fails the export.
        set rd [valkey_deferring_client]
        test_defer [list $rd close]
        granted_export_check_trigger {
            $rd FLUSHALL
            $rd flush
        }
        assert_equal "OK" [$rd read]
    }
}

# A correct target sends nothing on the migration link after it reads
# FAILOVER-GRANTED, but a buggy or newer one could. In these cases node 2
# announces the port of a fake target (tests/helpers/fake_slot_migration_target.tcl),
# so the source's migration link goes to the fake. The fake plays the target
# up to FAILOVER-GRANTED, then sends one more command. The source must stay
# paused instead of failing the export and accepting writes. The test then
# makes node 2 claim the slot, as the target would, and checks that the held
# write is redirected rather than acknowledged and lost.
proc granted_export_check_target_command {args} {
    set key "{06S}:granted"
    assert_match "OK" [R 0 SET $key old]

    set port [find_available_port $::baseport $::portcount]
    set out [tmpfile fake-target-out]
    test_defer [list file delete $out]
    set pid [exec [info nameofexecutable] tests/helpers/fake_slot_migration_target.tcl $port {*}$args > $out &]
    test_defer [list catch [list exec kill -9 $pid]]
    wait_for_condition 50 50 {
        [ping_server 127.0.0.1 $port]
    } else {
        fail "Failed to start the fake target"
    }

    set node2_id [R 2 CLUSTER MYID]
    test_config_set {R 2} cluster-announce-port $port
    wait_for_condition 100 100 {
        [string match "*:$port@*" [dict get [cluster_get_node_by_id 0 $node2_id] addr]]
    } else {
        fail "Node 0 did not learn node 2's new port"
    }

    assert_match "OK" [R 0 CLUSTER MIGRATESLOTS SLOTSRANGE 0 0 NODE $node2_id]
    wait_for_condition 100 100 {
        [string match "*SENT*" [exec cat $out]]
    } else {
        fail "The fake target did not get FAILOVER-GRANTED: [R 0 CLUSTER GETSLOTMIGRATIONS]"
    }
    # Give the source time to process the extra command.
    after 300
    set jobname [dict get [granted_export_get_job 0] name]
    assert_equal "failover-granted" [dict get [granted_export_get_job 0 $jobname] state]
    assert_equal "slot_migration_in_progress" [s 0 paused_reason]

    set rd [valkey_deferring_client]
    test_defer [list $rd close]
    $rd SET $key new
    $rd flush
    wait_for_blocked_client
    assert_equal "old" [R 0 GET $key]

    # Take over slot 0 the way the real target would after FAILOVER-GRANTED:
    # node 2 claims it with a higher config epoch. The source must then
    # finish the export and redirect the held SET, so no write it
    # acknowledged is dropped with the slot.
    R 2 CLUSTER BUMPEPOCH
    assert_equal "OK" [R 2 CLUSTER SETSLOT 0 NODE $node2_id]
    wait_for_condition 100 100 {
        [granted_export_slot_in_ranges [dict get [cluster_get_node_by_id 0 $node2_id] slots] 0]
    } else {
        fail "Node 0 did not see node 2 take over slot 0"
    }
    assert_error "MOVED 0 *" {$rd read}
    assert_equal "success" [dict get [granted_export_get_job 0 $jobname] state]
    assert_equal "none" [s 0 paused_reason]
}

set tags {logreqres:skip external:skip tls:skip needs:debug cluster}

start_cluster 3 0 [list tags $tags] {
    test "Granted export: unknown SYNCSLOTS subcommand from the target does not unpause it" {
        granted_export_check_target_command CLUSTER SYNCSLOTS BOGUS
    }
}

start_cluster 3 0 [list tags $tags] {
    test "Granted export: an error reply on the export link does not unpause it" {
        # ESTABLISH on a client that is already a slot migration client is
        # rejected with an error, which reaches afterErrorReply().
        granted_export_check_target_command CLUSTER SYNCSLOTS ESTABLISH
    }
}

start_cluster 3 0 [list tags $tags] {
    test "Granted export: a repeated REQUEST-PAUSE from the target does not unpause it" {
        granted_export_check_target_command CLUSTER SYNCSLOTS REQUEST-PAUSE
    }
}

start_cluster 3 0 [list tags $tags] {
    test "Granted export: a repeated REQUEST-FAILOVER from the target does not unpause it" {
        granted_export_check_target_command CLUSTER SYNCSLOTS REQUEST-FAILOVER
    }
}

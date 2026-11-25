# A replica that Sentinel converted back to replica (+convert-to-slave) and
# that is promoted again before the next INFO must still get the full wait
# before Sentinel demotes it again, so a new configuration can arrive.

source "../tests/includes/init-tests.tcl"

set ::replica_id 1
set ::replica_port [RPort $::replica_id]

proc replica_field_from_sentinel_0 {port field} {
    foreach replica [S 0 SENTINEL REPLICAS mymaster] {
        if {[dict get $replica port] == $port} {
            return [dict get $replica $field]
        }
    }
    return {}
}

# Wait for Sentinel 0 to convert the replica back to replica. Return a time
# (in ms) the INFO that triggered +convert-to-slave is not older than: the last
# time the replica was still seen as primary, or $lower_bound.
proc wait_for_convert_to_slave {lower_bound} {
    set before_convert $lower_bound
    for {set j 0} {$j < 3000} {incr j} {
        set now [clock milliseconds]
        if {[RI $::replica_id role] eq {slave}} {
            return $before_convert
        }
        set before_convert $now
        after 10
    }
    fail "Sentinel did not convert the replica back"
}

# Wait until Sentinel 0 processed the replies to all the commands it sent to
# the replica.
proc wait_for_no_pending_commands {} {
    wait_for_condition 500 10 {
        [replica_field_from_sentinel_0 $::replica_port link-pending-commands] == 0
    } else {
        fail "Sentinel still has pending commands"
    }
}

# Promote the replica again and check that Sentinel 0 did not process any INFO
# after the one that triggered +convert-to-slave, so it did not observe the
# replica role in between.
#
# Sentinel sends the next INFO one info-period (10s) after it processed that
# one. $before_convert can be later than it only by the time the REPLICAOF
# takes to reach the instance, so the checks below hold unless a local command
# is delayed by several seconds.
#
# Return a time (in ms) such that any INFO Sentinel 0 processes after it was
# sent after the new promotion.
proc promote_again_before_next_info {before_convert} {
    # Replies to requests sent before +convert-to-slave may still be on their
    # way to Sentinel. Wait until it processed all of them, so any INFO it
    # processes after the new promotion was sent after the conversion.
    wait_for_no_pending_commands
    R $::replica_id replicaof no one
    # Promoting well within the info-period means no INFO was sent after the
    # conversion and before the promotion.
    if {[clock milliseconds] > $before_convert + 5000} {
        fail "The replica was not promoted again soon enough"
    }
    set info_refresh [replica_field_from_sentinel_0 $::replica_port info-refresh]
    # Reading the clock after the reply can only make the INFO look newer.
    set last_info [expr {[clock milliseconds] - $info_refresh}]
    if {$last_info > $before_convert + 1000} {
        fail "Sentinel processed an INFO before the replica was promoted again"
    }
    # Every request sent before the promotion has been answered once there
    # are no pending commands, so a later INFO was sent after the promotion.
    wait_for_no_pending_commands
    assert_equal {master} [RI $::replica_id role]
    return [clock milliseconds]
}

# Return 1 if Sentinel 0 processed an INFO after $since (in ms). The clock is
# read before the request, so the reply latency cannot make an older INFO pass.
proc info_refreshed_since {since} {
    set now [clock milliseconds]
    expr {[replica_field_from_sentinel_0 $::replica_port info-refresh] < $now - $since}
}

# Wait for Sentinel 0 to process an INFO after $since (in ms), then check
# that it did not demote the replica.
proc assert_not_demoted_after_next_info {since} {
    wait_for_condition 1500 10 {
        [info_refreshed_since $since]
    } else {
        fail "Sentinel did not refresh INFO"
    }
    # A REPLICAOF would be queued while processing that INFO, so once Sentinel
    # has no pending commands it would have been executed.
    wait_for_no_pending_commands
    assert_equal {master} [RI $::replica_id role]
}

# Only Sentinel 0 monitors the primary: the others did not demote the replica,
# so they would legitimately demote it as soon as they see it as primary.
foreach_sentinel_id id {
    if {$id != 0} {
        S $id SENTINEL REMOVE mymaster
    }
}
S 0 SENTINEL SET mymaster down-after-milliseconds 30000

test "Replica promoted again right after +convert-to-slave is not demoted at once" {
    # With the default period, the next INFO comes 10s after the one that
    # triggers +convert-to-slave, leaving time to promote the replica again.
    S 0 SENTINEL DEBUG info-period 10000
    set start [clock milliseconds]
    R $::replica_id replicaof no one

    # Sentinel 0 sees the replica as a primary and, after the wait,
    # converts it back to replica.
    set before_convert [wait_for_convert_to_slave $start]

    set promoted [promote_again_before_next_info $before_convert]

    # The next INFO must be seen as a role change: the replica must not be
    # demoted before the wait elapses again.
    assert_not_demoted_after_next_info $promoted

    wait_for_condition 3000 10 {
        [RI $::replica_id role] eq {slave}
    } else {
        fail "Sentinel did not eventually convert the replica back"
    }
}

test "INFO replies sent before +convert-to-slave are ignored" {
    S 0 SENTINEL DEBUG info-period 10000
    R $::replica_id replicaof no one
    wait_for_condition 2000 10 {
        [replica_field_from_sentinel_0 $::replica_port role-reported] eq {master}
    } else {
        fail "Sentinel did not see the replica as primary"
    }

    # The sleep is longer than the info-period, so Sentinel sends INFO again
    # while the instance sleeps. When it wakes up, the first reply triggers
    # +convert-to-slave and the other ones, sent before the REPLICAOF, report
    # the instance as primary.
    set before_sleep [clock milliseconds]
    R $::replica_id debug sleep 12

    # The INFO that triggers +convert-to-slave is processed after the sleep.
    set before_convert [wait_for_convert_to_slave [expr {$before_sleep + 12000}]]

    set promoted [promote_again_before_next_info $before_convert]

    assert_not_demoted_after_next_info $promoted

    wait_for_condition 3000 10 {
        [RI $::replica_id role] eq {slave}
    } else {
        fail "Sentinel did not eventually convert the replica back"
    }
}

S 0 SENTINEL DEBUG info-period 10000

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

# Promote the replica again and check that Sentinel 0 did not process any INFO
# since $since (in ms), so it did not observe the replica role in between.
proc promote_again_before_next_info {since} {
    R $::replica_id replicaof no one
    set elapsed [expr {[clock milliseconds] - $since}]
    set info_refresh [replica_field_from_sentinel_0 $::replica_port info-refresh]
    if {$info_refresh < $elapsed} {
        fail "Sentinel processed an INFO before the replica was promoted again"
    }
}

# Wait for Sentinel 0 to process an INFO sent after $since (in ms), then check
# that it did not demote the replica.
proc assert_not_demoted_after_next_info {since} {
    wait_for_condition 1500 10 {
        [replica_field_from_sentinel_0 $::replica_port info-refresh] < [clock milliseconds] - $since
    } else {
        fail "Sentinel did not refresh INFO"
    }
    # The REPLICAOF would be sent while processing that INFO.
    after 1000
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
    R $::replica_id replicaof no one

    # Sentinel 0 sees the replica as a primary and, after the wait,
    # converts it back to replica.
    wait_for_condition 3000 10 {
        [RI $::replica_id role] eq {slave}
    } else {
        fail "Sentinel did not convert the replica back"
    }
    set converted [clock milliseconds]

    promote_again_before_next_info $converted
    set promoted [clock milliseconds]

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
    S 0 SENTINEL DEBUG info-period 2000
    R $::replica_id replicaof no one
    wait_for_condition 1000 10 {
        [replica_field_from_sentinel_0 $::replica_port role-reported] eq {master}
    } else {
        fail "Sentinel did not see the replica as primary"
    }

    # While the instance sleeps, Sentinel keeps sending INFO. When it wakes up,
    # the first reply triggers +convert-to-slave and the other ones, sent
    # before the REPLICAOF, report the instance as primary.
    R $::replica_id debug sleep 9
    S 0 SENTINEL DEBUG info-period 10000

    wait_for_condition 1000 10 {
        [RI $::replica_id role] eq {slave}
    } else {
        fail "Sentinel did not convert the replica back"
    }
    set converted [clock milliseconds]

    promote_again_before_next_info $converted
    set promoted [clock milliseconds]

    assert_not_demoted_after_next_info $promoted

    wait_for_condition 3000 10 {
        [RI $::replica_id role] eq {slave}
    } else {
        fail "Sentinel did not eventually convert the replica back"
    }
}

S 0 SENTINEL DEBUG info-period 10000

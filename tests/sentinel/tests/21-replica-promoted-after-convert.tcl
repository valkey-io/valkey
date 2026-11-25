# A replica that Sentinel converted back to replica (+convert-to-slave) and
# that is promoted again before the next INFO must still get the full wait
# before Sentinel demotes it again, so a new configuration can arrive.

source "../tests/includes/init-tests.tcl"

set replica_id 1
set replica_port [RPort $replica_id]

proc role_reported_by_sentinel_0 {port} {
    foreach replica [S 0 SENTINEL REPLICAS mymaster] {
        if {[dict get $replica port] == $port} {
            return [dict get $replica role-reported]
        }
    }
    return {}
}

# Only Sentinel 0 monitors the primary: the others did not demote the replica,
# so they would legitimately demote it as soon as they see it as primary.
foreach_sentinel_id id {
    if {$id != 0} {
        S $id SENTINEL REMOVE mymaster
    }
}
S 0 SENTINEL DEBUG info-period 2000
S 0 SENTINEL SET mymaster down-after-milliseconds 30000

test "Replica promoted again right after +convert-to-slave is not demoted at once" {
    R $replica_id replicaof no one

    # Sentinel 0 sees the replica as a primary and, after the wait,
    # converts it back to replica.
    wait_for_condition 3000 10 {
        [RI $replica_id role] eq {slave}
    } else {
        fail "Sentinel did not convert the replica back"
    }

    # Promote it again before the next INFO from Sentinel 0.
    R $replica_id replicaof no one

    # The next INFO must be seen as a role change: the replica must not be
    # demoted before the wait elapses again.
    after 5000
    assert_equal {master} [RI $replica_id role]

    wait_for_condition 3000 10 {
        [RI $replica_id role] eq {slave}
    } else {
        fail "Sentinel did not eventually convert the replica back"
    }
}

test "INFO replies sent before +convert-to-slave are ignored" {
    R $replica_id replicaof no one
    wait_for_condition 1000 10 {
        [role_reported_by_sentinel_0 $replica_port] eq {master}
    } else {
        fail "Sentinel did not see the replica as primary"
    }

    # While the instance sleeps, Sentinel keeps sending INFO. When it wakes up,
    # the first reply triggers +convert-to-slave and the other ones, sent
    # before the REPLICAOF, report the instance as primary.
    R $replica_id debug sleep 9
    S 0 SENTINEL DEBUG info-period 10000

    wait_for_condition 1000 10 {
        [RI $replica_id role] eq {slave}
    } else {
        fail "Sentinel did not convert the replica back"
    }

    # Promote it again before the next INFO from Sentinel 0, which comes
    # after the wait: it must be seen as a role change.
    R $replica_id replicaof no one
    after 13000
    assert_equal {master} [RI $replica_id role]

    wait_for_condition 3000 10 {
        [RI $replica_id role] eq {slave}
    } else {
        fail "Sentinel did not eventually convert the replica back"
    }
}

S 0 SENTINEL DEBUG info-period 10000

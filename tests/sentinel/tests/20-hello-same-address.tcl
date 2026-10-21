# A newer HELLO for the stored primary must reset the failover and notify
# client-reconfig-script (#4917).

source "../tests/includes/init-tests.tcl"

proc only_replica_eligible {sid port} {
    set eligible {}
    foreach replica [S $sid SENTINEL REPLICAS mymaster] {
        if {[dict get $replica slave-priority] != 0} {
            lappend eligible [dict get $replica port]
        }
    }
    expr {$eligible eq [list $port]}
}

proc pending_reconfig_has {sid expected} {
    foreach script [S $sid SENTINEL PENDING-SCRIPTS] {
        if {[lrange [dict get $script argv] 1 end] eq $expected} {
            return 1
        }
    }
    return 0
}

proc restore_fixture {replica_ids} {
    foreach id $replica_ids {
        catch {R $id ACL SETUSER default +replicaof +slaveof}
        catch {R $id CONFIG SET replica-priority 100}
    }
    foreach_sentinel_id id {
        catch {S $id SENTINEL DEBUG publish-period 2000 info-period 10000 slave-reconf-timeout 10000}
    }
}

test "Newer same-address HELLO resets a promoted failover" {
    set sid 0
    set primary [S $sid SENTINEL PRIMARY mymaster]
    set original [list [dict get $primary ip] [dict get $primary port]]
    set replica_ids {}
    foreach_valkey_id id {
        if {$id != $master_id} { lappend replica_ids $id }
    }
    set promoted_port [RPort [lindex $replica_ids 0]]

    foreach id [lrange $replica_ids 1 end] {
        # Keep these replicas reachable, but unable to finish reconfiguration.
        R $id CONFIG SET replica-priority 0
        R $id ACL SETUSER default -replicaof -slaveof
    }

    if {[catch {
        # Stop observer Sentinels from converting the promoted node back early.
        foreach_sentinel_id id {
            if {$id != $sid} { S $id SENTINEL DEBUG publish-period 60000 }
        }
        S $sid SENTINEL DEBUG info-period 1000 slave-reconf-timeout 60000
        S $sid SENTINEL SET mymaster failover-timeout 60000
        S $sid SENTINEL SET mymaster client-reconfig-script [auto_execok false]

        wait_for_condition 200 50 {
            [only_replica_eligible $sid $promoted_port]
        } else {
            fail "Sentinel did not observe replica priorities"
        }
        wait_for_condition 300 50 {
            [catch {S $sid SENTINEL FAILOVER mymaster}] == 0
        } else {
            fail "Manual failover did not start"
        }
        wait_for_condition 1000 50 {
            [dict get [S $sid SENTINEL PRIMARY mymaster] failover-state] eq {reconf_slaves}
        } else {
            fail "Failover did not reach reconf_slaves"
        }

        set primary [S $sid SENTINEL PRIMARY mymaster]
        set reported [S $sid SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]
        assert_equal [lindex $original 1] [dict get $primary port]
        assert_not_equal [lindex $original 1] [lindex $reported 1]
        assert {[pending_reconfig_has $sid [concat mymaster leader start $original $reported]]}

        set epoch [expr {[dict get $primary config-epoch] + 1}]
        set other [lindex [S $sid SENTINEL SENTINELS mymaster] 0]
        set hello "[dict get $other ip],[dict get $other port],[dict get $other runid],0,mymaster,[join $original ,],$epoch"
        S $sid PUBLISH __sentinel__:hello $hello

        set primary [S $sid SENTINEL PRIMARY mymaster]
        assert_equal $original [S $sid SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]
        assert_equal $epoch [dict get $primary config-epoch]
        assert_no_match {*failover_in_progress*} [dict get $primary flags]
        assert {[pending_reconfig_has $sid [concat mymaster observer start $reported $original]]}
    } err]} {
        restore_fixture $replica_ids
        error $err $::errorInfo
    }
    restore_fixture $replica_ids
}

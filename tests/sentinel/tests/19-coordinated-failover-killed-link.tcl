# Regression test for coordinated failover when the Sentinel command link to
# the old primary is disconnected just as the promoted replica reports its new
# role.  In that window Sentinel must not dereference a NULL command link while
# doing its best-effort client cleanup on the old primary.

source "../tests/includes/init-tests.tcl"

foreach_sentinel_id id {
    S $id sentinel debug info-period 1000
    S $id sentinel debug publish-period 100
    S $id sentinel debug ping-period 100
}

proc sentinel_cmd_client_name {sentinel_id} {
    set myid [S $sentinel_id SENTINEL MYID]
    return "sentinel-[string range $myid 0 7]-cmd"
}

test "Coordinated failover tolerates old primary command link disconnect" {
    set sentinel_id 0
    set old_master_id $master_id
    set old_port [RPort $old_master_id]
    set old_addr [S $sentinel_id SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]
    assert {[lindex $old_addr 1] == $old_port}

    set sentinel_client_name [sentinel_cmd_client_name $sentinel_id]

    wait_for_condition 1000 50 {
        [string match "*name=$sentinel_client_name*" [R $old_master_id CLIENT LIST]]
    } else {
        fail "Sentinel command client $sentinel_client_name was not connected to the old primary"
    }

    wait_for_condition 300 50 {
        [catch {S $sentinel_id SENTINEL FAILOVER mymaster COORDINATED}] == 0
    } else {
        catch {S $sentinel_id SENTINEL FAILOVER mymaster COORDINATED} reply
        fail "Sentinel manual coordinated failover did not start, got: $reply"
    }

    wait_for_condition 1000 10 {
        [dict get [S $sentinel_id SENTINEL PRIMARY mymaster] failover-state] eq {wait_promotion}
    } else {
        fail "Sentinel did not reach wait_promotion state"
    }

    # Keep the command connection from this Sentinel to the old primary closed
    # while the next INFO reply from the promoted replica advances the failover.
    # Before the fix, sentinelKillClients(old-primary) calls
    # valkeyAsyncCommand(NULL, ...) in this window, and the Sentinel crashes.
    S $sentinel_id sentinel debug ping-period 10000
    catch {R $old_master_id CLIENT KILL NAME $sentinel_client_name}

    wait_for_condition 1000 10 {
        [string match "*disconnected*" [dict get [S $sentinel_id SENTINEL PRIMARY mymaster] flags]]
    } else {
        fail "Sentinel command link to the old primary did not disconnect"
    }

    assert_equal {PONG} [S $sentinel_id PING]

    wait_for_condition 1000 50 {
        [lindex [S $sentinel_id SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster] 1] != $old_port
    } else {
        fail "Sentinel did not complete the coordinated failover"
    }
}

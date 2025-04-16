# Test manual safe failover

source "../tests/includes/init-tests.tcl"

foreach_sentinel_id id {
    S $id sentinel debug info-period 2000
    S $id sentinel debug default-down-after 6000
    S $id sentinel debug publish-period 1000
}

set loop_counter 0

test "Manual safe failover works" {
    set old_port [RPort $master_id]
    set addr [S 0 SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]
    assert {[lindex $addr 1] == $old_port}

    # Enable the repl-read-only configuration of the primary node.
    R $master_id config set replica-read-only yes

    R $master_id set counter 0

    # Perform a safe failover.
    catch {S 0 SENTINEL FAILOVER mymaster safe} reply
    assert {$reply eq "OK"}

    while {1} {
        catch {R $master_id incr counter} reply
        if {[string match "*READONLY*" $reply]} {
            break
        }
        incr loop_counter
    }

    set old_primary_counter [R $master_id get counter]
    assert {$old_primary_counter == $loop_counter}

    # Wait for all Sentinel nodes to update the primary node information.
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [lindex [S $id SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster] 1] != $old_port
        } else {
            fail "At least one Sentinel did not receive failover info"
        }
    }

    set addr [S 0 SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]
    set master_id [get_instance_id_by_port valkey [lindex $addr 1]]
}

test "safe failover: Check data consistency" {
    set primary_counter [R $master_id get counter]
    foreach_valkey_id id {
        if {$id != $master_id} {
            set replica_counter [R $id get counter]
            assert {$replica_counter == $primary_counter}
        }
    }
}

test "safe failover: New primary [join $addr {:}] role matches" {
    assert {[RI $master_id role] eq {master}}
}

test "safe failover: All the other replicas now point to the new primary" {
    foreach_valkey_id id {
        if {$id != $master_id && $id != 0} {
            wait_for_condition 1000 50 {
                [RI $id master_port] == [lindex $addr 1]
            } else {
                fail "Valkey ID $id not configured to replicate with new primary"
            }
        }
    }
}

test "safe failover: The old primary eventually gets reconfigured as a replica" {
    wait_for_condition 1000 50 {
        [RI 0 master_port] == [lindex $addr 1]
    } else {
        fail "Old primary not reconfigured as replica of new primary"
    }
}

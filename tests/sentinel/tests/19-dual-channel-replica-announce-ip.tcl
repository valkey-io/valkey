# Test dual-channel replication with replica-announce-ip
# This test verifies that when dual-channel replication is enabled,
# the rdb/aof channel correctly announces its IP address to Sentinel
# using the replica-announce-ip configuration.

source "../tests/includes/utils.tcl"

# Configure replica-announce-ip before initialization
test "(pre-init) Configure replica-announce-ip" {
    restart_killed_instances

    # Set replica-announce-ip on all replicas (non-primary instances)
    # We'll use 127.0.0.1 as the announced IP (must be reachable for Sentinel)
    set ::announced_ip "127.0.0.1"
    for {set id 1} {$id < $::instances_count} {incr id} {
        R $id config set replica-announce-ip $::announced_ip
    }
}

source "../tests/includes/init-tests.tcl"

# Enable dual-channel replication after initialization is complete
test "Enable dual-channel replication and verify replica-announce-ip" {
    # Enable dual-channel replication on all instances
    foreach_valkey_id id {
        R $id config set dual-channel-replication-enabled yes
        R $id config set repl-diskless-sync yes
    }

    # Give Sentinel time to update its view of replicas
    after 1000
}

proc verify_replica_announced_ip {expected_ip} {
    foreach_sentinel_id id {
        # Check that replicas are reported with the announced IP
        set replicas [S $id SENTINEL REPLICAS mymaster]
        foreach replica $replicas {
            set replica_ip [dict get $replica ip]
            if {$replica_ip ne $expected_ip} {
                return 0
            }
        }
    }
    return 1
}

test "Sentinel reports replicas with announced IP in dual-channel replication" {
    # Wait for replicas to sync with the primary
    wait_for_condition 1000 50 {
        [verify_replica_announced_ip $::announced_ip]
    } else {
        fail "Sentinel did not report replicas with the announced IP ($::announced_ip)"
    }
}

test "Verify replica is actually connected and syncing" {
    # Verify that the replica is actually online and replicating
    # by setting a value on the primary and checking it propagates
    set test_key "test_key_for_dual_channel"
    set test_value "test_value_[randomInt 1000000]"

    R $master_id set $test_key $test_value

    # Wait for the value to propagate to all replicas
    for {set replica_id 1} {$replica_id < $::instances_count} {incr replica_id} {
        wait_for_condition 1000 50 {
            [R $replica_id get $test_key] eq $test_value
        } else {
            fail "Value did not propagate to replica $replica_id"
        }
    }
}

test "Sentinel failover works with dual-channel replication and announced IP" {
    # Verify that failover still works correctly with dual-channel replication
    set old_primary_id $master_id
    set old_primary_addr [S 0 SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]

    # Kill the primary
    kill_instance valkey $old_primary_id

    # Wait for Sentinel to detect the failure and perform failover
    wait_for_condition 10000 50 {
        [lindex [S 0 SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster] 1] != [lindex $old_primary_addr 1]
    } else {
        fail "Sentinel did not perform failover"
    }

    # Verify new primary is elected
    set new_primary_addr [S 0 SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster]
    assert {$new_primary_addr ne $old_primary_addr}

    # Restart the old primary
    restart_instance valkey $old_primary_id

    # Wait for the old primary to become a replica
    wait_for_condition 1000 50 {
        [RI $old_primary_id role] eq {slave}
    } else {
        fail "Old primary did not become a replica"
    }
}

# Cleanup: revert any special configuration
test "(post-cleanup) Reset dual-channel replication and replica-announce-ip" {
    foreach_valkey_id id {
        catch {R $id config set dual-channel-replication-enabled no}
        catch {R $id config set replica-announce-ip ""}
        catch {R $id config set repl-diskless-sync no}
    }
}

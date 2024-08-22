# Initialization tests -- most units will start including this.
source "../tests/includes/utils.tcl"

test "(init) Restart killed instances" {
    restart_killed_instances
}

test "(init) Remove old primary entry from sentinels" {
    foreach_sentinel_id id {
        catch {S $id SENTINEL REMOVE mymaster}
    }
}

set redis_slaves [expr $::instances_count - 1]
test "(init) Create a primary-replicas cluster of [expr $redis_slaves+1] instances" {
    create_valkey_master_slave_cluster [expr {$redis_slaves+1}]
}
set master_id 0

test "(init) Sentinels can start monitoring a primary" {
    set sentinels [llength $::sentinel_instances]
    set quorum [expr {$sentinels/2+1}]
    foreach_sentinel_id id {
        S $id SENTINEL MONITOR mymaster \
              [get_instance_attrib valkey $master_id host] \
              [get_instance_attrib valkey $master_id port] $quorum
    }
    foreach_sentinel_id id {
        assert {[S $id sentinel primary mymaster] ne {}}
        S $id SENTINEL SET mymaster down-after-milliseconds 2000
        S $id SENTINEL SET mymaster failover-timeout 10000
        S $id SENTINEL debug tilt-period 5000
        S $id SENTINEL SET mymaster parallel-syncs 10
        if {$::leaked_fds_file != "" && [exec uname] == "Linux"} {
            S $id SENTINEL SET mymaster notification-script ../../tests/helpers/check_leaked_fds.tcl
            S $id SENTINEL SET mymaster client-reconfig-script ../../tests/helpers/check_leaked_fds.tcl
        }
    }
}

test "(init) Sentinels can talk with the primary" {
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [catch {S $id SENTINEL GET-PRIMARY-ADDR-BY-NAME mymaster}] == 0
        } else {
            fail "Sentinel $id can't talk with the primary."
        }
    }
}

test "(init) Sentinels are able to auto-discover other sentinels" {
    verify_sentinel_auto_discovery
}

test "(init) Sentinels are able to auto-discover replicas" {
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [dict get [S $id SENTINEL PRIMARY mymaster] num-slaves] == $redis_slaves
        } else {
            fail "At least some sentinels can't detect some replicas"
        }
    }
}

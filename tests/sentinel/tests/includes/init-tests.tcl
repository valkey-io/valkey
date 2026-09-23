# Initialization tests -- most units will start including this.
source "../tests/includes/utils.tcl"

proc sentinel_monitor_primary { primary_name primary_id } {
    set sentinels [llength $::sentinel_instances]
    set quorum [expr {$sentinels/2+1}]
    foreach_sentinel_id id {
        S $id SENTINEL MONITOR $primary_name \
          [get_instance_attrib valkey $primary_id host] [get_instance_attrib valkey $primary_id port] $quorum
    }
    foreach_sentinel_id id {
        assert {[S $id sentinel primary $primary_name] ne {}}
        S $id SENTINEL SET $primary_name down-after-milliseconds 2000
        S $id SENTINEL SET $primary_name failover-timeout 10000
        S $id SENTINEL DEBUG tilt-period 5000
        S $id SENTINEL SET $primary_name parallel-syncs 10
        if {$::leaked_fds_file != "" && [exec uname] == "Linux"} {
            S $id SENTINEL SET $primary_name notification-script ../../tests/helpers/check_leaked_fds.tcl
            S $id SENTINEL SET $primary_name client-reconfig-script ../../tests/helpers/check_leaked_fds.tcl
        }
    }
}

proc verify_sentinel_connect_to_primary { primary_name } {
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [catch {S $id SENTINEL GET-PRIMARY-ADDR-BY-NAME $primary_name}] == 0
        } else {
            fail "Sentinel $id can't talk with the primary $primary_name"
        }
    }
}

proc verify_sentinel_discover_replicas { primary_name expected_replicas } {
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [dict get [S $id SENTINEL primary $primary_name] num-slaves] == $expected_replicas
        } else {
            fail "For primary $primary_name, at least some sentinel can't detect some replicas"
        }
    }
}

proc init_cluster { primary_name primary_id num_instances } {
    puts "Initializing test setup for cluster: primary $primary_name with $num_instances instances"

    test "(init) Restart killed instances" {
        restart_killed_instances
    }

    test "(init) Remove old primary entry from sentinels" {
        foreach_sentinel_id id {
            catch {S $id SENTINEL REMOVE $primary_name}
        }
    }

    test "(init) Create a primary-replicas cluster of $num_instances instances" {
        create_valkey_primary_replica_cluster $num_instances $primary_id
    }

    test "(init) Sentinels can start monitoring a primary" {
        sentinel_monitor_primary $primary_name $primary_id
    }

    test "(init) Sentinels can talk with the primary" {
        verify_sentinel_connect_to_primary $primary_name
    }

    test "(init) Sentinels are able to auto-discover other sentinels" {
        verify_sentinel_auto_discovery $primary_name
    }

    test "(init) Sentinels are able to auto-discover replicas" {
        verify_sentinel_discover_replicas $primary_name [expr $num_instances - 1]
    }
}

set master_id 0
init_cluster "mymaster" $master_id $::instances_count
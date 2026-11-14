# Test that Sentinel does not send CONFIG REWRITE to instances started
# without a configuration file: the command is guaranteed to fail and
# would get recorded in the instance error statistics on every failover
# or replica reconfiguration.

source "../tests/includes/init-tests.tcl"

foreach_sentinel_id id {
    S $id sentinel debug info-period 2000
    S $id sentinel debug default-down-after 6000
    S $id sentinel debug publish-period 1000
}

# Spawn a Valkey instance passing the configuration on the command line,
# so that the server runs without a config file (the config_file field in
# the INFO output is empty). The instance is registered in the instances
# list like spawn_instance does, so it can be driven with R/RI and gets
# killed at cleanup.
proc spawn_valkey_no_config_file {} {
    set instance_id [llength $::valkey_instances]
    set dirname "valkey_${instance_id}"
    lappend ::dirs $dirname
    catch {exec rm -rf $dirname}
    file mkdir $dirname

    set port [find_available_port $::valkey_base_port $::valkey_port_count]
    set pport 0
    set args [list \
        --repl-diskless-sync-delay 0 \
        --dir ./$dirname \
        --logfile log.txt \
        --enable-debug-assert yes \
        --enable-protected-configs yes \
        --enable-debug-command yes \
        --save ""]
    if {$::tls} {
        set pport [find_available_port $::valkey_base_port $::valkey_port_count]
        lappend args \
            --tls-port $port \
            --port $pport \
            --tls-replication yes \
            --tls-cluster yes \
            --tls-cert-file [format "%s/../../tls/server.crt" [pwd]] \
            --tls-key-file [format "%s/../../tls/server.key" [pwd]] \
            --tls-client-cert-file [format "%s/../../tls/client.crt" [pwd]] \
            --tls-client-key-file [format "%s/../../tls/client.key" [pwd]] \
            --tls-dh-params-file [format "%s/../../tls/valkey.dh" [pwd]] \
            --tls-ca-cert-file [format "%s/../../tls/ca.crt" [pwd]]
    } else {
        lappend args --port $port
    }

    set errfile [file join $dirname err.txt]
    set pid [exec $::VALKEY_SERVER_BIN {*}$args 2>> $errfile &]

    # Check availability
    if {[server_is_up $::host $port 100] == 0} {
        set logfile [file join $dirname log.txt]
        catch {puts [exec tail $logfile]}
        abort_sentinel_test "Problem starting valkey #$instance_id: ping timeout, maybe server start failed, check $logfile"
    }
    puts "Starting valkey #$instance_id at port $port without a config file"
    lappend ::pids $pid

    # Push the instance into the valkey instances list.
    set link [valkey $::host $port 0 $::tls]
    $link reconnect 1
    lappend ::valkey_instances [list \
        instance_id $instance_id \
        pid $pid \
        host $::host \
        port $port \
        plaintext-port $pport \
        link $link \
    ]
    return $instance_id
}

# Return true when Sentinel 0 reports the expected number of replicas for
# the primary, all with the link up and none flagged as down.
proc sentinel_replicas_link_up {primary_name expected_replicas} {
    set replicas [S 0 SENTINEL REPLICAS $primary_name]
    if {[llength $replicas] != $expected_replicas} {
        return 0
    }
    foreach replica $replicas {
        set flags [dict get $replica flags]
        if {[string match "*down*" $flags] || [string match "*disconnected*" $flags]} {
            return 0
        }
    }
    return 1
}

test "Sentinel skips CONFIG REWRITE for instances started without a config file" {
    # Two instances without a config file, plus a regular instance (with
    # a config file) used as a control to verify CONFIG REWRITE is still
    # sent when the instance was started with a configuration file.
    set nocfg_ids [list [spawn_valkey_no_config_file] [spawn_valkey_no_config_file]]
    spawn_instance valkey $::valkey_base_port 1
    set cfg_id [expr [llength $::valkey_instances] - 1]

    foreach id $nocfg_ids {
        assert_equal "" [RI $id config_file]
    }
    assert_not_equal "" [RI $cfg_id config_file]

    set primary_id [lindex $nocfg_ids 0]
    set primary_host [get_instance_attrib valkey $primary_id host]
    set primary_port [get_instance_attrib valkey $primary_id port]
    foreach id [list [lindex $nocfg_ids 1] $cfg_id] {
        R $id replicaof $primary_host $primary_port
    }
    wait_for_condition 1000 50 {
        [RI [lindex $nocfg_ids 1] master_link_status] eq "up" &&
        [RI $cfg_id master_link_status] eq "up"
    } else {
        fail "Unable to create a primary-replica cluster on instances without a config file"
    }

    foreach_sentinel_id id {
        S $id SENTINEL MONITOR nocfg $primary_host $primary_port 1
        S $id SENTINEL SET nocfg down-after-milliseconds 5000
        S $id SENTINEL SET nocfg failover-timeout 30000
        S $id SENTINEL SET nocfg parallel-syncs 10
    }

    # Wait for the sentinels to discover the replicas, so that the cached
    # INFO output (reporting an empty config_file field) is available for
    # all the instances before the failover.
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [dict get [S $id SENTINEL primary nocfg] num-slaves] == 2
        } else {
            fail "Sentinel $id can't detect the replicas of the no-config primary"
        }
    }

    # Wait for the replicas to be good failover candidates (link up, not
    # flagged as down) in the leader sentinel view.
    wait_for_condition 1000 50 {
        [sentinel_replicas_link_up nocfg 2]
    } else {
        fail "Sentinel 0 doesn't see the replicas of the no-config primary as up"
    }

    # Trigger a manual failover, retrying it every 10 seconds until the
    # primary switches (a stalled failover is aborted by the sentinel at
    # failover-timeout and then restarted here).
    set switched 0
    for {set i 0} {$i < 1200 && !$switched} {incr i} {
        if {$i % 200 == 0} {
            catch {S 0 SENTINEL FAILOVER nocfg}
        }
        if {[lindex [S 0 SENTINEL GET-PRIMARY-ADDR-BY-NAME nocfg] 1] != $primary_port} {
            set switched 1
        }
        after 50
    }
    if {!$switched} {
        fail "Sentinel did not switch the primary"
    }

    # Wait for the old primary to be converted into a replica of the new
    # one, so that every monitored instance went through a reconfiguration.
    wait_for_condition 1000 50 {
        [RI $primary_id role] eq "slave"
    } else {
        fail "The old primary was not reconfigured as a replica"
    }

    # The instance started with a config file must have rewritten it: the
    # success is logged by the server (the reply is discarded by Sentinel).
    wait_for_condition 1000 50 {
        [string match "*CONFIG REWRITE executed with success*" \
            [exec cat [file join valkey_$cfg_id log.txt]]]
    } else {
        fail "Instance with a config file did not execute CONFIG REWRITE"
    }

    # Wait a couple of INFO periods for late reconfigurations, then check
    # that no failed command was recorded by the instances started without
    # a config file.
    after 4000
    foreach id $nocfg_ids {
        assert_no_match "*errorstat_*" [R $id INFO errorstats]
    }

    foreach_sentinel_id id {
        catch {S $id SENTINEL REMOVE nocfg}
    }
    remove_valkey_instance [concat $nocfg_ids $cfg_id]
}

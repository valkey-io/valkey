# Sentinel must not send CONFIG REWRITE to instances started without a
# config file, it can only fail there and pollutes their error stats.

proc start_server_without_config {port dir args} {
    file mkdir $dir
    set pid [exec $::VALKEY_SERVER_BIN --port $port --bind 127.0.0.1 --dir $dir \
        --save "" --logfile [file join $dir log.txt] {*}$args &]
    lappend ::pids $pid
    wait_for_condition 50 100 {
        [catch {set r [valkey 127.0.0.1 $port 0 0]; $r ping; $r close}] == 0
    } else {
        fail "Server without config file did not start on port $port"
    }
    return $pid
}

# Sentinel kills normal clients when it reconfigures an instance, so use a
# new connection for every call.
proc nocfg_call {port args} {
    set r [valkey 127.0.0.1 $port 0 0]
    set res [$r {*}$args]
    $r close
    return $res
}

if {!$::tls} {
    test "Sentinel does not send CONFIG REWRITE to instances without a config file" {
        set primary_port [find_available_port $::valkey_base_port $::valkey_port_count]
        set replica_port [find_available_port $::valkey_base_port $::valkey_port_count]
        set primary_pid [start_server_without_config $primary_port [file normalize "nocfg_primary"]]
        set replica_pid [start_server_without_config $replica_port [file normalize "nocfg_replica"] \
            --replicaof 127.0.0.1 $primary_port]
        assert_match "*config_file:\r\n*" [nocfg_call $primary_port info server]

        S 0 SENTINEL MONITOR nocfg 127.0.0.1 $primary_port 1
        S 0 SENTINEL SET nocfg down-after-milliseconds 2000
        S 0 SENTINEL SET nocfg failover-timeout 10000

        wait_for_condition 200 100 {
            [llength [S 0 SENTINEL REPLICAS nocfg]] == 1
        } else {
            fail "Sentinel did not discover the replica"
        }

        nocfg_call $primary_port config resetstat
        nocfg_call $replica_port config resetstat
        wait_for_condition 100 100 {
            [catch {S 0 SENTINEL FAILOVER nocfg}] == 0
        } else {
            fail "Sentinel did not accept the failover"
        }

        wait_for_condition 200 100 {
            [lindex [S 0 SENTINEL GET-PRIMARY-ADDR-BY-NAME nocfg] 1] == $replica_port &&
            [string match "*role:slave*" [nocfg_call $primary_port info replication]]
        } else {
            fail "Failover did not complete"
        }

        foreach port [list $primary_port $replica_port] {
            assert_no_match "*cmdstat_config|rewrite*" [nocfg_call $port info commandstats]
            assert_no_match "*errorstat_ERR*" [nocfg_call $port info errorstats]
        }

        S 0 SENTINEL REMOVE nocfg
        stop_instance $primary_pid
        stop_instance $replica_pid
    }
}

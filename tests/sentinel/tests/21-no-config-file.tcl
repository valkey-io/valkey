# Sentinel must not send CONFIG REWRITE to instances started without a
# config file, it can only fail there and pollutes their error stats.
# Instances started with a config file must still get it.

proc start_plain_server {port dir with_config args} {
    file mkdir $dir
    set cmd [list $::VALKEY_SERVER_BIN]
    if {$with_config} {
        set conf [file join $dir valkey.conf]
        close [open $conf w]
        lappend cmd $conf
    }
    set pid [exec {*}$cmd --port $port --bind 127.0.0.1 --dir $dir \
        --save "" --logfile [file join $dir log.txt] {*}$args &]
    lappend ::pids $pid
    test_defer [list stop_instance $pid]
    wait_for_condition 50 100 {
        [catch {nocfg_call $port ping}] == 0
    } else {
        fail "Server on port $port did not start"
    }
    return $pid
}

# Sentinel kills normal clients when it reconfigures an instance, so use a
# new connection for every call.
proc nocfg_call {port args} {
    set r [valkey 127.0.0.1 $port 0 0]
    try {
        return [$r {*}$args]
    } finally {
        catch {$r close}
    }
}

# A plain failover reconfigures the instances with sentinelSendReplicaOf, a
# coordinated one with sentinelKillClients. Both used to send CONFIG REWRITE
# without checking for a config file.
foreach with_config {0 1} {
    foreach mode {{} COORDINATED} {
        if {$::tls} break

        if {$with_config} {
            set descr "Sentinel sends CONFIG REWRITE to instances with a config file"
        } else {
            set descr "Sentinel does not send CONFIG REWRITE to instances without a config file"
        }
        test "$descr ([string trim "$mode failover"])" {
            set name "[expr {$with_config ? "cfg" : "nocfg"}][string tolower $mode]"
            set primary_port [find_available_port $::valkey_base_port $::valkey_port_count]
            set replica_port [find_available_port $::valkey_base_port $::valkey_port_count]
            start_plain_server $primary_port [file normalize "${name}_primary"] $with_config
            start_plain_server $replica_port [file normalize "${name}_replica"] $with_config \
                --replicaof 127.0.0.1 $primary_port
            if {$with_config} {
                assert_match "*config_file:/*" [nocfg_call $primary_port info server]
            } else {
                assert_match "*config_file:\r\n*" [nocfg_call $primary_port info server]
            }
            wait_for_condition 100 100 {
                [string match "*master_link_status:up*" [nocfg_call $replica_port info replication]]
            } else {
                fail "Replica did not sync with the primary"
            }

            S 0 SENTINEL MONITOR $name 127.0.0.1 $primary_port 1
            test_defer [list S 0 SENTINEL REMOVE $name]
            S 0 SENTINEL SET $name down-after-milliseconds 2000
            S 0 SENTINEL SET $name failover-timeout 10000

            wait_for_condition 200 100 {
                [llength [S 0 SENTINEL REPLICAS $name]] == 1
            } else {
                fail "Sentinel did not discover the replica"
            }

            nocfg_call $primary_port config resetstat
            nocfg_call $replica_port config resetstat
            wait_for_condition 100 100 {
                [catch {S 0 SENTINEL FAILOVER $name {*}$mode}] == 0
            } else {
                fail "Sentinel did not accept the failover"
            }

            wait_for_condition 200 100 {
                [lindex [S 0 SENTINEL GET-PRIMARY-ADDR-BY-NAME $name] 1] == $replica_port &&
                [catch {nocfg_call $primary_port info replication} info] == 0 &&
                [string match "*role:slave*" $info]
            } else {
                fail "Failover did not complete"
            }

            # In a coordinated failover the rewrite would run in the same queued
            # transaction as CLIENT UNPAUSE, which can finish after the role change.
            if {$mode eq "COORDINATED"} {
                foreach port [list $primary_port $replica_port] {
                    wait_for_condition 100 100 {
                        [catch {nocfg_call $port info commandstats} stats] == 0 &&
                        [string match "*cmdstat_client|unpause*" $stats]
                    } else {
                        fail "Sentinel did not finish reconfiguring the instance on port $port"
                    }
                }
            }

            foreach port [list $primary_port $replica_port] {
                if {$with_config} {
                    assert_match "*cmdstat_config|rewrite*" [nocfg_call $port info commandstats]
                } else {
                    assert_no_match "*cmdstat_config|rewrite*" [nocfg_call $port info commandstats]
                }
                assert_no_match "*errorstat_ERR*" [nocfg_call $port info errorstats]
            }
        }
    }
}

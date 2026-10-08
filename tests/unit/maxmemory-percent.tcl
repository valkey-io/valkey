proc expected_maxmemory_percent {percent} {
    set limit [s total_system_memory]
    set cgroup_limit [s cgroup_memory_limit]
    if {$cgroup_limit != 0} {
        set limit $cgroup_limit
    }
    return [expr {$limit * $percent / 100}]
}

start_server {tags {"maxmemory external:skip"} overrides {maxmemory 75%}} {
    test {Percentage maxmemory is resolved at startup and reported in bytes} {
        assert_equal {maxmemory 75%} [r config get maxmemory]
        assert_equal [expected_maxmemory_percent 75] [s maxmemory]
    }

    test {Percentage maxmemory preserves CONFIG INFO numeric metadata} {
        set info [lindex [r config info maxmemory] 0]
        assert_equal numeric [dict get $info type]
        assert_equal {0 18446744073709551615} [dict get $info range]
    }

    test {CONFIG SET maxmemory accepts integer percentages} {
        foreach percent {1 50 75 100} {
            assert_equal OK [r config set maxmemory $percent%]
            assert_equal [list maxmemory $percent%] [r config get maxmemory]
            assert_equal [expected_maxmemory_percent $percent] [s maxmemory]
        }
    }

    test {CONFIG SET maxmemory rejects invalid percentages without changing the limit} {
        r config set maxmemory 75%
        set before [s maxmemory]
        foreach invalid {0% -1% 101% 1.5% abc% % 75%% 9223372036854775808%} {
            assert_error {*percentage argument must be between 1 and 100*} {
                r config set maxmemory $invalid
            }
            assert_equal {maxmemory 75%} [r config get maxmemory]
            assert_equal $before [s maxmemory]
        }
    }

    test {Percentage maxmemory rolls back on a CONFIG SET validation error} {
        set before [s maxmemory]
        assert_error {*CONFIG SET failed*} {
            r config set maxmemory 20mb maxmemory-clients 101%
        }
        assert_equal {maxmemory 75%} [r config get maxmemory]
        assert_equal $before [s maxmemory]

        r config set maxmemory 20mb
        assert_error {*CONFIG SET failed*} {
            r config set maxmemory 50% maxmemory-clients 101%
        }
        assert_equal {maxmemory 20971520} [r config get maxmemory]
        assert_equal 20971520 [s maxmemory]
    }

    test {Percentage maxmemory rolls back on a CONFIG SET apply error} {
        proc maxmemory_dummy_accept {chan addr port} { close $chan }
        set listener [socket -server maxmemory_dummy_accept -myaddr 127.0.0.1 0]
        set used_port [lindex [fconfigure $listener -sockname] 2]
        foreach initial {20mb 75%} {
            r config set maxmemory $initial
            set before [s maxmemory]
            set config_before [r config get maxmemory]
            assert_error {*Unable to listen on this port*} {
                r config set maxmemory 50% port $used_port
            }
            assert_equal $config_before [r config get maxmemory]
            assert_equal $before [s maxmemory]
        }
        close $listener
    }

    test {CONFIG REWRITE preserves percentage maxmemory across restart} {
        r config set maxmemory 75%
        r config rewrite
        restart_server 0 true false
        assert_equal {maxmemory 75%} [r config get maxmemory]
        assert_equal [expected_maxmemory_percent 75] [s maxmemory]
    }

    test {Switching from percentage maxmemory preserves unsigned byte values and unlimited mode} {
        foreach bytes {9223372036854775808 18446744073709551615 0} {
            r config set maxmemory 75%
            r config set maxmemory $bytes
            r config rewrite
            restart_server 0 true false
            assert_equal [list maxmemory $bytes] [r config get maxmemory]
        }
        assert_equal 0 [s maxmemory]
    }
}

foreach invalid {0% -1% 101% 1.5%} {
    test "Startup rejects invalid maxmemory percentage $invalid" {
        set config [tmpfile maxmemory-percent.conf]
        set fd [open $config w]
        puts $fd "maxmemory $invalid"
        close $fd
        catch {exec $::VALKEY_SERVER_BIN $config 2>@1} error
        assert_match {*percentage argument must be between 1 and 100*} $error
    }
}

start_server {tags {"repl downgrade"}} {
    set replica [srv 0 client]
    start_server {tags {"repl downgrade"}} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        test {Configure the Redis 6.0 downgrade target as a replica} {
            $replica replicaof $master_host $master_port
            wait_for_condition 50 100 {
                [string match {*master_link_status:up*} [$replica info replication]]
            } else {
                fail "Replication did not start"
            }
            $replica config resetstat
        }

        foreach {option clock_unit} {
            EXAT seconds
            PXAT milliseconds
        } {
            test "Replica applies Valkey-compatible SET $option propagation" {
                set key "set-[string tolower $option]"
                set value "value-$option"
                set expire_at [expr {[clock $clock_unit] + ($clock_unit eq "seconds" ? 100 : 100000)}]

                assert_equal OK [$master set $key $value $option $expire_at]
                assert_equal 1 [$master wait 1 5000]
                assert_equal $value [$replica get $key]

                set master_ttl [$master pttl $key]
                set replica_ttl [$replica pttl $key]
                assert_range $master_ttl 90000 100000
                assert_range $replica_ttl 90000 100000
                assert {abs($master_ttl - $replica_ttl) <= 2000}
                assert_equal up [s -1 master_link_status]
                assert_equal 0 [s -1 unexpected_error_replies]
            }
        }
    }
}

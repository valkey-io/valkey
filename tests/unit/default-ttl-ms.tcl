start_server {tags {expire}} {
    test {default-ttl-ms is disabled by default} {
        assert_equal {default-ttl-ms 0} [r config get default-ttl-ms]
        r set default-ttl-ms:disabled value
        assert_equal -1 [r pttl default-ttl-ms:disabled]
    }

    test {default-ttl-ms applies only to later SET writes} {
        r set default-ttl-ms:old value
        r config set default-ttl-ms 10000
        assert_equal -1 [r pttl default-ttl-ms:old]
        r set default-ttl-ms:new value
        set ttl [r pttl default-ttl-ms:new]
        assert {$ttl > 9000 && $ttl <= 10000}
    }

    test {default-ttl-ms applies to successful SET NX GET} {
        assert_equal {} [r set default-ttl-ms:nx-get value NX GET]
        assert_equal value [r get default-ttl-ms:nx-get]
        set ttl [r pttl default-ttl-ms:nx-get]
        assert {$ttl > 9000 && $ttl <= 10000}
    }

    test {default-ttl-ms rejects negatives without changing its value} {
        assert_error {*argument must be between 0 and*} {
            r config set default-ttl-ms -1
        }
        assert_equal {default-ttl-ms 10000} [r config get default-ttl-ms]
    }

    test {default-ttl-ms accepts LLONG_MAX/2 and rejects the next value} {
        assert_equal OK [r config set default-ttl-ms 4611686018427387903]
        assert_error {*argument must be between 0 and 4611686018427387903 inclusive*} {
            r config set default-ttl-ms 4611686018427387904
        }
        assert_equal {default-ttl-ms 4611686018427387903} [r config get default-ttl-ms]
        r config set default-ttl-ms 10000
    }

    test {explicit TTL and KEEPTTL take precedence over default-ttl-ms} {
        r set default-ttl-ms:explicit value PX 30000
        set ttl [r pttl default-ttl-ms:explicit]
        assert {$ttl > 29000 && $ttl <= 30000}

        set before [r pexpiretime default-ttl-ms:explicit]
        r set default-ttl-ms:explicit replacement KEEPTTL
        assert_equal $before [r pexpiretime default-ttl-ms:explicit]
    }

    test {default-ttl-ms applies when LPUSH and RPUSH create lists} {
        assert_equal 2 [r lpush default-ttl-ms:lpush a b]
        set lpush_ttl [r pttl default-ttl-ms:lpush]
        assert {$lpush_ttl > 9000 && $lpush_ttl <= 10000}

        assert_equal 2 [r rpush default-ttl-ms:rpush a b]
        set rpush_ttl [r pttl default-ttl-ms:rpush]
        assert {$rpush_ttl > 9000 && $rpush_ttl <= 10000}
    }

    test {LPUSH and RPUSH preserve persistent existing lists} {
        r config set default-ttl-ms 0
        r lpush default-ttl-ms:lpush-persistent seed
        r rpush default-ttl-ms:rpush-persistent seed
        r config set default-ttl-ms 10000

        r lpush default-ttl-ms:lpush-persistent value
        r rpush default-ttl-ms:rpush-persistent value
        assert_equal -1 [r pttl default-ttl-ms:lpush-persistent]
        assert_equal -1 [r pttl default-ttl-ms:rpush-persistent]
    }

    test {LPUSH and RPUSH preserve existing absolute deadlines} {
        r lpush default-ttl-ms:lpush-expiring seed
        r rpush default-ttl-ms:rpush-expiring seed
        set lpush_deadline [r pexpiretime default-ttl-ms:lpush-expiring]
        set rpush_deadline [r pexpiretime default-ttl-ms:rpush-expiring]

        after 5
        r lpush default-ttl-ms:lpush-expiring value
        r rpush default-ttl-ms:rpush-expiring value
        assert_equal $lpush_deadline [r pexpiretime default-ttl-ms:lpush-expiring]
        assert_equal $rpush_deadline [r pexpiretime default-ttl-ms:rpush-expiring]
    }

    test {disabling default-ttl-ms changes only future list creations} {
        r lpush default-ttl-ms:list-before-disable value
        set deadline [r pexpiretime default-ttl-ms:list-before-disable]

        r config set default-ttl-ms 0
        assert_equal $deadline [r pexpiretime default-ttl-ms:list-before-disable]
        r rpush default-ttl-ms:list-after-disable value
        assert_equal -1 [r pttl default-ttl-ms:list-after-disable]
        r config set default-ttl-ms 10000
    }

    test {LPUSHX and RPUSHX do not create missing lists} {
        assert_equal 0 [r lpushx default-ttl-ms:lpushx-missing value]
        assert_equal 0 [r rpushx default-ttl-ms:rpushx-missing value]
        assert_equal 0 [r exists default-ttl-ms:lpushx-missing default-ttl-ms:rpushx-missing]
    }

    test {LMOVE applies default only to a missing destination} {
        r rpush default-ttl-ms:lmove-source a b
        r pexpire default-ttl-ms:lmove-source 30000
        set source_deadline [r pexpiretime default-ttl-ms:lmove-source]

        assert_equal b [r lmove default-ttl-ms:lmove-source default-ttl-ms:lmove-destination RIGHT LEFT]
        assert_equal $source_deadline [r pexpiretime default-ttl-ms:lmove-source]
        set destination_ttl [r pttl default-ttl-ms:lmove-destination]
        assert {$destination_ttl > 9000 && $destination_ttl <= 10000}

        r rpush default-ttl-ms:lmove-source-existing a b
        r pexpire default-ttl-ms:lmove-source-existing 30000
        r rpush default-ttl-ms:lmove-destination-existing x
        r pexpire default-ttl-ms:lmove-destination-existing 25000
        set existing_source_deadline [r pexpiretime default-ttl-ms:lmove-source-existing]
        set existing_destination_deadline [r pexpiretime default-ttl-ms:lmove-destination-existing]

        assert_equal b [r lmove default-ttl-ms:lmove-source-existing default-ttl-ms:lmove-destination-existing RIGHT LEFT]
        assert_equal $existing_source_deadline [r pexpiretime default-ttl-ms:lmove-source-existing]
        assert_equal $existing_destination_deadline [r pexpiretime default-ttl-ms:lmove-destination-existing]
    }

    test {BLMOVE propagates normalized LMOVE before destination expiration} {
        r rpush default-ttl-ms:blmove-source a b
        set repl [attach_to_replication_stream]

        assert_equal b [r blmove default-ttl-ms:blmove-source default-ttl-ms:blmove-destination RIGHT LEFT 0]
        r incr default-ttl-ms:stream-sentinel-1
        assert_replication_stream $repl {
            {multi}
            {select *}
            {lmove default-ttl-ms:blmove-source default-ttl-ms:blmove-destination RIGHT LEFT}
            {pexpireat default-ttl-ms:blmove-destination *}
            {exec}
            {incr default-ttl-ms:stream-sentinel-1}
        }
        close_replication_stream $repl
    } {} {needs:repl}
}

start_server {tags {expire repl external:skip}} {
    set replica [srv 0 client]
    start_server {} {
        set primary [srv 0 client]

        test {list default expiration is authoritative across replication} {
            $primary config set default-ttl-ms 10000
            $replica config set default-ttl-ms 1000
            $replica replicaof [srv 0 host] [srv 0 port]
            wait_for_condition 50 100 {
                [string match {*master_link_status:up*} [$replica info replication]]
            } else {
                fail "Replica did not connect to primary"
            }

            $primary lpush default-ttl-ms:replicated-list value
            wait_for_ofs_sync $replica $primary
            set primary_deadline [$primary pexpiretime default-ttl-ms:replicated-list]
            assert {$primary_deadline > 0}
            assert_equal $primary_deadline [$replica pexpiretime default-ttl-ms:replicated-list]
        }
    }
}

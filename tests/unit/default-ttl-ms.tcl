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

    test {default-ttl-ms applies to every newly created built-in key type} {
        r set default-ttl-ms:string-created value
        r hset default-ttl-ms:hash-created field value
        r sadd default-ttl-ms:set-created member
        r zadd default-ttl-ms:zset-created 1 member
        r xadd default-ttl-ms:stream-created * field value
        r pfadd default-ttl-ms:hll-created member

        foreach key {
            default-ttl-ms:string-created
            default-ttl-ms:hash-created
            default-ttl-ms:set-created
            default-ttl-ms:zset-created
            default-ttl-ms:stream-created
            default-ttl-ms:hll-created
        } {
            set ttl [r pttl $key]
            assert {$ttl > 9000 && $ttl <= 10000}
        }
    }

    test {default-ttl-ms applies only on creation, not replacement or mutation} {
        r config set default-ttl-ms 0
        r set default-ttl-ms:existing-string old PX 30000
        r hset default-ttl-ms:existing-hash old value
        r config set default-ttl-ms 10000

        r set default-ttl-ms:existing-string new
        assert_equal -1 [r pttl default-ttl-ms:existing-string]

        r hset default-ttl-ms:existing-hash new value
        assert_equal -1 [r pttl default-ttl-ms:existing-hash]
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
        r set default-ttl-ms:stream-sentinel-1 0 KEEPTTL
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

    test {default-ttl-ms is visible inside scripts and explicit changes win} {
        assert_equal {10000 -1 30000} [r eval {
            redis.call('hset', KEYS[1], 'f', 'v')
            local initial = redis.call('pttl', KEYS[1])
            redis.call('persist', KEYS[1])
            local persistent = redis.call('pttl', KEYS[1])
            redis.call('pexpire', KEYS[1], 30000)
            return {initial, persistent, redis.call('pttl', KEYS[1])}
        } 1 default-ttl-ms:script]
    }

    test {default-ttl-ms handles duplicate assignments and recreated keys} {
        r mset default-ttl-ms:duplicate first default-ttl-ms:duplicate second
        assert_equal second [r get default-ttl-ms:duplicate]
        assert_equal -1 [r pttl default-ttl-ms:duplicate]
        assert_equal 10000 [r eval {
            redis.call('sadd', KEYS[1], 'first')
            redis.call('del', KEYS[1])
            redis.call('sadd', KEYS[1], 'second')
            return redis.call('pttl', KEYS[1])
        } 1 default-ttl-ms:recreated]
        r set default-ttl-ms:failed-condition old PX 30000
        set deadline [r pexpiretime default-ttl-ms:failed-condition]
        assert_equal old [r set default-ttl-ms:failed-condition new NX GET]
        assert_equal $deadline [r pexpiretime default-ttl-ms:failed-condition]
    }

    test {default-ttl-ms preserves transfer and restore expiration state} {
        r set default-ttl-ms:transfer value KEEPTTL
        r copy default-ttl-ms:transfer default-ttl-ms:copied
        r rename default-ttl-ms:copied default-ttl-ms:renamed
        assert_equal -1 [r pttl default-ttl-ms:renamed]
        set payload [r dump default-ttl-ms:transfer]
        r restore default-ttl-ms:restored 0 $payload
        assert_equal -1 [r pttl default-ttl-ms:restored]
        r restore default-ttl-ms:restored-explicit 30000 $payload
        assert_range [r pttl default-ttl-ms:restored-explicit] 29000 30000
        r move default-ttl-ms:renamed 10
        r select 10
        assert_equal -1 [r pttl default-ttl-ms:renamed]
        assert_equal OK [r select 9]
    } {} {cluster:skip}

    test {RDB reload preserves original deadlines and persistent keys} {
        r set default-ttl-ms:rdb-persistent value KEEPTTL
        r hset default-ttl-ms:rdb-expiring f v
        set deadline [r pexpiretime default-ttl-ms:rdb-expiring]
        r config set default-ttl-ms 60000
        r debug reload
        assert_equal -1 [r pttl default-ttl-ms:rdb-persistent]
        assert_equal $deadline [r pexpiretime default-ttl-ms:rdb-expiring]
    } {} {needs:debug external:skip}
}

start_server {tags {expire external:skip needs:debug} overrides {appendonly yes save {}}} {
    test {AOF replay and rewrite preserve default-ttl-ms deadlines} {
        r config set default-ttl-ms 0
        r set default-ttl-ms:aof-persistent value
        r config set default-ttl-ms 60000
        r hset default-ttl-ms:aof-hash f v
        r sadd default-ttl-ms:aof-set m
        set hash_deadline [r pexpiretime default-ttl-ms:aof-hash]
        set set_deadline [r pexpiretime default-ttl-ms:aof-set]
        r config set default-ttl-ms 120000
        r debug loadaof
        assert_equal -1 [r pttl default-ttl-ms:aof-persistent]
        assert_equal $hash_deadline [r pexpiretime default-ttl-ms:aof-hash]
        assert_equal $set_deadline [r pexpiretime default-ttl-ms:aof-set]
        foreach preamble {yes no} {
            r config set aof-use-rdb-preamble $preamble
            r bgrewriteaof
            waitForBgrewriteaof r
            r debug loadaof
            assert_equal -1 [r pttl default-ttl-ms:aof-persistent]
            assert_equal $hash_deadline [r pexpiretime default-ttl-ms:aof-hash]
            assert_equal $set_deadline [r pexpiretime default-ttl-ms:aof-set]
        }
    }
}

# Catch default recomputation during full sync and lost/reordered expiration
# propagation under repeated writes, overrides, and delete/recreate cycles.
foreach diskless {no yes} {
    start_server {tags {expire repl external:skip}} {
        set replica [srv 0 client]
        start_server {} {
            set primary [srv 0 client]
            test "default TTL bulk replication and full sync diskless=$diskless" {
                $primary config set default-ttl-ms 600000
                $primary config set repl-diskless-sync $diskless
                $primary config set repl-diskless-sync-delay 0
                $replica config set default-ttl-ms 1
                for {set i 0} {$i < 200} {incr i} {
                    $primary hset bulk:$i f original
                    $primary set persistent:$i value KEEPTTL
                }
                $replica replicaof [srv 0 host] [srv 0 port]
                wait_for_condition 100 100 {
                    [status $replica master_link_status] eq {up}
                } else {
                    fail "Default TTL bulk replica failed to synchronize"
                }
                for {set i 0} {$i < 200} {incr i} {
                    set deadline [$primary pexpiretime bulk:$i]
                    assert_range [expr {$deadline - [clock milliseconds]}] 540000 600000
                    assert_equal $deadline [$replica pexpiretime bulk:$i]
                    assert_equal -1 [$replica pttl persistent:$i]
                    $primary hset bulk:$i f updated
                    assert_equal $deadline [$primary pexpiretime bulk:$i]
                    $primary multi
                    $primary del recreated:$i
                    $primary rpush recreated:$i a b
                    $primary pexpire recreated:$i 300000
                    $primary exec
                    $primary eval {
                        redis.call('set', KEYS[1], 'value')
                        redis.call('persist', KEYS[1])
                    } 1 script:$i
                }
                wait_for_ofs_sync $primary $replica
                for {set i 0} {$i < 200} {incr i} {
                    foreach prefix {bulk recreated persistent script} {
                        set key $prefix:$i
                        assert_equal [$primary dump $key] [$replica dump $key]
                        assert_equal [$primary pexpiretime $key] [$replica pexpiretime $key]
                    }
                    assert_equal -1 [$replica pttl script:$i]
                    assert_range [$replica pttl recreated:$i] 240000 300000
                }
            }
        }
    }
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
            foreach command {
                {set default-ttl-ms:repl-string v}
                {hset default-ttl-ms:repl-hash f v}
                {sadd default-ttl-ms:repl-set m}
                {zadd default-ttl-ms:repl-zset 1 m}
                {xadd default-ttl-ms:repl-stream * f v}
                {incr default-ttl-ms:repl-counter}
            } {
                $primary {*}$command
                wait_for_ofs_sync $replica $primary
                set key [lindex $command 1]
                set deadline [$primary pexpiretime $key]
                assert {$deadline > 0}
                assert_equal $deadline [$replica pexpiretime $key]
            }
        }
    }
}

# Standalone command semantics: defaults apply on creation, validation precedes
# mutation, and runtime policy changes leave previously stored deadlines intact.
start_server {tags {expire} overrides {max-ttl-ms 0}} {
    test {max-ttl-ms defaults and configuration bounds} {
        assert_equal {max-ttl-ms 0} [r config get max-ttl-ms]
        r set old value
        assert_equal -1 [r pttl old]
        assert_error {*argument must be between*} {r config set max-ttl-ms -1}
        r config set max-ttl-ms 4611686018427387903
        r set largest value
        assert_range [r pttl largest] 4611686018427386903 4611686018427387903
        assert_error {*argument must be between*} {r config set max-ttl-ms 4611686018427387904}
        r config set max-ttl-ms 60000
        assert_equal -1 [r pttl old]
    }

    test {max-ttl-ms covers all native types and creation paths} {
        # Keep multi-key commands in one slot for external cluster runs.
        foreach command {
            {set string value}
            {set keep-new value KEEPTTL}
            {set nx-new value NX GET}
            {incr counter}
            {lpush list{t} value}
            {hset hash field value}
            {sadd set{t} member}
            {zadd zset{t} 1 member}
            {xadd stream * field value}
            {pfadd hll member}
            {setbit bitmap 10 1}
            {xgroup create group-stream group $ MKSTREAM}
        } {
            r {*}$command
        }
        r mset multi-a{t} a multi-b{t} b duplicate{t} a duplicate{t} b
        r sunionstore set-result{t} set{t}
        r zunionstore zset-result{t} 1 zset{t}
        r lmove list{t} moved{t} RIGHT LEFT
        foreach key {string keep-new nx-new counter hash set{t} zset{t} stream hll bitmap group-stream multi-a{t} multi-b{t} duplicate{t} set-result{t} zset-result{t} moved{t}} {
            assert_range [r pttl $key] 55000 60000
        }
        set deadline [r pexpiretime hash]
        r hset hash another value
        assert_equal $deadline [r pexpiretime hash]
        r set string replacement
        assert_range [r pttl string] 55000 60000
    }

    test {max-ttl-ms accepts bounded relative and absolute expirations} {
        r set bounded value PX 60000
        set deadline [r pexpiretime bounded]
        r set bounded new KEEPTTL
        assert_equal $deadline [r pexpiretime bounded]
        r pexpire bounded 50000
        r expire bounded 50
        r pexpireat bounded [expr {[clock milliseconds] + 50000}]
        r expireat bounded [expr {[clock seconds] + 50}]
        r getex bounded PX 50000
        r setex seconds 50 value
        r psetex milliseconds 50000 value
        r set absolute value PXAT [expr {[clock milliseconds] + 50000}]
        r set absolute-seconds value EXAT [expr {[clock seconds] + 50}]
        foreach key {bounded seconds milliseconds absolute absolute-seconds} {
            assert_range [r pttl $key] 45000 50000
        }
        r pexpire bounded -1
        assert_equal 0 [r exists bounded]
    }

    test {max-ttl-ms rejects TTL changes before mutating values} {
        r set protected original
        set deadline [r pexpiretime protected]
        foreach command [list \
            {set protected changed PX 60001} \
            {set protected changed EX 61} \
            {psetex protected 60001 changed} \
            {setex protected 61 changed} \
            {pexpire protected 60001} \
            {expire protected 61} \
            [list pexpireat protected [expr {[clock milliseconds] + 120000}]] \
            [list expireat protected [expr {[clock seconds] + 120}]] \
            [list set protected changed PXAT [expr {[clock milliseconds] + 120000}]] \
            [list set protected changed EXAT [expr {[clock seconds] + 120}]] \
            {getex protected PX 60001} \
            {getex protected PERSIST} \
            {persist protected}] {
            assert_error {*TTL exceeds max allowed*} {r {*}$command}
            assert_equal original [r get protected]
            assert_equal $deadline [r pexpiretime protected]
        }
        assert_error {*TTL exceeds max allowed*} {r set rejected value PX 60001}
        assert_equal 0 [r exists rejected]
        r set protected{t} original
        assert_error {*TTL exceeds max allowed*} {r msetex 2 protected{t} changed rejected{t} value PX 60001}
        assert_equal original [r get protected{t}]
        assert_equal 0 [r exists rejected{t}]
    }

    test {max-ttl-ms runtime reduction preserves old TTLs and rejects extensions} {
        r set reduced-limit value PX 50000
        set deadline [r pexpiretime reduced-limit]
        r config set max-ttl-ms 10000
        # KEEPTTL does not request a new deadline, even if the old TTL is larger.
        r set reduced-limit replacement KEEPTTL
        assert_equal $deadline [r pexpiretime reduced-limit]
        foreach option {NX XX GT LT} {
            assert_error {*TTL exceeds max allowed*} {r pexpire reduced-limit 20000 $option}
            assert_equal $deadline [r pexpiretime reduced-limit]
        }
        r pexpire reduced-limit 9000
        assert_range [r pttl reduced-limit] 8000 9000
        r config set max-ttl-ms 60000
    }

    test {max-ttl-ms failed conditional writes preserve the original deadline} {
        r set conditional original PX 50000
        set deadline [r pexpiretime conditional]
        assert_equal original [r set conditional changed NX GET]
        assert_equal {} [r set conditional-missing value XX]
        assert_equal original [r get conditional]
        assert_equal $deadline [r pexpiretime conditional]
        assert_equal 0 [r exists conditional-missing]
    }

    test {max-ttl-ms RESTORE validates ABSTTL and overflow before REPLACE} {
        r set restore-target original
        set payload [r dump restore-target]
        set deadline [r pexpiretime restore-target]
        # Both invalid requests must preserve the destination, including its TTL.
        assert_error {*TTL exceeds max allowed*} {
            r restore restore-target [expr {[clock milliseconds] + 120000}] $payload ABSTTL REPLACE
        }
        assert_error {*invalid expire time*} {
            r restore restore-target 9223372036854775807 $payload REPLACE
        }
        assert_equal original [r get restore-target]
        assert_equal $deadline [r pexpiretime restore-target]
        set accepted [expr {[clock milliseconds] + 50000}]
        r restore restore-target $accepted $payload ABSTTL REPLACE
        assert_equal $accepted [r pexpiretime restore-target]
    }

    test {max-ttl-ms transfer and restore validation is atomic} {
        set payload [r dump protected]
        r restore restored 0 $payload
        assert_range [r pttl restored] 55000 60000
        assert_error {*TTL exceeds max allowed*} {r restore protected 60001 $payload REPLACE}
        assert_equal original [r get protected]
        r config set max-ttl-ms 0
        r set long value PX 120000
        r set persistent value
        r config set max-ttl-ms 60000
        foreach command {{rename long protected} {copy long protected REPLACE} {move long 10}} {
            assert_error {*TTL exceeds max allowed*} {r {*}$command}
            assert_equal value [r get long]
            assert_equal original [r get protected]
        }
        r copy persistent copied
        r rename persistent renamed
        r move copied 10
        assert_range [r pttl renamed] 55000 60000
        r select 10
        assert_range [r pttl copied] 55000 60000
        assert_equal OK [r select 9]
    } {} {cluster:skip}

    test {max-ttl-ms is visible within scripts and supports recreation} {
        assert_equal {60000 50000 60000} [r eval {
            redis.call('hset', KEYS[1], 'f', 'v')
            local initial = redis.call('pttl', KEYS[1])
            redis.call('pexpire', KEYS[1], 50000)
            local shorter = redis.call('pttl', KEYS[1])
            redis.call('del', KEYS[1])
            redis.call('sadd', KEYS[1], 'member')
            return {initial, shorter, redis.call('pttl', KEYS[1])}
        } 1 script]
        r multi
        r set transaction value
        r pexpire transaction 50000
        assert_equal {OK 1} [r exec]
        assert_range [r pttl transaction] 45000 50000
    }

    test {max-ttl-ms applies when a blocked move resumes} {
        # A blocked move is re-executed after the source becomes available.
        set blocked [valkey_deferring_client]
        $blocked blmove blocked-source{t} blocked-destination{t} RIGHT LEFT 0
        wait_for_condition 100 10 {[s blocked_clients] == 1} else {
            fail "BLMOVE did not block"
        }
        r lpush blocked-source{t} value
        assert_equal value [$blocked read]
        $blocked close
        assert_range [r pttl blocked-destination{t}] 55000 60000
    }

    test {max-ttl-ms disabling preserves existing deadlines} {
        set deadline [r pexpiretime protected]
        r config set max-ttl-ms 0
        assert_equal $deadline [r pexpiretime protected]
        r set disabled value
        assert_equal -1 [r pttl disabled]
        r persist protected
        assert_equal -1 [r pttl protected]
    }

    test {max-ttl-ms automatically purges data} {
        r config set max-ttl-ms 10
        r hset purge f v
        wait_for_condition 100 10 {[r exists purge] == 0} else {
            fail "Key with automatic TTL did not expire"
        }
        r config set max-ttl-ms 0
    }

    test {max-ttl-ms RDB reload preserves stored expiration state} {
        r set rdb-persistent value
        r set rdb-long value PX 120000
        set deadline [r pexpiretime rdb-long]
        r config set max-ttl-ms 1
        r debug reload
        assert_equal -1 [r pttl rdb-persistent]
        assert_equal $deadline [r pexpiretime rdb-long]
        assert_equal OK [r config set max-ttl-ms 0]
    } {} {needs:debug external:skip}
}

# A stricter local limit on reload must not revalidate or recompute saved TTLs,
# for either incremental AOF commands or either rewritten AOF format.
start_server {tags {expire external:skip needs:debug} overrides {appendonly yes save {}}} {
    test {max-ttl-ms AOF replay and rewrite preserve expiration state} {
        r set persistent value
        # Keep deadlines beyond the test timeout so slow coverage runs cannot expire them.
        r config set max-ttl-ms 3600000
        r hset expiring f v
        r set explicit value PX 3000000
        set deadline [r pexpiretime expiring]
        set explicit_deadline [r pexpiretime explicit]
        r config set max-ttl-ms 1
        foreach format {original yes no} {
            if {$format ne "original"} {
                r config set aof-use-rdb-preamble $format
                r bgrewriteaof
                waitForBgrewriteaof r
            }
            r debug loadaof
            assert_equal -1 [r pttl persistent]
            assert_equal $deadline [r pexpiretime expiring]
            assert_equal $explicit_deadline [r pexpiretime explicit]
        }
    }
}

# Full sync and incremental replication must use the primary's absolute times,
# even when the replica's configured maximum would reject the same client write.
foreach diskless {no yes} {
    start_server {tags {expire repl external:skip}} {
        set replica [srv 0 client]
        start_server {} {
            set primary [srv 0 client]
            test "max-ttl-ms replication preserves primary deadlines diskless=$diskless" {
                $primary set persistent value
                $primary config set max-ttl-ms 60000
                $primary hset full-sync f v
                $primary config set repl-diskless-sync $diskless
                $primary config set repl-diskless-sync-delay 0
                $replica config set max-ttl-ms 1
                $replica replicaof [srv 0 host] [srv 0 port]
                wait_for_condition 100 100 {[status $replica master_link_status] eq {up}} else {
                    fail "Replica failed to synchronize"
                }
                foreach command {
                    {set string v} {hset hash f v} {lpush list v} {sadd set v}
                    {zadd zset 1 v} {xadd stream * f v} {mset a 1 a 2 b 3}
                    {set explicit v PX 50000}
                    {blmove list moved RIGHT LEFT 0}
                } {
                    $primary {*}$command
                }
                $primary eval {
                    redis.call('set', KEYS[1], 'old')
                    redis.call('del', KEYS[1])
                    redis.call('sadd', KEYS[1], 'new')
                } 1 script
                $primary multi
                $primary set transaction v
                $primary pexpire transaction 50000
                $primary exec
                wait_for_ofs_sync $primary $replica
                foreach key {persistent full-sync string hash set zset stream a b explicit moved script transaction} {
                    assert_equal [$primary dump $key] [$replica dump $key]
                    assert_equal [$primary pexpiretime $key] [$replica pexpiretime $key]
                }
                assert_equal -1 [$replica pttl persistent]
            }
        }
    }
}

# Catch accidental cluster-wide policy sharing: each slot owner uses its own
# default, while changing that default must not rewrite existing deadlines.
start_cluster 3 0 {tags {external:skip cluster}} {
    test {maximum TTL is local to each shard and disabling preserves deadlines} {
        wait_for_cluster_state ok
        set defaults {0 300000 600000}
        set seen {0 0 0}
        for {set node 0} {$node < 3} {incr node} {
            R $node config set max-ttl-ms [lindex $defaults $node]
        }
        for {set i 0} {$i < 100} {incr i} {
            set key shard-default:$i
            set owner -1
            for {set node 0} {$node < 3} {incr node} {
                if {[catch {R $node hset $key f v} reply]} {
                    assert_match {MOVED *} $reply
                    continue
                }
                set owner $node
                break
            }
            assert {$owner >= 0}
            lset seen $owner 1
            set ttl [lindex $defaults $owner]
            if {$ttl == 0} {
                assert_equal -1 [R $owner pttl $key]
            } else {
                assert_range [R $owner pttl $key] [expr {$ttl - 10000}] $ttl
            }
            set deadline [R $owner pexpiretime $key]
            R $owner config set max-ttl-ms 0
            R $owner hset $key f updated
            assert_equal $deadline [R $owner pexpiretime $key]
            R $owner config set max-ttl-ms $ttl
        }
        assert_equal {1 1 1} $seen
    }
}

# Catch local-default recomputation on replicas and after promotion.
start_cluster 1 1 {tags {external:skip cluster}} {
    test {maximum TTL cluster becomes ready} {
        wait_for_cluster_state ok
    }
    set primary [srv 0 client]
    set replica [srv -1 client]
    $replica readonly
    $primary config set max-ttl-ms 600000
    $replica config set max-ttl-ms 0

    # Same-slot names let multi-key commands exercise TTL semantics without
    # cross-slot routing errors masking a validation or propagation regression.
    test {cluster max-ttl-ms covers native key types and bounded explicit TTLs} {
        foreach command {
            {set "{policy}:string" v}
            {lpush "{policy}:list" v}
            {sadd "{policy}:set" v}
            {hset "{policy}:hash" f v}
            {zadd "{policy}:zset" 1 v}
            {xadd "{policy}:stream" * f v}
            {pfadd "{policy}:hll" v}
            {set "{policy}:keep" v KEEPTTL}
        } {
            $primary {*}$command
            assert_range [$primary pttl [lindex $command 1]] 590000 600000
        }
        $primary msetex 2 "{policy}:a" first "{policy}:b" second PX 300000
        $primary getex "{policy}:string" PX 300000
        wait_for_ofs_sync $primary $replica
        foreach suffix {string list set hash zset stream hll keep a b} {
            set key "{policy}:$suffix"
            assert_equal [$primary dump $key] [$replica dump $key]
            assert_equal [$primary pexpiretime $key] [$replica pexpiretime $key]
        }
        assert_range [$primary pttl "{policy}:a"] 290000 300000
    }

    test {cluster max-ttl-ms rejects oversized TTLs without partial writes} {
        $primary set "{reject}:existing" original PX 300000
        set deadline [$primary pexpiretime "{reject}:existing"]
        set payload [$primary dump "{reject}:existing"]
        foreach command [list \
            {set "{reject}:existing" changed PX 600001} \
            {pexpire "{reject}:existing" 600001} \
            {getex "{reject}:existing" PX 600001} \
            {persist "{reject}:existing"} \
            {getex "{reject}:existing" PERSIST} \
            {msetex 2 "{reject}:existing" changed "{reject}:missing" new PX 600001} \
            [list restore "{reject}:existing" 600001 $payload REPLACE] \
            [list pexpireat "{reject}:existing" [expr {[clock milliseconds] + 1200000}]]] {
            assert_error {*TTL exceeds max allowed*} {$primary {*}$command}
            assert_equal original [$primary get "{reject}:existing"]
            assert_equal $deadline [$primary pexpiretime "{reject}:existing"]
            assert_equal 0 [$primary exists "{reject}:missing"]
        }
        # Rejected changes must leave the replica at the same value and deadline.
        wait_for_ofs_sync $primary $replica
        assert_equal original [$replica get "{reject}:existing"]
        assert_equal $deadline [$replica pexpiretime "{reject}:existing"]
        assert_equal 0 [$replica exists "{reject}:missing"]
    }

    test {cluster max-ttl-ms preserves script and transaction expiry ordering} {
        assert_equal {600000 300000} [$primary eval {
            redis.call('set', KEYS[1], 'old')
            redis.call('del', KEYS[1])
            redis.call('hset', KEYS[1], 'f', 'new')
            local initial = redis.call('pttl', KEYS[1])
            redis.call('pexpire', KEYS[1], 300000)
            return {initial, redis.call('pttl', KEYS[1])}
        } 1 "{ordering}:script"]
        $primary multi
        $primary mset "{ordering}:a" old "{ordering}:a" new "{ordering}:b" value
        $primary pexpire "{ordering}:b" 300000
        assert_equal {OK 1} [$primary exec]
        # A stale default queued after the explicit expiry would change this TTL.
        wait_for_ofs_sync $primary $replica
        foreach key {"{ordering}:script" "{ordering}:a" "{ordering}:b"} {
            assert_equal [$primary dump $key] [$replica dump $key]
            assert_equal [$primary pexpiretime $key] [$replica pexpiretime $key]
        }
        assert_range [$replica pttl "{ordering}:b"] 290000 300000
    }

    test {cluster maximum TTL deadlines survive mixed-type writes and replication} {
        for {set i 0} {$i < 100} {incr i} {
            set key "{max-ttl}:$i"
            $primary hset $key field value
            set deadline [$primary pexpiretime $key]
            assert_range [expr {$deadline - [clock milliseconds]}] 590000 600000
            $primary hset $key field updated
            assert_equal $deadline [$primary pexpiretime $key]
            $primary lpush "${key}:list" value
            $primary sadd "${key}:set" value
        }
        wait_for_ofs_sync $primary $replica
        for {set i 0} {$i < 100} {incr i} {
            foreach suffix {{} :list :set} {
                set key "{max-ttl}:$i$suffix"
                assert_equal [$primary pexpiretime $key] [$replica pexpiretime $key]
                assert_equal [$primary dump $key] [$replica dump $key]
            }
        }
    }

    test {cluster promotion preserves old deadlines and uses promoted node default} {
        set key "{max-ttl}:0"
        set deadline [$primary pexpiretime $key]
        $replica cluster failover
        wait_for_condition 100 100 {
            [status $replica role] eq {master} && [status $primary master_link_status] eq {up}
        } else {
            fail "Maximum TTL cluster failover did not complete"
        }
        wait_for_cluster_state ok
        assert_equal $deadline [$replica pexpiretime $key]
        $replica hset "{max-ttl}:after-promotion" field value
        assert_equal -1 [$replica pttl "{max-ttl}:after-promotion"]
        $primary readonly
        wait_for_ofs_sync $replica $primary
        assert_equal -1 [$primary pttl "{max-ttl}:after-promotion"]
        assert_equal $deadline [$primary pexpiretime $key]
    }
}

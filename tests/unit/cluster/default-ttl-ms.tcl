# Catch accidental cluster-wide policy sharing: each slot owner uses its own
# default, while changing that default must not rewrite existing deadlines.
start_cluster 3 0 {tags {external:skip cluster}} {
    test {default TTL is local to each shard and disabling preserves deadlines} {
        wait_for_cluster_state ok
        set defaults {0 300000 600000}
        set seen {0 0 0}
        for {set node 0} {$node < 3} {incr node} {
            R $node config set default-ttl-ms [lindex $defaults $node]
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
            R $owner config set default-ttl-ms 0
            R $owner hset $key f updated
            assert_equal $deadline [R $owner pexpiretime $key]
            R $owner config set default-ttl-ms $ttl
        }
        assert_equal {1 1 1} $seen
    }
}

# Catch local-default recomputation on replicas and after promotion.
start_cluster 1 1 {tags {external:skip cluster}} {
    test {default TTL cluster becomes ready} {
        wait_for_cluster_state ok
    }
    set primary [srv 0 client]
    set replica [srv -1 client]
    $replica readonly
    $primary config set default-ttl-ms 600000
    $replica config set default-ttl-ms 0

    test {cluster default TTL deadlines survive mixed-type writes and replication} {
        for {set i 0} {$i < 100} {incr i} {
            set key "{default-ttl}:$i"
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
                set key "{default-ttl}:$i$suffix"
                assert_equal [$primary pexpiretime $key] [$replica pexpiretime $key]
                assert_equal [$primary dump $key] [$replica dump $key]
            }
        }
    }

    test {cluster promotion preserves old deadlines and uses promoted node default} {
        set key "{default-ttl}:0"
        set deadline [$primary pexpiretime $key]
        $replica cluster failover
        wait_for_condition 100 100 {
            [status $replica role] eq {master} && [status $primary master_link_status] eq {up}
        } else {
            fail "Default TTL cluster failover did not complete"
        }
        wait_for_cluster_state ok
        assert_equal $deadline [$replica pexpiretime $key]
        $replica hset "{default-ttl}:after-promotion" field value
        assert_equal -1 [$replica pttl "{default-ttl}:after-promotion"]
        $primary readonly
        wait_for_ofs_sync $replica $primary
        assert_equal -1 [$primary pttl "{default-ttl}:after-promotion"]
        assert_equal $deadline [$primary pexpiretime $key]
    }
}

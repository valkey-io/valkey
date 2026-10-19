set testmodule [file normalize tests/modules/keymemorydelta.so]

proc kmd_stat {name {level 0}} {
    dict get [r $level kmd.stats] $name
}

# The test module keeps a shadow of the keyspace built only from key memory
# delta events. Check that it matches the keyspace and MEMORY USAGE, and that
# every event so far was consistent with the previous ones.
proc assert_kmd_consistent {{dbs {9}} {level 0} {home 9}} {
    foreach db $dbs {
        r $level select $db
        set expected {}
        foreach key [r $level keys *] {
            set usage [r $level memory usage $key]
            if {$usage ne {}} {lappend expected [list $key $usage]}
        }
        set shadow {}
        foreach {key usage} [r $level kmd.dump $db] {
            lappend shadow [list $key $usage]
        }
        assert_equal [lsort -index 0 $expected] [lsort -index 0 $shadow]
    }
    r $level select $home
    assert_equal 0 [kmd_stat chain_errors $level]
    assert_equal 0 [kmd_stat noop_errors $level]
}

# Check the number of events fired while running a script.
proc assert_kmd_events {expected script} {
    set events [kmd_stat events]
    uplevel 1 $script
    assert_equal $expected [expr {[kmd_stat events] - $events}]
}

# Logged events (db key old_exists old_bytes new_exists new_bytes) of a key.
proc kmd_log_for {key {level 0}} {
    set entries {}
    foreach entry [r $level kmd.log] {
        if {[lindex $entry 1] eq $key} {lappend entries $entry}
    }
    return $entries
}

proc kmd_reset {} {
    r flushall
    r kmd.reset
}

tags "modules" {
    start_server [list overrides [list loadmodule "$testmodule"]] {
        test {Key memory delta tracks writes to all data types} {
            kmd_reset
            r set string value
            r append string -more
            r setrange string 20 x
            r incrby counter 1000000000000
            for {set i 0} {$i < 1000} {incr i} {
                r hset hash field$i value$i
                r rpush list element$i
                r sadd set member$i
                r sadd intset $i
                r zadd zset $i member$i
                r xadd stream * field $i
            }
            foreach {key encoding} {hash hashtable list quicklist set hashtable zset btree} {
                assert_encoding $encoding $key
            }
            r hdel hash field0
            r lpop list
            r spop set
            r zrem zset member1
            r xtrim stream MAXLEN 100
            r del intset
            assert_kmd_consistent
        }

        test {Key memory delta payload describes the key before and after the unit} {
            kmd_reset
            r set key value
            set size [r memory usage key]
            assert_equal [list 9 key 0 0 1 $size] [lindex [r kmd.log] end]
            r append key [string repeat x 100]
            set new_size [r memory usage key]
            assert_equal [list 9 key 1 $size 1 $new_size] [lindex [r kmd.log] end]
            r del key
            assert_equal [list 9 key 1 $new_size 0 0] [lindex [r kmd.log] end]

            # Writes that change nothing aren't reported.
            r set key value
            r expire key 1000
            assert_kmd_events 0 {
                r set key value keepttl
                r set key other nx
                r expire key 2000
                r get key
                r del missing
            }

            # Keys moved between databases are reported in both.
            r set moved [string repeat x 50]
            set size [r memory usage moved]
            r move moved 0
            set entries [lsort -index 0 -integer [lrange [kmd_log_for moved] 1 2]]
            assert_equal {0 moved 0 0 1} [lrange [lindex $entries 0] 0 4]
            assert_equal [list 9 moved 1 $size 0 0] [lindex $entries 1]

            r set source value
            r rename source destination
            assert_equal {0 0} [lrange [lindex [kmd_log_for source] end] 4 5]
            assert_equal {9 destination 0 0 1} [lrange [lindex [kmd_log_for destination] end] 0 4]
            r copy destination copy
            r copy destination copy DB 0
            set payload [r dump destination]
            r set destination [string repeat y 100]
            r restore destination 0 $payload REPLACE
            r restore restored 0 $payload
            assert_kmd_consistent {0 9}
        }

        test {Key memory delta coalesces transactions, scripts and functions} {
            kmd_reset
            r function load replace {#!lua name=kmd
server.register_function('kmd_write', function(keys, args)
    server.call('set', keys[1], args[1])
    server.call('append', keys[1], args[2])
end)}
            assert_kmd_events 1 {
                r multi
                r set key one
                r append key two
                r exec
            }
            assert_kmd_events 1 {
                r eval {redis.call('hset', KEYS[1], 'one', '1'); redis.call('hset', KEYS[1], 'two', '2')} 1 hash
            }
            assert_kmd_events 1 {r fcall kmd_write 1 function one two}
            assert_kmd_events 1 {
                r multi
                r set key four
                r eval {redis.call('append', KEYS[1], 'five')} 1 key
                r exec
            }
            r function flush
            assert_kmd_consistent
        }

        test {Key memory delta handles flushes and SWAPDB} {
            foreach flush {{flushdb} {flushdb async} {flushall} {flushall async}} {
                kmd_reset
                r set before 1
                r multi
                r incr inside
                r {*}$flush
                r set after 1
                r exec
                assert_equal {after} [r keys *]
                assert_equal {} [kmd_log_for inside]
                assert_equal 1 [llength [kmd_log_for before]]
                assert_kmd_consistent
            }

            kmd_reset
            r select 0
            r set zero 0
            r select 9
            r set nine 9
            r multi
            r append nine 9
            r swapdb 0 9
            r append nine 9
            r exec
            assert_kmd_consistent {0 9}

            # A key modified before and after a SWAPDB is reported once, in the
            # database where it ends up.
            kmd_reset
            r multi
            r set moved a
            r swapdb 0 9
            r select 0
            r append moved b
            r select 9
            r exec
            set entries [kmd_log_for moved]
            assert_equal 1 [llength $entries]
            assert_equal {0 moved 0 0 1} [lrange [lindex $entries 0] 0 4]
            assert_kmd_consistent {0 9}

            # A unit touching many keys, in a database flushed after the first
            # key was touched in another one.
            kmd_reset
            r multi
            r incr first
            r select 0
            for {set i 0} {$i < 20} {incr i} {r set key$i $i}
            r select 9
            r flushdb
            r set last 1
            r exec
            assert_kmd_consistent {0 9}
        }

        test {Key memory delta reports lazily expired keys, not read by callbacks} {
            kmd_reset
            r debug set-active-expire 0
            with_cleanup {
                r set kmd:expired value px 1
                r phset kmd:tree path fields 1 field value
                after 10
                assert_kmd_events 1 {r set kmd:read 1}
                # The callback read the expired key without deleting it.
                assert_equal 1 [llength [kmd_log_for kmd:expired]]
                assert_match {*keys=3,*} [r info keyspace]
                assert_equal {} [r get kmd:expired]
                assert_equal {0 0} [lrange [lindex [kmd_log_for kmd:expired] end] 4 5]
            } {
                r debug set-active-expire 1
            }
            assert_kmd_consistent
        } {} {needs:debug}

        test {Key memory delta tracks write commands that look keys up for reading} {
            # Keyspace statistics must not depend on the tracking.
            set commands {
                {set string value} {getex string EX 100} {getex string PERSIST} {getdel string}
                {getex missing EX 100} {set string value} {getset string a-value-longer-than-the-previous-one}
                {hset hash f1 v1 f2 v2} {hgetex hash EX 100 FIELDS 1 f1} {hpersist hash FIELDS 1 f1}
                {xadd stream 1-0 a 1} {xgroup create stream group 0} {xreadgroup GROUP group c1 STREAMS stream >}
                {xack stream group 1-0} {xadd stream 2-0 b 2} {xreadgroup GROUP group c1 STREAMS stream >}
                {xclaim stream group c2 0 2-0} {xautoclaim stream group c3 0 0-0}
            }
            set stats {}
            foreach subscribe {kmd.unsubscribe kmd.subscribe} {
                kmd_reset
                r $subscribe
                set hits [s keyspace_hits]
                set misses [s keyspace_misses]
                foreach command $commands {r {*}$command}
                lappend stats [list [expr {[s keyspace_hits] - $hits}] [expr {[s keyspace_misses] - $misses}]]
            }
            assert_equal [lindex $stats 0] [lindex $stats 1]
            assert_kmd_consistent

            # PFCOUNT unshares the string of a freshly loaded HyperLogLog.
            r pfadd hll a b c
            r debug reload
            r pfcount hll
            assert_kmd_consistent
        } {} {needs:debug}

        test {Key memory delta tracks aggregates resized by reads} {
            kmd_reset
            set members {}
            set pairs {}
            set scored {}
            for {set i 0} {$i < 4000} {incr i} {
                lappend members m$i
                lappend pairs m$i v
                lappend scored $i m$i
            }
            # Lookups make progress on incremental rehashing.
            r sadd set {*}$members
            r hset hash {*}$pairs
            r zadd zset {*}$scored
            for {set i 0} {$i < 100} {incr i} {
                r phset tree path$i fields 2 field value$i other value$i
            }
            for {set i 0} {$i < 4000} {incr i 4} {
                r sismember set m$i
                r hget hash m$i
                r zscore zset m$i
                if {$i < 400} {r phget tree path[expr {$i / 4}] field}
            }
            assert_kmd_consistent

            # Delete most elements while resizing is forbidden, as during a
            # fork: scans and random sampling then shrink the tables.
            r debug dict-resizing 0
            r srem set {*}[lrange $members 0 3679]
            r hdel hash {*}[lrange $members 0 3679]
            r zrem zset {*}[lrange $members 0 3679]
            r debug dict-resizing 1
            assert_kmd_consistent
            r sscan set 0
            r hrandfield hash
            r zscan zset 0
            assert_kmd_consistent

            # The same, through the key handles of a module's keyspace scan.
            r sadd set {*}$members
            r hset hash {*}$pairs
            r zadd zset {*}$scored
            r debug dict-resizing 0
            r srem set {*}[lrange $members 0 3679]
            r hdel hash {*}[lrange $members 0 3679]
            r zrem zset {*}[lrange $members 0 3679]
            r debug dict-resizing 1
            assert_kmd_consistent
            r kmd.scan
            assert_kmd_consistent
        } {} {needs:debug}

        test {Key memory delta tracks lists compressed by reads} {
            # An insertion grows a middle node that was too small to compress
            # without compressing it, and the next iteration compresses it.
            kmd_reset
            set depth [lindex [r config get list-compress-depth] 1]
            set size [lindex [r config get list-max-listpack-size] 1]
            r config set list-compress-depth 1
            r config set list-max-listpack-size 5
            with_cleanup {
                r rpush list a1 a2 a3 a4 a5 b1 b2 b3 b4 b5 c1 c2 c3 c4 c5
                r lrem list 1 b1
                r lrem list 1 b2
                r linsert list before b3 [string repeat x 100]
                assert_kmd_consistent
                set usage [r memory usage list]
                r lrange list 0 -1
                assert_lessthan [r memory usage list] $usage
                assert_kmd_consistent
            } {
                r config set list-compress-depth $depth
                r config set list-max-listpack-size $size
            }
        }

        test {Key memory delta reports actively expired keys and fields} {
            foreach max_listpack {512 0} {
                with_config hash-max-listpack-entries $max_listpack {
                    kmd_reset
                    r set key value px 20
                    r hset partial f1 v1 f2 [string repeat x 100] f3 v3
                    r hset whole f1 v1
                    set expired [s expired_fields]
                    r hpexpire partial 20 FIELDS 1 f2
                    r hpexpire whole 20 FIELDS 1 f1
                    wait_for_condition 50 100 {
                        [s expired_fields] == $expired + 2 && [r dbsize] == 1
                    } else {
                        fail "keys and hash fields were not expired"
                    }
                    assert_equal {partial} [r keys *]
                    assert_kmd_consistent
                }
            }
        }

        test {Key memory delta tracks volatile sets restructured by field expiration} {
            # Leave the set of volatile fields in a shape that the next active
            # expiration visit restructures, although no field is due.
            kmd_reset
            r debug set-active-expire 0
            r config set hash-max-listpack-entries 0
            with_cleanup {
                set t [expr {[clock milliseconds] + 100000000}]
                set hour 3600000
                set fields {f0 v}
                set g {}
                set x {}
                for {set i 0} {$i < 10} {incr i} {
                    lappend fields g$i v
                    lappend g g$i
                }
                for {set i 0} {$i < 130} {incr i} {
                    lappend fields x$i v
                    lappend x x$i
                }
                r hset hash {*}$fields
                r hpexpireat hash $t FIELDS 1 f0
                r hpexpireat hash [expr {$t + $hour}] FIELDS 10 {*}$g
                for {set i 0} {$i < 130} {incr i} {
                    r hpexpireat hash [expr {$t + (2 + $i) * $hour}] FIELDS 1 x$i
                }
                r hdel hash {*}$x
                r hpexpireat hash [expr {$t + $hour}] FIELDS 1 f0
                assert_kmd_consistent
                set usage [r memory usage hash]
                r debug set-active-expire 1
                wait_for_condition 50 100 {
                    [r memory usage hash] != $usage
                } else {
                    fail "the volatile set was not restructured"
                }
                r debug set-active-expire 0
                assert_kmd_consistent
            } {
                r config set hash-max-listpack-entries 512
                r debug set-active-expire 1
            }
        } {} {needs:debug}

        test {Key memory delta reports evicted keys without evicting more} {
            # Without a time limit, eviction stops only once it accounted for
            # enough freed memory. The bound is loose since allkeys-random
            # varies, but tracking counted as freed memory evicts far more.
            set lazy_eviction [lindex [r config get lazyfree-lazy-eviction] 1]
            set tenacity [lindex [r config get maxmemory-eviction-tenacity] 1]
            r config set maxmemory-policy allkeys-random
            r config set maxmemory-eviction-tenacity 100
            with_cleanup {
                foreach lazy {no yes} {
                    r config set lazyfree-lazy-eviction $lazy
                    set evicted {}
                    foreach subscribe {kmd.unsubscribe kmd.subscribe} {
                        kmd_reset
                        r $subscribe
                        for {set i 0} {$i < 4000} {incr i} {
                            r set key$i v
                        }
                        set before [s evicted_keys]
                        r config set maxmemory [expr {[s used_memory] - 60000}]
                        r set trigger v
                        r config set maxmemory 0
                        lappend evicted [expr {[s evicted_keys] - $before}]
                    }
                    assert_kmd_consistent
                    lassign $evicted unsubscribed subscribed
                    assert_morethan $unsubscribed 0
                    assert_lessthan $subscribed [expr {$unsubscribed * 2 + 50}]
                }
            } {
                r config set maxmemory 0
                r config set maxmemory-policy noeviction
                r config set maxmemory-eviction-tenacity $tenacity
                r config set lazyfree-lazy-eviction $lazy_eviction
                r kmd.subscribe
            }
        } {} {needs:config-maxmemory}

        test {Key memory delta reports keys loaded from an RDB file} {
            r flushall
            r set kmd:load:del 1
            r set kmd:load:set 1
            r set plain 1
            r hset hash field value
            r kmd.reset
            r debug reload
            assert_equal 0 [r exists kmd:load:del]
            assert_equal 6 [kmd_stat events]
            assert_equal 6 [kmd_stat loading_events]
            # Keys are reported before their loaded notification, whose
            # callbacks may modify them.
            set entries [kmd_log_for kmd:load:del]
            assert_equal 2 [llength $entries]
            assert_equal {9 kmd:load:del 0 0 1} [lrange [lindex $entries 0] 0 4]
            assert_equal {0 0} [lrange [lindex $entries 1] 4 5]
            set entries [kmd_log_for kmd:load:set]
            assert_equal 2 [llength $entries]
            assert_equal [list 1 [r memory usage kmd:load:set]] [lrange [lindex $entries 1] 4 5]
            assert_kmd_consistent

            r flushall
            r set duplicate small
            r save
            r set duplicate [string repeat x 200]
            set old_size [r memory usage duplicate]
            r kmd.reset
            r debug reload nosave noflush merge
            set entries [kmd_log_for duplicate]
            assert_equal [list [list 9 duplicate 1 $old_size 1 [r memory usage duplicate]]] $entries
        } {} {needs:debug}

        test {Key memory delta subscription can be removed and restored} {
            # Only keys written while subscribed are reported.
            kmd_reset
            r kmd.unsubscribe
            r set ignored 1
            r multi
            r set c 1
            r kmd.subscribe
            r set d 1
            r exec
            assert_equal [list [list 9 d 0 0 1 [r memory usage d]]] [r kmd.log]
            r multi
            r set a 1
            r kmd.unsubscribe
            r set b 1
            r exec
            r kmd.subscribe
            assert_equal 1 [kmd_stat events]

            # Unload the module while deltas are pending.
            r multi
            r set e 1
            r module unload keymemorydelta
            r exec
            r set e 2
            assert_equal OK [r module load $testmodule]
            kmd_reset
            r set f 1
            assert_kmd_consistent
        }

        test {Key memory delta callbacks write through post notification jobs} {
            # A job registered by a callback runs within the same unit, and
            # its write is reported and propagated with the command. Runs last
            # since the replication backlog it creates skews eviction.
            kmd_reset
            set repl [attach_to_replication_stream]
            assert_kmd_events 2 {r set kmd:job 1}
            assert_replication_stream $repl {
                {multi}
                {select *}
                {set kmd:job 1}
                {set kmd:job:result *}
                {exec}
            }
            close_replication_stream $repl
            assert_kmd_consistent
        } {} {needs:repl}
    }

    start_server [list overrides [list loadmodule "$testmodule keyevent" appendonly yes]] {
        test {Key memory delta reports keys loaded from an AOF file} {
            r set a 1
            r multi
            r set b 2
            r flushdb async
            r set c 3
            r exec
            r multi
            r set d 4
            r del d
            r exec
            r hset hash field value
            r append a [string repeat x 100]
            r set overwritten small
            r set overwritten [string repeat x 100]
            r set deleted value
            r del deleted
            r kmd.reset
            r debug loadaof
            assert_morethan [kmd_stat events] 0
            assert_equal [kmd_stat events] [kmd_stat loading_events]
            assert_kmd_consistent

            # The module also sees the keys loaded when the server starts, with
            # each replayed command or transaction in an execution unit of its
            # own, although the server isn't running commands yet.
            restart_server 0 true false
            assert_morethan [kmd_stat loading_events] 0
            assert_equal {} [kmd_log_for d]
            assert_kmd_consistent
        } {} {needs:debug}
    }

    start_server [list overrides [list loadmodule "$testmodule"]] {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            set primary [srv -1 client]
            set primary_host [srv -1 host]
            set primary_port [srv -1 port]

            test {Key memory delta on a replica} {
                $primary config set repl-diskless-sync yes
                $primary config set repl-diskless-sync-delay 0
                $primary set p1 value
                $primary hset p2 field value
                for {set i 0} {$i < 100} {incr i} {
                    $primary rpush p3 $i
                }

                # The replica loads the dataset into a temporary database,
                # replacing keys of its own that aren't reported.
                r config set repl-diskless-load swapdb
                r set r1 value
                r set r2 [string repeat x 100]
                r kmd.reset
                r replicaof $primary_host $primary_port
                wait_for_sync r
                wait_for_ofs_sync $primary r
                assert_equal 3 [kmd_stat async_events]
                assert_equal 0 [kmd_stat async_deletions]
                assert_equal {} [kmd_log_for r1]
                assert_equal {} [kmd_log_for r2]
                assert_kmd_consistent

                set events [kmd_stat replica_events]
                $primary set p4 value
                $primary append p1 more
                wait_for_ofs_sync $primary r
                assert_morethan [kmd_stat replica_events] $events
                assert_kmd_consistent
                assert_kmd_consistent {9} -1
            }

            test {Key memory delta on a replica serving reads during an asynchronous load} {
                r config set loading-process-events-interval-bytes 1024
                for {set i 0} {$i < 1500} {incr i} {
                    $primary set fill$i [string repeat v 100]
                }
                # Leave the replica's copy of a set in the middle of a rehash.
                for {set i 0} {$i < 5000} {incr i} {
                    $primary sadd rehashing m$i
                    if {$i < 200} continue
                    wait_for_ofs_sync $primary r
                    if {[string match {*Hash table 1*} [r debug htstats-key rehashing full]]} break
                }
                assert_kmd_consistent

                # A full resync keeping the replication id loads asynchronously,
                # while the replica serves reads from the dataset it replaces.
                r config set key-load-delay 500
                $primary config set repl-backlog-size 16384
                # The replica reconnects right away, so disconnect it and trim
                # the part of the backlog it needs atomically.
                $primary multi
                $primary client kill type replica
                $primary set overrun1 [string repeat o 100000]
                $primary set overrun2 [string repeat o 100000]
                $primary del overrun1 overrun2
                $primary exec
                wait_for_condition 500 10 {
                    [s async_loading] eq 1
                } else {
                    fail "the replica did not load asynchronously"
                }
                for {set i 0} {$i < 8} {incr i} {
                    r sismember rehashing m1
                }
                r config set key-load-delay 0
                wait_for_condition 100 100 {
                    [s async_loading] eq 0 && [s master_link_status] eq "up"
                } else {
                    fail "the asynchronous load did not complete"
                }
                wait_for_ofs_sync $primary r
                assert_kmd_consistent
            } {} {needs:debug}
        }
    }

    start_cluster 2 0 [list tags [list external:skip cluster] overrides [list loadmodule "$testmodule"]] {
        test {Key memory delta reports keys removed and added by slot operations} {
            set slot [R 0 cluster keyslot "{kmd}"]
            set source [expr {$slot < 8192 ? 0 : 1}]
            set target [expr {1 - $source}]
            proc fill_slot {node} {
                for {set i 0} {$i < 10} {incr i} {R $node set "{kmd}key$i" [string repeat x $i]}
            }
            fill_slot $source
            assert_kmd_consistent {0} -$source 0
            R $source cluster flushslot $slot
            assert_equal 0 [R $source cluster countkeysinslot $slot]
            assert_kmd_consistent {0} -$source 0

            fill_slot $source
            R $source cluster migrateslots slotsrange $slot $slot node [R $target cluster myid]
            wait_for_condition 100 100 {
                [R $target cluster countkeysinslot $slot] == 10 && [R $source cluster countkeysinslot $slot] == 0
            } else {
                fail "slot was not migrated"
            }
            assert_kmd_consistent {0} -$source 0
            assert_kmd_consistent {0} -$target 0
        }
    }
}

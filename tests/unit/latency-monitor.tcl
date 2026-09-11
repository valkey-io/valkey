start_server {tags {"latency-monitor needs:latency"}} {
    # Set a threshold high enough to avoid spurious latency events.
    r config set latency-monitor-threshold 200
    r latency reset

    test {LATENCY HISTOGRAM with empty histogram} {
        r config resetstat
        set histo [dict create {*}[r latency histogram]]
        # Config resetstat is recorded
        assert_equal [dict size $histo] 1
        assert_match {*config|resetstat*} $histo
    }

    test {LATENCY HISTOGRAM all commands} {
        r config resetstat
        r set a b
        r set c d
        set histo [dict create {*}[r latency histogram]]
        assert_match {calls 2 histogram_usec *} [dict get $histo set]
        assert_match {calls 1 histogram_usec *} [dict get $histo "config|resetstat"]
    }

    test {LATENCY HISTOGRAM sub commands} {
        r config resetstat
        r client id
        r client list
        # parent command reply with its sub commands
        set histo [dict create {*}[r latency histogram client]]
        assert {[dict size $histo] == 2}
        assert_match {calls 1 histogram_usec *} [dict get $histo "client|id"]
        assert_match {calls 1 histogram_usec *} [dict get $histo "client|list"]

        # explicitly ask for one sub-command
        set histo [dict create {*}[r latency histogram "client|id"]]
        assert {[dict size $histo] == 1}
        assert_match {calls 1 histogram_usec *} [dict get $histo "client|id"]
    }

    test {LATENCY HISTOGRAM with a subset of commands} {
        r config resetstat
        r set a b
        r set c d
        r get a
        r hset f k v
        r hgetall f
        set histo [dict create {*}[r latency histogram set hset]]
        assert_match {calls 2 histogram_usec *} [dict get $histo set]
        assert_match {calls 1 histogram_usec *} [dict get $histo hset]
        assert_equal [dict size $histo] 2
        set histo [dict create {*}[r latency histogram hgetall get zadd]]
        assert_match {calls 1 histogram_usec *} [dict get $histo hgetall]
        assert_match {calls 1 histogram_usec *} [dict get $histo get]
        assert_equal [dict size $histo] 2
    }

    test {LATENCY HISTOGRAM command} {
        r config resetstat
        r set a b
        r get a
        assert {[llength [r latency histogram set get]] == 4}
    }

    test {LATENCY HISTOGRAM with wrong command name skips the invalid one} {
        r config resetstat
        assert {[llength [r latency histogram blabla]] == 0}
        assert {[llength [r latency histogram blabla blabla2 set get]] == 0}
        r set a b
        r get a
        assert_match {calls 1 histogram_usec *} [lindex [r latency histogram blabla blabla2 set get] 1]
        assert_match {calls 1 histogram_usec *} [lindex [r latency histogram blabla blabla2 set get] 3]
        assert {[string length [r latency histogram blabla set get]] > 0}
    }

    # Largest latency bucket (usec) recorded for a LATENCY HISTOGRAM command entry.
    proc max_histogram_usec {cmd_entry} {
        set maxk 0
        foreach {k v} [dict get $cmd_entry histogram_usec] {
            if {$k > $maxk} {set maxk $k}
        }
        return $maxk
    }

    test {LATENCY HISTOGRAM E2E is disabled by default} {
        # Default feature set is "cmd" only (processing time); e2e is off.
        assert_equal {cmd} [lindex [r config get latency-tracking-features] 1]
        r config resetstat
        r set stk v
        r get stk
        # Processing histogram still records; the e2e one stays empty.
        assert {[llength [r latency histogram]] > 0}
        assert {[llength [r latency histogram e2e]] == 0}
    }

    test {LATENCY HISTOGRAM default reporting - processing time} {
        r config resetstat
        r set stk v
        r get stk
        set cmd [dict create {*}[r latency histogram cmd]]
        set only [dict create {*}[r latency histogram cmd set]]
        assert_equal [dict keys $only] {set}
    }

    test {LATENCY HISTOGRAM E2E recording} {
        r config set latency-tracking-features "cmd e2e"
        r config resetstat
        set rd [valkey_deferring_client]
        $rd set stk v
        $rd get stk
        $rd get stk
        $rd flush
        # Drain the replies so the writes complete (and samples seal) before close.
        assert_equal {OK} [$rd read]
        assert_equal {v} [$rd read]
        assert_equal {v} [$rd read]
        $rd close
        set histo [dict create {*}[r latency histogram e2e]]
        assert_match {calls 1 histogram_usec *} [dict get $histo set]
        assert_match {calls 2 histogram_usec *} [dict get $histo get]
        r config set latency-tracking-features cmd
    }

    test {LATENCY HISTOGRAM E2E measures end-to-end time including queue and block wait} {
        r config set latency-tracking-features "cmd e2e"
        r config resetstat
        r del stlist
        # One batch: a fast SET, a command that blocks ~3s, then a GET that is queued behind the blocking command
        set rd [valkey_deferring_client]
        $rd set stk v
        $rd blpop stlist 3
        $rd get stk
        $rd flush
        # Wait for all replies
        assert_equal {OK} [$rd read]
        assert_equal {} [$rd read]
        assert_equal {v} [$rd read]
        $rd close

        set histo [dict create {*}[r latency histogram e2e]]
        assert_match {calls 1 histogram_usec *} [dict get $histo set]
        assert_match {calls 1 histogram_usec *} [dict get $histo blpop]
        assert_match {calls 1 histogram_usec *} [dict get $histo get]

        # SET is fast (< 0.5s); BLPOP and GET span the ~3s wait (>= 2.5s).
        # Timing-sensitive: skip under environments that can't measure latency reliably.
        if {!$::no_latency} {
            assert {[max_histogram_usec [dict get $histo set]] < 500000}
            assert {[max_histogram_usec [dict get $histo blpop]] >= 2500000}
            assert {[max_histogram_usec [dict get $histo get]] >= 2500000}
        }
        r config set latency-tracking-features cmd
    }

    test {LATENCY HISTOGRAM E2E I/O threads} {
        try {
            r config set io-threads 2
            r config set io-threads-always-active yes
            r config set latency-tracking-features "cmd e2e"
            r config resetstat
            # Batch several commands so replies are drained on the I/O-thread write path.
            set rd [valkey_deferring_client]
            $rd set itk v
            $rd get itk
            $rd get itk
            $rd flush
            assert_equal {OK} [$rd read]
            assert_equal {v} [$rd read]
            assert_equal {v} [$rd read]
            $rd close
            set histo [dict create {*}[r latency histogram e2e]]
            assert_match {calls 1 histogram_usec *} [dict get $histo set]
            assert_match {calls 2 histogram_usec *} [dict get $histo get]
        } finally {
            # Always restore the I/O-thread config so a failure here doesn't leak into later tests.
            r config set latency-tracking-features cmd
            r config set io-threads-always-active no
            r config set io-threads 1
        }
    }

    test {LATENCY HISTOGRAM E2E MULTI/EXEC} {
        r config set latency-tracking-features "cmd e2e"
        r config resetstat
        set rd [valkey_deferring_client]
        $rd multi
        $rd set mtk v
        $rd get mtk
        $rd incr mtn
        $rd exec
        $rd flush
        assert_equal {OK} [$rd read]
        assert_equal {QUEUED} [$rd read]
        assert_equal {QUEUED} [$rd read]
        assert_equal {QUEUED} [$rd read]
        assert_equal {OK v 1} [$rd read]
        $rd close
        set histo [dict create {*}[r latency histogram e2e]]
        # Only EXEC is recorded.
        assert_match {calls 1 histogram_usec *} [dict get $histo exec]
        assert {![dict exists $histo set]}
        assert {![dict exists $histo get]}
        assert {![dict exists $histo incr]}
        assert {![dict exists $histo multi]}
        r config set latency-tracking-features cmd
    }

    test {LATENCY HISTOGRAM E2E skips CLIENT REPLY OFF and SKIP} {
        r config set latency-tracking-features "cmd e2e"
        r config resetstat
        set rd [valkey_deferring_client]
        # skip reply on the SET command
        $rd client reply skip
        $rd set skipk v
        $rd get skipk
        $rd flush
        assert_equal {v} [$rd read]
        # disable reply on client
        $rd client reply off
        $rd set offk v
        $rd incr offn
        $rd client reply on
        $rd flush
        assert_equal {OK} [$rd read]
        $rd close
        # Only the GET is recorded, SET/INCR are not.
        set histo [dict create {*}[r latency histogram e2e]]
        assert_match {calls 1 histogram_usec *} [dict get $histo get]
        assert {![dict exists $histo set]}
        assert {![dict exists $histo incr]}
        r config set latency-tracking-features cmd
    }

    test {LATENCY HISTOGRAM E2E on large multi-block reply} {
        r config set latency-tracking-features "cmd e2e"
        # A value large enough to spill the reply into several output blocks.
        set big [string repeat A 200000]
        r set bigk $big
        r config resetstat
        set rd [valkey_deferring_client]
        $rd get bigk
        $rd flush
        assert_equal $big [$rd read]
        $rd close
        set histo [dict create {*}[r latency histogram e2e]]
        assert_match {calls 1 histogram_usec *} [dict get $histo get]
        r config set latency-tracking-features cmd
    }

    test {LATENCY HISTOGRAM E2E records nothing for replicated writes on a replica} {
        start_server {} {
            set replica [srv 0 client]
            set primary [srv -1 client]
            set primary_host [srv -1 host]
            set primary_port [srv -1 port]

            $replica config set latency-tracking-features "cmd e2e"
            $replica replicaof $primary_host $primary_port
            wait_for_sync $replica
            $replica config resetstat

            $primary set rpk v
            $primary set rpk v2
            $primary incr rpn
            wait_for_condition 50 100 {
                [$replica get rpk] eq {v2}
            } else {
                fail "replica did not apply replicated writes"
            }

            set histo [dict create {*}[$replica latency histogram e2e]]
            assert {![dict exists $histo set]}
            assert {![dict exists $histo incr]}

            $replica replicaof no one
            $replica config set latency-tracking-features cmd
        }
    } {} {external:skip}

tags {"needs:debug"} {
    set old_threshold_value [lindex [r config get latency-monitor-threshold] 1]

    test {Test latency events logging} {
        r config set latency-monitor-threshold 200
        r latency reset
        r debug sleep 0.3
        after 1100
        r debug sleep 0.4
        after 1100
        r debug sleep 0.5
        r config set latency-monitor-threshold 0
        assert {[r latency history command] >= 3}
    }

    test {LATENCY HISTORY output is ok} {
        set res [r latency history command]
        if {$::verbose} {
            puts "LATENCY HISTORY data:"
            puts $res
        }

        set min 250
        set max 450
        foreach event $res {
            lassign $event time latency
            if {!$::no_latency} {
                assert {$latency >= $min && $latency <= $max}
            }
            incr min 100
            incr max 100
            set last_time $time ; # Used in the next test
        }
    }

    test {LATENCY LATEST output is ok} {
        set res [r latency latest]
        if {$::verbose} {
            puts "LATENCY LATEST data:"
            puts $res
        }

        # See the previous "Test latency events logging" test for each call.
        foreach event $res {
            lassign $event eventname time latency max sum cnt
            assert {$eventname eq "command"}
            if {!$::no_latency} {
                # To avoid timing issues, each event decreases by 50 and
                # increases by 200 to increase the range.
                assert_equal $time $last_time
                assert_range $max 450 700 ;# debug sleep 0.5
                assert_range $sum 1050 1800 ;# debug sleep 0.3 + 0.4 + 0.5
                assert_equal $cnt 3
            }
            break
        }
    }

    test {LATENCY GRAPH can output the event graph} {
        set res [r latency graph command]
        if {$::verbose} {
            puts "LATENCY GRAPH data:"
            puts $res
        }
        if {!$::no_latency} {
            assert_match {*command*high*low*} $res

            # These numbers are taken from the "Test latency events logging" test.
            # (debug sleep 0.3) and (debug sleep 0.5), using range to prevent timing issue.
            regexp "command - high (.*?) ms, low (.*?) ms" $res -> high low
            assert_range $high 450 700
            assert_range $low 250 500
        }
    }

    r config set latency-monitor-threshold $old_threshold_value
} ;# tag

    test {LATENCY of expire events are correctly collected} {
        r config set latency-monitor-threshold 1
        r config set lazyfree-lazy-expire no
        r flushdb
        if {$::valgrind} {set count 100000} else {set count 1000000}
        r eval {
            local i = 0
            while (i < tonumber(ARGV[1])) do
                redis.call('sadd',KEYS[1],i)
                i = i+1
             end
        } 1 mybigkey $count
        r pexpire mybigkey 50
        wait_for_condition 5 100 {
            [r dbsize] == 0
        } else {
            fail "key wasn't expired"
        }
        assert_match {*expire-cycle*} [r latency latest]

        test {LATENCY GRAPH can output the expire event graph} {
             assert_match {*expire-cycle*high*low*} [r latency graph expire-cycle]
        }

        r config set latency-monitor-threshold 200
        r config set lazyfree-lazy-expire yes
    }

    test {LATENCY HISTORY / RESET with wrong event name is fine} {
        assert {[llength [r latency history blabla]] == 0}
        assert {[r latency reset blabla] == 0}
    }

    test {LATENCY DOCTOR produces some output} {
        assert {[string length [r latency doctor]] > 0}
    }

    test {LATENCY RESET is able to reset events} {
        assert {[r latency reset] > 0}
        assert {[r latency latest] eq {}}
    }

    test {LATENCY HELP should not have unexpected options} {
        catch {r LATENCY help xxx} e
        assert_match "*wrong number of arguments for 'latency|help' command" $e
    }
}

start_cluster 1 1 {tags {"latency-monitor cluster external:skip needs:latency"} overrides {latency-monitor-threshold 1}} {
    test "Cluster config file latency" {
        # This test just a sanity test so that we can make sure the code path is cover.
        # We don't assert anything since we can't be sure whether it will be counted.
        R 0 cluster saveconfig
        R 1 cluster saveconfig
        R 1 cluster failover takeover
        R 0 latency latest
        R 1 latency latest
    }
}

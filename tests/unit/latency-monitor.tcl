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

    # Percentiles (as a dict, e.g. p50 -> usec) of the e2e_percentiles_usec_<kind> line
    # in INFO latencystats, or an empty dict if that kind has no samples.
    proc e2e_percentiles {kind {client r}} {
        set res {}
        if {[regexp "e2e_percentiles_usec_$kind:(\[^\r\n\]*)" [$client info latencystats] -> line]} {
            foreach kv [split $line ,] {
                lassign [split $kv =] k v
                dict set res $k $v
            }
        }
        return $res
    }

    proc e2e_has_samples {kind {client r}} {
        expr {[dict size [e2e_percentiles $kind $client]] > 0}
    }

    test {E2E latency is disabled by default} {
        # Default feature set is "cmd" only (processing time); e2e is off.
        assert_equal {cmd} [lindex [r config get latency-tracking-features] 1]
        r config resetstat
        r set stk v
        r get stk
        # Processing histograms still record; no e2e histograms or lines are reported.
        assert {[llength [r latency histogram]] > 0}
        assert_equal {} [r latency e2e_histogram]
        assert_no_match {*e2e_percentiles_usec_*} [r info latencystats]
    }

    test {E2E latency records samples by command kind} {
        r config set latency-tracking-features "cmd e2e"
        r config resetstat
        set rd [valkey_deferring_client]
        # One batch with a command of each kind.
        $rd set stk v
        $rd get stk
        $rd auth wrongpass
        $rd ping
        $rd flush
        # Drain the replies so the writes complete (and samples flush) before close.
        assert_equal {OK} [$rd read]
        assert_equal {v} [$rd read]
        catch {$rd read}
        assert_equal {PONG} [$rd read]
        $rd close
        foreach kind {write read auth other} {
            assert {[e2e_has_samples $kind]}
        }
        r config set latency-tracking-features cmd
    }

    test {LATENCY E2E_HISTOGRAM reports the histogram of every command kind with samples} {
        r config resetstat
        assert_equal {} [r latency e2e_histogram]
        r config set latency-tracking-features "cmd e2e"
        set rd [valkey_deferring_client]
        $rd set stk v
        $rd get stk
        $rd get stk
        $rd flush
        assert_equal {OK} [$rd read]
        assert_equal {v} [$rd read]
        assert_equal {v} [$rd read]
        $rd close
        set histogram [r latency e2e_histogram]
        assert {![dict exists $histogram auth]}
        assert_match {calls 1 histogram_usec *} [dict get $histogram write]
        assert_match {calls 2 histogram_usec *} [dict get $histogram read]
        # Buckets are cumulative: the last one holds every sample.
        assert_equal 2 [lindex [dict get $histogram read histogram_usec] end]
        assert_error {*wrong number of arguments*} {r latency e2e_histogram read}
        r config set latency-tracking-features cmd
    }

    test {E2E latency classifies HELLO by its AUTH option} {
        r config set latency-tracking-features "cmd e2e"
        r config resetstat
        set rd [valkey_deferring_client]
        $rd hello 2
        $rd read
        assert {![e2e_has_samples auth]}
        $rd hello 2 setname e2e auth default wrongpass
        catch {$rd read}
        assert {[e2e_has_samples auth]}
        $rd close
        r config set latency-tracking-features cmd
    }

    test {E2E latency measures end-to-end time including queue and block wait} {
        try {
            r config set latency-tracking-features "cmd e2e"
            r del stlist
            r config resetstat
            # One batch: a fast SET, a command that blocks ~0.6s, then a GET that is queued behind the blocking command
            set rd [valkey_deferring_client]
            $rd set stk v
            $rd blpop stlist 0.6
            $rd get stk
            $rd flush
            # Wait for all replies
            assert_equal {OK} [$rd read]
            assert_equal {} [$rd read]
            assert_equal {v} [$rd read]
            $rd close

            set histogram [r latency e2e_histogram]
            set write [dict keys [dict get $histogram write histogram_usec]]
            set read [dict keys [dict get $histogram read histogram_usec]]
            assert_match {calls 2 *} [dict get $histogram write]
            assert_match {calls 1 *} [dict get $histogram read]

            # SET is fast (< 0.1s); BLPOP (write) and GET (read) span the ~0.6s wait (>= 0.5s).
            # Timing-sensitive: skip under environments that can't measure latency reliably.
            if {!$::no_latency} {
                assert {[lindex $write 0] < 100000}
                assert {[lindex $write end] >= 500000}
                assert {[lindex $read 0] >= 500000}
            }
        } finally {
            r config set latency-tracking-features cmd
        }
    }

    test {E2E latency with I/O threads} {
        set old_percentiles [lindex [r config get latency-tracking-info-percentiles] 1]
        try {
            r config set io-threads 2
            r config set io-threads-always-active yes
            r config set latency-tracking-features "cmd e2e"
            r config set latency-tracking-info-percentiles "100"
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

            wait_for_condition 50 100 {
                [e2e_has_samples write] && [e2e_has_samples read]
            } else {
                fail "e2e samples were not recorded for I/O-thread writes"
            }

            # An idle period between requests is not included in the next request's sample
            after 1000
            $rd get itk
            assert_equal {v} [$rd read]
            $rd close
            if {!$::no_latency} {
                wait_for_condition 50 100 {
                    [dict get [e2e_percentiles read] p100] < 500000
                } else {
                    fail "e2e sample was held across an idle period: [e2e_percentiles read]"
                }
            }
        } finally {
            r config set latency-tracking-info-percentiles $old_percentiles
            r config set latency-tracking-features cmd
            r config set io-threads-always-active no
            r config set io-threads 1
        }
    }

    test {E2E latency records MULTI/EXEC once} {
        r config set latency-tracking-features "cmd e2e"
        r config resetstat
        set rd [valkey_deferring_client]
        $rd multi
        $rd set "{mt}k" v
        $rd get "{mt}k"
        $rd incr "{mt}n"
        $rd exec
        $rd flush
        assert_equal {OK} [$rd read]
        assert_equal {QUEUED} [$rd read]
        assert_equal {QUEUED} [$rd read]
        assert_equal {QUEUED} [$rd read]
        assert_equal {OK v 1} [$rd read]
        $rd close
        # Only EXEC (other) is recorded, not the queued write/read commands.
        assert {[e2e_has_samples other]}
        assert {![e2e_has_samples write]}
        assert {![e2e_has_samples read]}
        r config set latency-tracking-features cmd
    }

    test {E2E latency records commands of Pub/Sub clients} {
        r config set latency-tracking-features "cmd e2e"
        set rd [valkey_deferring_client]
        $rd subscribe e2e-chain
        assert_equal {subscribe e2e-chain 1} [$rd read]
        r config resetstat
        $rd ping
        $rd ping
        $rd ping
        $rd flush
        assert_equal {pong {}} [$rd read]
        assert_equal {pong {}} [$rd read]
        assert_equal {pong {}} [$rd read]
        $rd close
        # The 3 PINGs (other) of the subscribed client. Besides them, "other" holds the CONFIG
        # RESETSTAT of r and at most one sample per earlier poll, so at least 3 must be the PINGs.
        set polls 0
        wait_for_condition 50 100 {
            [incr polls] > 0 && [dict exists [set histogram [r latency e2e_histogram]] other] &&
            [dict get $histogram other calls] - $polls >= 3
        } else {
            fail "e2e samples were not recorded for a Pub/Sub client: [r latency e2e_histogram]"
        }
        r config set latency-tracking-features cmd
    }

    test {E2E latency skips CLIENT REPLY OFF and SKIP} {
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
        assert {[e2e_has_samples read]}
        assert {![e2e_has_samples write]}
        r config set latency-tracking-features cmd
    }

    test {E2E latency on large multi-block reply} {
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
        assert {[e2e_has_samples read]}
        r config set latency-tracking-features cmd
    }

    test {E2E latency samples are reset by CONFIG RESETSTAT} {
        r config set latency-tracking-features "cmd e2e"
        r get stk
        r get stk
        assert {[e2e_has_samples read]}
        r config resetstat
        assert {![e2e_has_samples read]}
        r config set latency-tracking-features cmd
    }

    test {E2E latency records nothing for replicated writes on a replica} {
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

            # WAIT makes the primary send GETACK, forcing the replica to write a REPLCONF ACK
            # on the replication link, which would flush any samples recorded on it.
            assert_equal 1 [$primary wait 1 5000]
            # The replication link is not a tracked client, so its writes are not recorded.
            assert {![e2e_has_samples write $replica]}

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

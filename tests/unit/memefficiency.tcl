proc test_memory_efficiency {range} {
    r flushall
    set rd [valkey_deferring_client]
    set base_mem [s used_memory]
    set written 0
    for {set j 0} {$j < 10000} {incr j} {
        set key key:$j
        set val [string repeat A [expr {int(rand()*$range)}]]
        $rd set $key $val
        incr written [string length $key]
        incr written [string length $val]
        incr written 2 ;# A separator is the minimum to store key-value data.
    }
    for {set j 0} {$j < 10000} {incr j} {
        $rd read ; # Discard replies
    }

    set current_mem [s used_memory]
    set used [expr {$current_mem-$base_mem}]
    set efficiency [expr {double($written)/$used}]
    return $efficiency
}

start_server {tags {"memefficiency external:skip"}} {
    foreach {size_range expected_min_efficiency} {
        32    0.15
        64    0.25
        128   0.35
        1024  0.75
        16384 0.82
    } {
        test "Memory efficiency with values in range $size_range" {
            set efficiency [test_memory_efficiency $size_range]
            assert {$efficiency >= $expected_min_efficiency}
        }
    }
}


run_solo {defrag} {
    # make logging a bit more readable
    proc to_mb {bytes} {
        return [format "%6.2f MB" [expr $bytes / 1024.0 / 1024.0]]
    }

    # Utility function used to create a sequence where each key has an increasing
    # number of fields/values.  For each key, the number for fields/values will
    # increase by 2x.  Key 0 will have 2 values, key 1 will have 4, key 2 will
    # have 8, etc.  Pass in the previous key/field pair to get the next.
    # The sequence is: (0,0), (0,1), (1,0), (1,1), (1,2), (1,3), (2,0)...
    # The optional 'incby' parameter allows incrementing by 2 across the sequence.
    proc next_exp_kf {key field {incby 1}} {
        assert {$incby == 1 || $incby == 2}
        incr field $incby
        if {$field == 2 ** ($key+1)} {
            incr key
            set field 0
        }
        return [list $key $field]
    }

    # Logs defragging state if ::verbose is true
    proc log_frag {title} {
        # Note, this delay is outside of the "if" so that behavior is the same, with and
        #  without verbose logging.  Also, this allows for assertions to follow without
        #  adding additional delay statements
        after 120  ;# serverCron only updates the info once in 100ms
        if {$::verbose} {
            puts "Frag info: $title"
            puts "    used: [to_mb [s allocator_allocated]]"
            puts "    frag: [to_mb [s allocator_frag_bytes]]"
            puts "    frag ratio: [s allocator_frag_ratio]"
            puts "    hits: [s active_defrag_hits]"
            puts "    misses: [s active_defrag_misses]"
        }
    }

    # Start defrag and wait for it to stop
    # The optional code block is executed after defrag has started
    proc perform_defrag {{code_block {}}} {
        r config set latency-monitor-threshold 5
        r latency reset

        set old_defrag_time [s total_active_defrag_time]
        r config set activedefrag yes

        # Wait for defrag to start
        wait_for_condition 10 50 {
            [s total_active_defrag_time] ne $old_defrag_time
        } else {
            log_frag "defrag not started"
            puts [r info memory]
            puts [r info stats]
            puts [r memory malloc-stats]
            fail "defrag not started."
        }

        # Execute the passed in code block
        uplevel 1 $code_block

        # Wait for the active defrag to stop working.
        wait_for_condition 150 200 {
            [s active_defrag_running] eq 0
        } else {
            log_frag "defrag didn't stop"
            puts [r info memory]
            puts [r memory malloc-stats]
            fail "defrag didn't stop."
        } debug {
            log_frag "defragging..."
        }

        r config set activedefrag no
        after 120 ;# ensure memory stats are current when function exits
    }

    # Checks for any significant latency events
    proc validate_latency {limit_ms} {
        if {!$::no_latency} {
            set max_latency 0
            foreach event [r latency latest] {
                lassign $event eventname time latency max
                if {$eventname == "active-defrag-cycle" || $eventname == "while-blocked-cron"} {
                    set max_latency $max
                }
            }
            if {$::verbose} {
                puts "Validating max latency ($max_latency) is LT $limit_ms"
                if {$max_latency > 0} {
                    puts [r latency latest]
                    puts [r latency history active-defrag-cycle]
                    puts [r latency history while-blocked-cron]
                }
            }
            assert {$max_latency <= $limit_ms}
        }
    }

    # Validate expected fragmentation ratio
    # op = "<" (less than) or ">" (greater than)
    proc validate_frag_ratio {op value} {
        set frag [s allocator_frag_ratio]
        if {$::verbose} {
            puts "Validating frag ($frag) $op $value"
        }
        set allocated_bytes [s allocator_allocated]
        if {$allocated_bytes < 20 * 1024 * 1024} {
            # If allocated bytes is too small, the ratios get wonky.  Since we use 2MB for
            # active-defrag-ignore-bytes, let's make sure that we have at least 10x that amount
            # allocated before trying to verify any fragmentation ratios.  Otherwise the tests
            # are likely to get flaky.
            error "test error: trying to validate frag ratio with only $allocated_bytes allocated"
        }
        if {$op == "<"} {
            assert_lessthan $frag $value
        } elseif {$op == ">"} {
            assert_morethan $frag $value
        } else {
            error "Operator value($op) must be '<' or '>'"
        }
    }

    # Performs a standardized defrag test.  The "populate" block generates data.  The "fragment" block
    # fragments that data (usually by deleting half).
    #  - caller must generate at least 40 MB of data
    #  - after fragmentation, at least 20 MB of data must remain, and fragmentation ratio must exceed 1.4
    # Positional parameters:
    #    name - name of the test
    # Named parameters:
    #    populate {code} - required, populates initial unfragmented data
    #    fragment {code} - required, fragments the populated data
    #    while_defragging {code} - optional, code executed after defrag has started
    #    latency <ms> - optional, verifies the latency to a ms target (default 5)
    proc perform_defrag_test {name args} {
        set opts(latency) 5
        set opts(while_defragging) {}
        array set opts $args
        assert {[info exists opts(populate)]}
        assert {[info exists opts(fragment)]}

        r config set active-defrag-threshold-lower 5
        r config set active-defrag-cycle-min 40
        r config set active-defrag-cycle-max 60
        r config set active-defrag-ignore-bytes 2500kb
        r config set maxmemory 0

        log_frag "test start ($name)"

        # at this point, we expect fragmentation BYTES to be below a threshold
        #  (percentage is wildly variable with low memory usage)
        assert {[s allocator_frag_bytes] < 2 * 1024 * 1024}

        # populate the DB with data
        set initial_allocated [s allocator_allocated]
        uplevel 1 $opts(populate)

        log_frag "after adding data"
        # check that enough data has been populated
        set required [expr 50 * 1024 * 1024]
        set allocated [expr [s allocator_allocated] - $initial_allocated]
        if {$allocated < $required} {
            fail "Tests are required to create at least $required bytes of data before fragmentation - only $allocated bytes created"
        }
        # check that at this point, fragmentation is very low
        validate_frag_ratio < 1.05

        # fragment the data
        uplevel 1 $opts(fragment)

        log_frag "after fragmenting data"
        # we want some fragmentation, but we still want a minimum allocation
        validate_frag_ratio > 1.4
        set required [expr 25 * 1024 * 1024]
        set allocated [expr [s allocator_allocated] - $initial_allocated]
        if {$allocated < $required} {
            fail "Tests are required to retain at least $required bytes of data after fragmentation - only $allocated bytes retained"
        }

        set digest [debug_digest]

        perform_defrag $opts(while_defragging)

        log_frag "after defragging"
        validate_frag_ratio < 1.1
        validate_latency $opts(latency)

        # verify the data isn't corrupted or changed
        set newdigest [debug_digest]
        assert {$digest eq $newdigest}
        r save ;# saving an rdb iterates over all the data / pointers
    }

    proc test_main_dictionary {type} {
        set title "Active Defrag main dictionary: $type"
        test $title {
            set n 200000

            perform_defrag_test $title populate {
                # add a mass of string keys
                set rd [valkey_deferring_client]
                for {set j 0} {$j < $n} {incr j} {
                    $rd setrange $j 250 a
                    if {$j % 3 == 0} {
                        $rd expire $j 1000 ;# put expiration on some
                    }
                }
                for {set j 0} {$j < $n} {incr j} {
                    $rd read ; # Discard replies
                    if {$j % 3 == 0} {
                        $rd read
                    }
                }
                assert {[scan [regexp -inline {expires\=([\d]*)} [r info keyspace]] expires=%d] > 0}
            } fragment {
                # delete half of the keys
                for {set j 0} {$j < $n} {incr j 2} {
                    $rd del $j
                }
                for {set j 0} {$j < $n} {incr j 2} { $rd read } ; # Discard replies
                $rd close
                
                # use custom slower defrag speed to start so that while_defragging has time
                r config set active-defrag-cycle-min 2
                r config set active-defrag-cycle-max 3
            } while_defragging {
                # This test runs for a while, during this interval, we verify changing the CPU range.
                set current_cpu [s active_defrag_running]
                assert_range $current_cpu 2 3

                # alter the CPU limits and check that they took effect
                r config set active-defrag-cycle-min 1
                r config set active-defrag-cycle-max 1
                after 120
                # it's possible that defrag has already completed
                assert {[s active_defrag_running] == 1}

                # now bump up the CPU to finish quickly
                r config set active-defrag-cycle-min 40
                r config set active-defrag-cycle-max 60
            }

            # in standalone mode, test AOF loading too (using the remaining data from the section above)
            if {$type eq "standalone"} {
            test "Active defrag - AOF loading" {
                # reset stats and load the AOF file
                r config resetstat
                r config set key-load-delay -25 ;# sleep on average 1/25 usec
                # Note: This test is checking if defrag is working DURING AOF loading (while
                #       timers are not active).  So we don't give any extra time, and we deactivate
                #       defrag immediately after the AOF loading is complete.  During loading,
                #       defrag will get invoked less often, causing starvation prevention.  We
                #       should expect longer latency measurements.
                r config set active-defrag-cycle-min 50
                r config set active-defrag-cycle-max 50
                r config set activedefrag yes
                r debug loadaof
                r config set activedefrag no
                after 50 ;# give defrag time to stop

                log_frag "after AOF loading"

                # The AOF contains simple (fast) SET commands (and the cron during loading runs every 1024 commands).
                # Even so, defrag can get starved for periods exceeding 100ms.  Using 200ms for test stability, and
                # a 50% CPU requirement, we should allow up to 200ms latency
                # (as total time = 200 non duty + 200 duty = 400ms, and 50% of 400ms is 200ms).
                validate_latency 200

                # Make sure we had defrag hits during AOF loading.  Note that we don't worry about
                # the actual fragmentation ratio here.  It will vary based on when defrag stopped
                # mid-cycle.  Just check that we are defragging by the number of hits.
                assert {[s active_defrag_hits] > 100000}
            }
            } ;# Active defrag - AOF loading
        }
    }

    proc test_eval_scripts {type} {
        set title "Active Defrag eval scripts: $type"
        test $title {
            set n 50000

            # scripts aren't defragged incrementally, expect big latency
            perform_defrag_test $title latency 200 populate {
                # Populate memory with interleaving script-key pattern of same size
                set dummy_script "--[string repeat x 450]\nreturn "
                set rd [valkey_deferring_client]
                for {set j 0} {$j < $n} {incr j} {
                    set val "$dummy_script[format "%06d" $j]"
                    $rd script load $val
                    $rd set k$j $val
                }
                for {set j 0} {$j < $n} {incr j} {
                    $rd read ; # Discard script load replies
                    $rd read ; # Discard set replies
                }
            } fragment {
                # Delete all the keys to create fragmentation
                for {set j 0} {$j < $n} {incr j} { $rd del k$j }
                for {set j 0} {$j < $n} {incr j} { $rd read } ; # Discard del replies
                $rd close
            }
        }
    }

    proc test_big_hash {type} {
        set title "Active Defrag big hash: $type"
        test $title {
            # number of total fields.  hashes are progressively increasing sizes.
            set n 200000

            perform_defrag_test $title populate {
                set rd [valkey_deferring_client]
                set val [string repeat A 250]
                set k 0
                set f 0
                for {set j 0} {$j < $n} {incr j} {
                    $rd hset k$k f$f $val
                    lassign [next_exp_kf $k $f] k f
                }
                for {set j 0} {$j < $n} {incr j} { $rd read } ; # Discard replies
            } fragment {
                set k 0
                set f 0
                for {set j 0} {$j < $n} {incr j 2} {
                    $rd hdel k$k f$f
                    lassign [next_exp_kf $k $f 2] k f
                }
                for {set j 0} {$j < $n} {incr j 2} { $rd read } ; # Discard replies
                $rd close
            }
        }
    }

    proc test_big_list {type} {
        set title "Active Defrag big list: $type"
        test $title {
            r config set list-max-listpack-size 3 ;# only 3 items per listpack node to generate more frag
            # number of total fields.  lists are progressively increasing sizes.
            set n 200000

            perform_defrag_test $title populate {
                set rd [valkey_deferring_client]
                set val [string repeat A 350]
                set k 0
                set f 0
                for {set j 0} {$j < $n} {incr j} {
                    $rd lpush k$k $val
                    lassign [next_exp_kf $k $f] k f
                }
                for {set j 0} {$j < $n} {incr j} { $rd read } ; # Discard replies
            } fragment {
                set k 0
                set f 0
                for {set j 0} {$j < $n} {incr j 2} {
                    $rd ltrim k$k 1 -1 ;# deletes the leftmost item
                    $rd lmove k$k k$k LEFT RIGHT ;# rotates the leftmost item to the right side
                    lassign [next_exp_kf $k $f 2] k f
                }
                for {set j 0} {$j < $n} {incr j 2} { $rd read; $rd read } ; # Discard replies
                $rd close
            }
        }
    }

    proc test_big_set {type} {
        set title "Active Defrag big set: $type"
        test $title {
            # number of total fields.  sets are progressively increasing sizes.
            set n 200000

            perform_defrag_test $title populate {
                set rd [valkey_deferring_client]
                set val [string repeat A 300]
                set k 0
                set f 0
                for {set j 0} {$j < $n} {incr j} {
                    $rd sadd k$k $val$f
                    lassign [next_exp_kf $k $f] k f
                }
                for {set j 0} {$j < $n} {incr j} { $rd read } ; # Discard replies
            } fragment {
                set k 0
                set f 0
                for {set j 0} {$j < $n} {incr j 2} {
                    $rd srem k$k $val$f
                    lassign [next_exp_kf $k $f 2] k f
                }
                for {set j 0} {$j < $n} {incr j 2} { $rd read } ; # Discard replies
                $rd close
            }
        }
    }

    proc test_big_zset {type} {
        set title "Active Defrag big zset: $type"
        test $title {
            # number of total fields.  zsets are progressively increasing sizes.
            set n 200000

            perform_defrag_test $title populate {
                set rd [valkey_deferring_client]
                set val [string repeat A 250]
                set k 0
                set f 0
                for {set j 0} {$j < $n} {incr j} {
                    $rd zadd k$k [expr rand()] $val$f
                    lassign [next_exp_kf $k $f] k f
                }
                for {set j 0} {$j < $n} {incr j} { $rd read } ; # Discard replies
            } fragment {
                set k 0
                set f 0
                for {set j 0} {$j < $n} {incr j 2} {
                    $rd zrem k$k $val$f
                    lassign [next_exp_kf $k $f 2] k f
                }
                for {set j 0} {$j < $n} {incr j 2} { $rd read } ; # Discard replies
                $rd close
            }
        }
    }

    proc test_stream {type} {
        set title "Active Defrag stream: $type"
        test $title {
            set n 200000
            # shrink the node size - it's hard to create frag with 4k nodes!
            r config set stream-node-max-bytes 128

            perform_defrag_test $title populate {
                set rd [valkey_deferring_client]
                set val [string repeat A 50]
                for {set j 0} {$j < $n} {incr j} {
                    $rd xadd k$j * field1 $val field2 $val
                }
                for {set j 0} {$j < $n} {incr j} { $rd read } ; # Discard replies
            } fragment {
                for {set j 0} {$j < $n} {incr j 2} {
                    $rd del k$j
                }
                for {set j 0} {$j < $n} {incr j 2} { $rd read } ; # Discard replies
                $rd close
            }
        }
    }

    proc test_pubsub {type} {
        set title "Active Defrag pubsub: $type"
        test $title {
            set n 100000

            set rd [valkey_deferring_client]
            set chan [string repeat A 100]

            # https://github.com/valkey-io/valkey/issues/1774
            # NOTE - pubsub defrag isn't working properly.  This wasn't caught before the
            #  test refactor.  This commented out code should work.  Instead, is substituted
            # code which verifies that defrag doesn't break the pubsub, but defrag efficacy
            # is not verified.
            # perform_defrag_test $title {
            #     for {set j 0} {$j < $n} {incr j} {
            #         set channel $chan$j
            #         $rd subscribe $channel
            #         $rd read ; # Discard subscribe replies
            #         $rd ssubscribe $channel
            #         $rd read ; # Discard ssubscribe replies
            #     }
            # } fragment {
            #     for {set j 0} {$j < $n} {incr j 2} {
            #         set channel $chan$j
            #         $rd unsubscribe $channel
            #         $rd read
            #         $rd sunsubscribe $channel
            #         $rd read
            #     }
            # }
            # --- BEGIN TEMPORARY CODE ---
            log_frag "test start (pubsub)"
            for {set j 0} {$j < $n} {incr j} {
                set channel $chan$j
                $rd subscribe $channel
                $rd read ; # Discard subscribe replies
                $rd ssubscribe $channel
                $rd read ; # Discard ssubscribe replies
            }
            log_frag "after adding data"
            for {set j 0} {$j < $n} {incr j 2} {
                set channel $chan$j
                $rd unsubscribe $channel
                $rd read
                $rd sunsubscribe $channel
                $rd read
            }
            log_frag "after fragmenting data"
            # run defrag for a limited time without validating efficacy
            r config set activedefrag yes
            after 30000
            r config set activedefrag no
            log_frag "after 30 seconds of defrag"
            after 120
            # --- END TEMPORARY CODE ---

            # now validate that the remaining channels still work
            for {set j 1} {$j < $n} {incr j 2} {
                set channel $chan$j
                r publish $channel "hello$j"
                assert_equal "message $channel hello$j" [$rd read] 
                $rd unsubscribe $channel
                $rd read
                r spublish $channel "shello$j"
                assert_equal "smessage $channel shello$j" [$rd read] 
                $rd sunsubscribe $channel
                $rd read
            }

            $rd close
        }
    }


    set standalone_tags [list defrag external:skip standalone]
    set cluster_tags [list defrag external:skip cluster]
    set aof_overrides [list appendonly yes auto-aof-rewrite-percentage 0 save "" lazyfree-lazy-user-del no]
    set std_overrides [list appendonly no save "" lazyfree-lazy-user-del no]

    set tests {}
    lappend tests [list test_main_dictionary standalone $aof_overrides] ;# only need AOF for main dict, standalone
    lappend tests [list test_eval_scripts standalone $std_overrides]
    lappend tests [list test_big_hash standalone $std_overrides]
    lappend tests [list test_big_list standalone $std_overrides]
    lappend tests [list test_big_set standalone $std_overrides]
    lappend tests [list test_big_zset standalone $std_overrides]
    lappend tests [list test_stream standalone $std_overrides]
    lappend tests [list test_pubsub standalone $std_overrides]

    lappend tests [list test_main_dictionary cluster $std_overrides]
    lappend tests [list test_eval_scripts cluster $std_overrides]
    lappend tests [list test_big_hash cluster $std_overrides]
    lappend tests [list test_big_list cluster $std_overrides]
    lappend tests [list test_big_set cluster $std_overrides]
    lappend tests [list test_big_zset cluster $std_overrides]
    lappend tests [list test_stream cluster $std_overrides]
    lappend tests [list test_pubsub cluster $std_overrides]

    # set ::verbose 1

    set have_defrag 0
    start_server [list tags $standalone_tags overrides $std_overrides] {
        if {[string match {*jemalloc*} [s mem_allocator]] && [r debug mallctl arenas.page] <= 8192} {
            set have_defrag 1
        }
    }

    if {$have_defrag} {
        foreach t $tests {
            lassign $t test_proc type overrides
            if {$type == "standalone"} {
                start_server [list tags $standalone_tags overrides $overrides] {
                    $test_proc $type
                }
            }
            if {$type == "cluster"} {
                start_cluster 1 0 [list tags $cluster_tags overrides $overrides] {
                    # Note: `start_cluster` passes the code through to another level which requires us
                    #  to do an uplevel here.  Otherwise `test_proc` isn't recognized.
                    uplevel 1 {$test_proc $type}
                }
            }
        }
    } else {
        puts "Jemalloc not available.  Defrag tests skipped."
    }
} ;# run_solo

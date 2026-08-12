start_server {tags {"commandlog"} overrides {commandlog-execution-slower-than 1000000 commandlog-request-larger-than 1048576 commandlog-reply-larger-than 1048576}} {
    test {COMMANDLOG - check that it starts with an empty log} {
        if {$::external} {
            r commandlog reset slow
            r commandlog reset large-request
            r commandlog reset large-reply
        }
        assert_equal [r commandlog len slow] 0
        assert_equal [r commandlog len large-request] 0
        assert_equal [r commandlog len large-reply] 0
    }

    test {COMMANDLOG - only logs commands exceeding the threshold} {
        # for slow
        r config set commandlog-execution-slower-than 100000
        r ping
        assert_equal [r commandlog len slow] 0
        r debug sleep 0.2
        assert_equal [r commandlog len slow] 1

        # for large-request
        r config set commandlog-request-larger-than 1024
        r ping
        assert_equal [r commandlog len large-request] 0
        set value [string repeat A 1024]
        r set testkey $value
        assert_equal [r commandlog len large-request] 1

        r config set commandlog-reply-larger-than 1024
        r ping
        assert_equal [r commandlog len large-reply] 0
        r get testkey
        assert_equal [r commandlog len large-reply] 1
    } {} {needs:debug}

    test {COMMANDLOG - zero max length is correctly handled} {
        r commandlog reset slow
        r commandlog reset large-request
        r commandlog reset large-reply
        r config set commandlog-slow-execution-max-len 0
        r config set commandlog-execution-slower-than 0
        r config set commandlog-large-request-max-len 0
        r config set commandlog-request-larger-than 0
        r config set commandlog-large-reply-max-len 0
        r config set commandlog-reply-larger-than 0
        for {set i 0} {$i < 100} {incr i} {
            r ping
        }
        assert_equal [r commandlog len slow] 0
        assert_equal [r commandlog len large-request] 0
        assert_equal [r commandlog len large-reply] 0
    }

    test {COMMANDLOG - max entries is correctly handled} {
        r config set commandlog-execution-slower-than 0
        r config set commandlog-slow-execution-max-len 10
        r config set commandlog-large-request-max-len 10
        r config set commandlog-request-larger-than 0
        r config set commandlog-large-reply-max-len 10
        r config set commandlog-reply-larger-than 0
        for {set i 0} {$i < 100} {incr i} {
            r ping
        }
        assert_equal [r commandlog len slow] 10
        assert_equal [r commandlog len large-request] 10
        assert_equal [r commandlog len large-reply] 10
    }

    test {COMMANDLOG - GET optional argument to limit output len works} {
        assert_equal 5  [llength [r commandlog get 5 slow]]
        assert_equal 10 [llength [r commandlog get -1 slow]]
        assert_equal 10 [llength [r commandlog get 20 slow]]

        assert_equal 5  [llength [r commandlog get 5 large-request]]
        assert_equal 10 [llength [r commandlog get -1 large-request]]
        assert_equal 10 [llength [r commandlog get 20 large-request]]

        assert_equal 5  [llength [r commandlog get 5 large-reply]]
        assert_equal 10 [llength [r commandlog get -1 large-reply]]
        assert_equal 10 [llength [r commandlog get 20 large-reply]]
    }

    test {COMMANDLOG - RESET subcommand works} {
        r config set commandlog-execution-slower-than 100000
        r config set commandlog-request-larger-than 1024
        r config set commandlog-reply-larger-than 1024
        r commandlog reset slow
        r commandlog reset large-request
        r commandlog reset large-reply
        assert_equal [r commandlog len slow] 0
        assert_equal [r commandlog len large-request] 0
        assert_equal [r commandlog len large-reply] 0
    }

    test {COMMANDLOG - logged entry sanity check} {
        r client setname foobar

        # for slow
        r debug sleep 0.2
        set e [lindex [r commandlog get -1 slow] 0]
        assert_equal [llength $e] 6
        if {!$::external} {
            assert_equal [lindex $e 0] 118
        }
        assert_equal [expr {[lindex $e 2] > 100000}] 1
        assert_equal [lindex $e 3] {debug sleep 0.2}
        assert_equal {foobar} [lindex $e 5]

        # for large-request
        set value [string repeat A 1024]
        r set testkey $value
        set e [lindex [r commandlog get -1 large-request] 0]
        assert_equal [llength $e] 6
        if {!$::external} {
            assert_equal [lindex $e 0] 118
        }
        assert_equal [expr {[lindex $e 2] > 1024}] 1
        assert_equal [lindex $e 3] {set testkey {AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA... (896 more bytes)}}
        assert_equal {foobar} [lindex $e 5]

        # for large-reply - without reply copy avoidance
        set copy_avoid [lindex [r config get min-string-size-avoid-copy-reply] 1]
        if {!$::external} {
            assert_morethan $copy_avoid 1024
        }
        r get testkey
        set e [lindex [r commandlog get -1 large-reply] 0]
        assert_equal [llength $e] 6
        if {!$::external} {
            assert_equal [lindex $e 0] 117
        }
        assert_equal [expr {[lindex $e 2] > 1024}] 1
        assert_equal [lindex $e 3] {get testkey}
        assert_equal {foobar} [lindex $e 5]

        # for large-reply - with reply copy avoidance
        # set min-string-size-avoid-copy-reply to 1 so wo will use reply copy avoidance
        r config set min-string-size-avoid-copy-reply 1
        r get testkey
        set e [lindex [r commandlog get -1 large-reply] 0]
        assert_equal [llength $e] 6
        if {!$::external} {
            assert_equal [lindex $e 0] 118
        }
        assert_equal [expr {[lindex $e 2] > 1024}] 1
        assert_equal [lindex $e 3] {get testkey}
        assert_equal {foobar} [lindex $e 5]
        # Restore min-string-size-avoid-copy-reply value
        r config set min-string-size-avoid-copy-reply $copy_avoid
    } {OK} {needs:debug}

    test {COMMANDLOG slow - Certain commands are omitted that contain sensitive information} {
        r config set commandlog-slow-execution-max-len 100
        r config set commandlog-execution-slower-than 0
        r commandlog reset slow
        catch {r acl setuser "slowlog test user" +get +set} _
        r config set primaryuser ""
        r config set primaryauth ""
        r config set requirepass ""
        r config set tls-key-file-pass ""
        r config set tls-client-key-file-pass ""
        r acl setuser slowlog-test-user +get +set
        r acl getuser slowlog-test-user
        r acl deluser slowlog-test-user non-existing-user
        r config set commandlog-execution-slower-than 0
        r config set commandlog-execution-slower-than -1
        set slowlog_resp [r commandlog get -1 slow]

        # Make sure normal configs work, but the two sensitive
        # commands are omitted or redacted
        assert_equal 11 [llength $slowlog_resp]
        assert_equal {commandlog reset slow} [lindex [lindex $slowlog_resp 10] 3]
        assert_equal {acl setuser (redacted) (redacted) (redacted)} [lindex [lindex $slowlog_resp 9] 3]
        assert_equal {config set primaryuser (redacted)} [lindex [lindex $slowlog_resp 8] 3]
        assert_equal {config set primaryauth (redacted)} [lindex [lindex $slowlog_resp 7] 3]
        assert_equal {config set requirepass (redacted)} [lindex [lindex $slowlog_resp 6] 3]
        assert_equal {config set tls-key-file-pass (redacted)} [lindex [lindex $slowlog_resp 5] 3]
        assert_equal {config set tls-client-key-file-pass (redacted)} [lindex [lindex $slowlog_resp 4] 3]
        assert_equal {acl setuser (redacted) (redacted) (redacted)} [lindex [lindex $slowlog_resp 3] 3]
        assert_equal {acl getuser (redacted)} [lindex [lindex $slowlog_resp 2] 3]
        assert_equal {acl deluser (redacted) (redacted)} [lindex [lindex $slowlog_resp 1] 3]
        assert_equal {config set commandlog-execution-slower-than 0} [lindex [lindex $slowlog_resp 0] 3]
    } {} {needs:repl}

    test {COMMANDLOG slow - Some commands can redact sensitive fields} {
        r config set commandlog-execution-slower-than 0
        r commandlog reset slow
        r migrate [srv 0 host] [srv 0 port] key 9 5000
        r migrate [srv 0 host] [srv 0 port] key 9 5000 AUTH user
        r migrate [srv 0 host] [srv 0 port] key 9 5000 AUTH2 user password
        r config set commandlog-execution-slower-than -1
        set slowlog_resp [r commandlog get -1 slow]

        # Make sure all 3 commands were logged, but the sensitive fields are omitted
        assert_equal 4 [llength $slowlog_resp]
        assert_match {* key 9 5000} [lindex [lindex $slowlog_resp 2] 3]
        assert_match {* key 9 5000 AUTH (redacted)} [lindex [lindex $slowlog_resp 1] 3]
        assert_match {* key 9 5000 AUTH2 (redacted) (redacted)} [lindex [lindex $slowlog_resp 0] 3]
    } {} {needs:repl}

    test {COMMANDLOG slow - Rewritten commands are logged as their original command} {
        r config set commandlog-execution-slower-than 0

        # Test rewriting client arguments
        r sadd set a b c d e
        r commandlog reset slow

        # SPOP is rewritten as DEL when all keys are removed
        r spop set 10
        assert_equal {spop set 10} [lindex [lindex [r commandlog get -1 slow] 0] 3]

        # Test replacing client arguments
        r commandlog reset slow

        # GEOADD is replicated as ZADD
        r geoadd cool-cities -122.33207 47.60621 Seattle
        assert_equal {geoadd cool-cities -122.33207 47.60621 Seattle} [lindex [lindex [r commandlog get -1 slow] 0] 3]

        # Test replacing a single command argument
        r set A 5
        r commandlog reset slow
        
        # GETSET is replicated as SET
        r getset a 5
        assert_equal {getset a 5} [lindex [lindex [r commandlog get -1 slow] 0] 3]

        # INCRBYFLOAT calls rewrite multiple times, so it's a special case
        r set A 0
        r commandlog reset slow
        
        # INCRBYFLOAT is replicated as SET
        r INCRBYFLOAT A 1.0
        assert_equal {INCRBYFLOAT A 1.0} [lindex [lindex [r commandlog get -1 slow] 0] 3]

        # blocked BLPOP is replicated as LPOP
        set rd [valkey_deferring_client]
        $rd blpop l 0
        wait_for_blocked_clients_count 1 50 100
        r multi
        r lpush l foo
        r commandlog reset slow
        r exec
        $rd read
        $rd close
        assert_equal {blpop l 0} [lindex [lindex [r commandlog get -1 slow] 0] 3]
    }

    test {COMMANDLOG slow - commands with too many arguments are trimmed} {
        r config set commandlog-execution-slower-than 0
        r commandlog reset slow
        r sadd set 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33
        set e [lindex [r commandlog get -1 slow] end-1]
        lindex $e 3
    } {sadd set 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 {... (2 more arguments)}}

    test {COMMANDLOG slow - too long arguments are trimmed} {
        r config set commandlog-execution-slower-than 0
        r commandlog reset slow
        set arg [string repeat A 129]
        r sadd set foo $arg
        set e [lindex [r commandlog get -1 slow] end-1]
        lindex $e 3
    } {sadd set foo {AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA... (1 more bytes)}}

    test {COMMANDLOG slow - EXEC is not logged, just executed commands} {
        r config set commandlog-execution-slower-than 100000
        r commandlog reset slow
        assert_equal [r commandlog len slow] 0
        r multi
        r debug sleep 0.2
        r exec
        assert_equal [r commandlog len slow] 1
        set e [lindex [r commandlog get -1 slow] 0]
        assert_equal [lindex $e 3] {debug sleep 0.2}
    } {} {needs:debug}

    test {COMMANDLOG slow - can clean older entries} {
        r client setname lastentry_client
        r config set commandlog-slow-execution-max-len 1
        r debug sleep 0.2
        assert {[llength [r commandlog get -1 slow]] == 1}
        set e [lindex [r commandlog get -1 slow] 0]
        assert_equal {lastentry_client} [lindex $e 5]
    } {} {needs:debug}

    test {COMMANDLOG slow - can be disabled} {
        r config set commandlog-slow-execution-max-len 1
        r config set commandlog-execution-slower-than 1
        r commandlog reset slow
        r debug sleep 0.2
        assert_equal [r commandlog len slow] 1
        r config set commandlog-execution-slower-than -1
        r commandlog reset slow
        r debug sleep 0.2
        assert_equal [r commandlog len slow] 0
    } {} {needs:debug}

    test {COMMANDLOG slow - count must be >= -1} {
       assert_error "ERR count should be greater than or equal to -1" {r commandlog get -2 slow}
       assert_error "ERR count should be greater than or equal to -1" {r commandlog get -222 slow}
    }

    test {COMMANDLOG slow - get all slow logs} {
        r config set commandlog-execution-slower-than 0
        r config set commandlog-slow-execution-max-len 3
        r commandlog reset slow

        r set key test
        r sadd set a b c
        r incr num
        r lpush list a

        assert_equal [r commandlog len slow] 3
        assert_equal 0 [llength [r commandlog get 0 slow]]
        assert_equal 1 [llength [r commandlog get 1 slow]]
        assert_equal 3 [llength [r commandlog get -1 slow]]
        assert_equal 3 [llength [r commandlog get 3 slow]]
    }
    
     test {COMMANDLOG slow - blocking command is reported only after unblocked} {
        # Cleanup first
        r del mylist
        # create a test client
        set rd [valkey_deferring_client]
        
        # config the slowlog and reset
        r config set commandlog-execution-slower-than 0
        r config set commandlog-slow-execution-max-len 110
        r commandlog reset slow
        
        $rd BLPOP mylist 0
        wait_for_blocked_clients_count 1 50 20
        assert_equal 0 [llength [regexp -all -inline (?=BLPOP) [r commandlog get -1 slow]]]
        
        r LPUSH mylist 1
        wait_for_blocked_clients_count 0 50 20
        assert_equal 1 [llength [regexp -all -inline (?=BLPOP) [r commandlog get -1 slow]]]
        
        $rd close
    }

    foreach is_eval {0 1} {
        test "COMMANDLOG slow - the commands in script are recorded normally - is_eval: $is_eval" {
            if {$is_eval == 0} {
                r function load replace "#!lua name=mylib \n redis.register_function('myfunc', function(KEYS, ARGS) server.call('ping') end)"
            }

            r client setname test-client
            r config set commandlog-execution-slower-than 0
            r commandlog reset slow

            if {$is_eval} {
                r eval "server.call('ping')" 0
            } else {
                r fcall myfunc 0
            }
            set slowlog_resp [r commandlog get 2 slow]
            assert_equal 2 [llength $slowlog_resp]

            # The first one is the script command, and the second one is the ping command executed in the script
            # Each slowlog contains: id, timestamp, execution time, command array, ip:port, client name
            set script_cmd [lindex $slowlog_resp 0]
            set ping_cmd [lindex $slowlog_resp 1]

            # Make sure the command are logged.
            if {$is_eval} {
                assert_equal {eval server.call('ping') 0} [lindex $script_cmd 3]
            } else {
                assert_equal {fcall myfunc 0} [lindex $script_cmd 3]
            }
            assert_equal {ping} [lindex $ping_cmd 3]

            # Make sure the client info are the logged.
            assert_equal [lindex $script_cmd 4] [lindex $ping_cmd 4]
            assert_equal {test-client} [lindex $script_cmd 5]
            assert_equal {test-client} [lindex $ping_cmd 5]
        }
    }

    test {COMMANDLOG large-reply - byte tracking with copy avoidance} {
        set copy_avoid [lindex [r config get min-string-size-avoid-copy-reply] 1]
        r config set min-string-size-avoid-copy-reply 1
        
        # Disable reply tracking
        r config set commandlog-reply-larger-than -1
        r commandlog reset large-reply
        
        set value [string repeat A 2048]
        r set testkey $value
        
        # Should not be logged
        r get testkey
        assert_equal [r commandlog len large-reply] 0
        
        # Enable tracking
        r config set commandlog-reply-larger-than 1024
        r commandlog reset large-reply
        
        # Get the value again, should be tracked
        r get testkey
        assert_equal [r commandlog len large-reply] 1
        set e [lindex [r commandlog get -1 large-reply] 0]
        assert_equal [lindex $e 3] {get testkey}
        # For 2048 bytes: $2048\r\n<data>\r\n = 2057
        assert_equal [lindex $e 2] 2057
        
        # Cleanup
        r config set min-string-size-avoid-copy-reply $copy_avoid
        r del testkey
    }

    test {COMMANDLOG - memory config with set / get / rewrite} {
        r config set commandlog-request-larger-than 10mb
        r config set commandlog-reply-larger-than 10mb
        assert_equal [r config get commandlog-request-larger-than] {commandlog-request-larger-than 10485760}
        assert_equal [r config get commandlog-reply-larger-than] {commandlog-reply-larger-than 10485760}

        r config rewrite
        restart_server 0 true false
        assert_equal [lindex [r config get commandlog-request-larger-than] 1] 10485760
        assert_equal [lindex [r config get commandlog-reply-larger-than] 1] 10485760
    } {} {external:skip}

    test {COMMANDLOG - special number -1 disables the command logging} {
        r config set commandlog-execution-slower-than -1
        r config set commandlog-request-larger-than -1
        r config set commandlog-reply-larger-than -1
        assert_error {*argument must be between -1 and *} {r config set commandlog-execution-slower-than -2}
        assert_error {*argument must be between -1 and *} {r config set commandlog-request-larger-than -2}
        assert_error {*argument must be between -1 and *} {r config set commandlog-reply-larger-than -2}

        r commandlog reset slow
        r commandlog reset large-request
        r commandlog reset large-reply

        r ping
        assert_equal [r commandlog len slow] 0
        assert_equal [r commandlog len large-request] 0
        assert_equal [r commandlog len large-reply] 0

        r config rewrite
        restart_server 0 true false
        assert_equal [lindex [r config get commandlog-execution-slower-than] 1] -1
        assert_equal [lindex [r config get commandlog-request-larger-than] 1] -1
        assert_equal [lindex [r config get commandlog-reply-larger-than] 1] -1
    } {} {external:skip}
}

start_server {tags {"commandlog"} overrides {commandlog-execution-slower-than 10000 commandlog-slow-execution-max-len 128 commandlog-slow-execution-magnitude-max-len 5}} {

    test {Magnitude store - captures commands below the recency threshold} {
        r commandlog reset slow
        for {set i 0} {$i < 10} {incr i} {
            r set key$i val$i
        }
        assert_equal 0 [r commandlog len slow recency]
        assert_equal 5 [r commandlog len slow magnitude]
    }

    test {Magnitude store - bounded by its own max-len} {
        r commandlog reset slow
        for {set i 0} {$i < 100} {incr i} {
            r set key$i val$i
        }
        assert_equal 5 [r commandlog len slow magnitude]
    }

    test {Magnitude store - entries are returned highest value first} {
        r commandlog reset slow
        for {set i 0} {$i < 50} {incr i} {
            r set key$i val$i
        }
        set entries [r commandlog get -1 slow magnitude]
        assert_equal 5 [llength $entries]
        set prev [lindex [lindex $entries 0] 2]
        foreach entry $entries {
            set value [lindex $entry 2]
            assert {$value <= $prev}
            set prev $value
        }
    }

    test {Magnitude store - a slow command displaces faster ones} {
        r commandlog reset slow
        for {set i 0} {$i < 50} {incr i} {
            r set key$i val$i
        }
        r debug sleep 0.01
        # The sleep is the slowest command by far, so it must be the top entry.
        set top [lindex [r commandlog get 1 slow magnitude] 0]
        assert {[lindex $top 2] > 5000}
    } {} {needs:debug}

    test {Magnitude store - keeps only the worst entries once full} {
        r commandlog reset slow
        for {set i 0} {$i < 5} {incr i} {
            r debug sleep 0.01
        }
        # Heap is full of 10ms sleeps. Fast commands must all be rejected.
        for {set i 0} {$i < 50} {incr i} {
            r set key$i val$i
        }
        foreach entry [r commandlog get -1 slow magnitude] {
            assert {[lindex $entry 2] > 5000}
        }
    } {} {needs:debug}

    test {Both stores are populated together} {
        r config set commandlog-execution-slower-than 0
        r commandlog reset slow
        for {set i 0} {$i < 10} {incr i} {
            r set key$i val$i
        }
        # Recency keeps up to 128, magnitude up to 5.
        assert {[r commandlog len slow recency] >= 10}
        assert_equal 5 [r commandlog len slow magnitude]
        r config set commandlog-execution-slower-than 10000
    }

    test {LEN and GET default to the recency store} {
        r config set commandlog-execution-slower-than 0
        r commandlog reset slow
        r set foo bar
        # Restore the threshold first, otherwise each command below is logged
        r config set commandlog-execution-slower-than 10000
        assert_equal [r commandlog len slow] [r commandlog len slow recency]
        assert_equal [r commandlog get -1 slow] [r commandlog get -1 slow recency]
    }

    test {RESET without a store clears both} {
        r config set commandlog-execution-slower-than 0
        for {set i 0} {$i < 10} {incr i} {
            r set key$i val$i
        }
        r debug sleep 0.01
        r config set commandlog-execution-slower-than 10000
        assert {[r commandlog len slow recency] > 0}
        assert_equal 5 [r commandlog len slow magnitude]

        r commandlog reset slow
        # The magnitude store will log the reset command
        set entries [r commandlog get -1 slow magnitude]
        assert_equal 1 [llength $entries]
        assert {[lindex [lindex $entries 0] 2] < 5000}
        assert_equal 0 [r commandlog len slow recency]
    } {} {needs:debug}

    test {RESET with a store clears only that one} {
        r config set commandlog-execution-slower-than 0
        r commandlog reset slow
        for {set i 0} {$i < 10} {incr i} {
            r set key$i val$i
        }
        r debug sleep 0.01
        r config set commandlog-execution-slower-than 10000

        # 1 reset + 10 sets + the sleep
        assert_equal 12 [r commandlog len slow recency]
        assert_equal 5 [r commandlog len slow magnitude]
        assert {[lindex [r commandlog get 1 slow magnitude] 0 2] > 5000}

        # Resetting only the recency
        r commandlog reset slow recency
        assert_equal 0 [r commandlog len slow recency]
        assert_equal 5 [r commandlog len slow magnitude]
        assert {[lindex [r commandlog get 1 slow magnitude] 0 2] > 5000}

        # Repopulate the recency store
        r config set commandlog-execution-slower-than 0
        r set foo1 bar1
        r set foo2 bar2
        r config set commandlog-execution-slower-than 10000

        # Resetting only the magnitude store
        r commandlog reset slow magnitude
        # The reset was logged into the threshold-ignoring magnitude store.
        set entries [r commandlog get -1 slow magnitude]
        assert_equal 1 [llength $entries]
        assert {[lindex [lindex $entries 0] 2] < 5000}
        # 1 config + 2 set
        assert_equal 3 [r commandlog len slow recency]
    } {} {needs:debug}

    test {Lowering the magnitude max-len trims immediately} {
        r commandlog reset slow
        for {set i 0} {$i < 50} {incr i} {
            r set key$i val$i
        }
        assert_equal 5 [r commandlog len slow magnitude]
        r config set commandlog-slow-execution-magnitude-max-len 2
        assert_equal 2 [r commandlog len slow magnitude]
        r config set commandlog-slow-execution-magnitude-max-len 5
    }

    test {Lowering the recency max-len trims immediately} {
        r config set commandlog-execution-slower-than 0
        r commandlog reset slow
        for {set i 0} {$i < 30} {incr i} {
            r set key$i val$i
        }
        r config set commandlog-execution-slower-than 10000
        assert {[r commandlog len slow recency] >= 20}
        r config set commandlog-slow-execution-max-len 4
        assert_equal 4 [r commandlog len slow recency]
        r config set commandlog-slow-execution-max-len 128
    }

    test {Magnitude store - disabled with max-len 0} {
        r config set commandlog-slow-execution-magnitude-max-len 0
        r commandlog reset slow
        for {set i 0} {$i < 10} {incr i} {
            r set key$i val$i
        }
        assert_equal 0 [r commandlog len slow magnitude]
        r config set commandlog-slow-execution-magnitude-max-len 5
    }

    test {Magnitude store - unaffected by the recency threshold being disabled} {
        r config set commandlog-execution-slower-than -1
        r commandlog reset slow
        for {set i 0} {$i < 10} {incr i} {
            r set key$i val$i
        }
        assert_equal 0 [r commandlog len slow recency]
        assert_equal 5 [r commandlog len slow magnitude]
        r config set commandlog-execution-slower-than 10000
    }

    test {SLOWLOG is unchanged and reads the recency entries} {
        r config set commandlog-execution-slower-than 0
        r commandlog reset slow
        for {set i 0} {$i < 10} {incr i} {
            r set key$i val$i
        }
        r config set commandlog-execution-slower-than 10000
        assert_equal [r slowlog len] [r commandlog len slow recency]
        assert_equal [r slowlog get -1] [r commandlog get -1 slow recency]
        assert_equal 3 [llength [r slowlog get 3]]
    }

    test {SLOWLOG takes no store selector} {
        assert_error "*wrong number of arguments*" {r slowlog len recency}
        assert_error "*wrong number of arguments*" {r slowlog reset magnitude}
        assert_error "*count should be greater than or equal to -1*" {r slowlog get magnitude}
    }

    test {SLOWLOG RESET clears only the recency entries} {
        r config set commandlog-execution-slower-than 0
        r commandlog reset slow
        for {set i 0} {$i < 10} {incr i} {
            r set key$i val$i
        }
        r debug sleep 0.01
        r config set commandlog-execution-slower-than 10000
        assert {[r commandlog len slow recency] > 0}
        assert {[r commandlog len slow magnitude] > 0}
        # The sleep is the slowest command by far, so it heads the magnitude set.
        assert {[lindex [lindex [r commandlog get 1 slow magnitude] 0] 2] > 5000}

        r slowlog reset
        assert_equal 0 [r commandlog len slow recency]
        # The magnitude set is untouched: the slow command is still retained.
        assert {[lindex [lindex [r commandlog get 1 slow magnitude] 0] 2] > 5000}
    } {} {needs:debug}

    test {Invalid store selector is rejected} {
        assert_error "*store should be one of*" {r commandlog len slow bogus}
        assert_error "*store should be one of*" {r commandlog get 1 slow bogus}
        assert_error "*store should be one of*" {r commandlog reset slow bogus}
    }
}

start_server {tags {"commandlog"}} {
    test {Magnitude store works for large-request and large-reply types} {
        r commandlog reset large-request
        r commandlog reset large-reply
        for {set i 0} {$i < 50} {incr i} {
            r set bigkey$i [string repeat A 100]
        }
        for {set i 0} {$i < 50} {incr i} {
            r get bigkey$i
        }
        # Default thresholds are 1MB so the recency sets stay empty,
        assert_equal 0 [r commandlog len large-request recency]
        assert_equal 0 [r commandlog len large-reply recency]
        assert_equal 32 [r commandlog len large-request magnitude]
        assert_equal 32 [r commandlog len large-reply magnitude]

        # Largest first
        set entries [r commandlog get -1 large-request magnitude]
        set prev [lindex [lindex $entries 0] 2]
        foreach entry $entries {
            assert {[lindex $entry 2] <= $prev}
            set prev [lindex $entry 2]
        }
        assert {[lindex [lindex $entries 0] 2] > 100}
    }
}

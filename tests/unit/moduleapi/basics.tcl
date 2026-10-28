set testmodule [file normalize tests/modules/basics.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {test module api basics} {
        r test.basics
    } {ALL TESTS PASSED}

    test {test rm_call auto mode} {
        r hello 2
        set reply [r test.rmcallautomode]
        assert_equal [lindex $reply 0] f1
        assert_equal [lindex $reply 1] v1
        assert_equal [lindex $reply 2] f2
        assert_equal [lindex $reply 3] v2
        r hello 3
        set reply [r test.rmcallautomode]
        assert_equal [dict get $reply f1] v1
        assert_equal [dict get $reply f2] v2
    }

    test {test get resp} {
        foreach resp {3 2} {
            if {[lsearch $::denytags "resp3"] >= 0} {
                if {$resp == 3} {continue}
            } elseif {$::force_resp3} {
                if {$resp == 2} {continue}
            }
            r hello $resp
            set reply [r test.getresp]
            assert_equal $reply $resp
            r hello 2
        }
    }

    test "Busy module name" {
        assert_error {ERR Error loading module: module initialization failed, module name is busy} {r module load $testmodule}
        verify_log_message 0 "*Module name is busy*" 0
    }

    test "test latency" {
        r config set latency-monitor-threshold 0
        r latency reset
        r test.latency 0
        r test.latency 1
        assert_equal {} [r latency latest]
        assert_equal {} [r latency history test]

        r config set latency-monitor-threshold 1
        r test.latency 0
        assert_equal 0 [llength [r latency history test]]
        r test.latency 1
        assert_match {*test * 1 1*} [r latency latest]
        r test.latency 2
        assert_match {*test * 2 2*} [r latency latest]
    }

    test "Test CallArgv" {
        set reply [r test.call_argv ping hello]
        assert_equal hello $reply

        set reply [r test.call_argv ping]
        assert_equal PONG $reply

        r set x hello
        r set y world
        set reply [r test.call_argv mget x y]
        assert_equal [lindex $reply 0] hello
        assert_equal [lindex $reply 1] world

        assert_error "ERR Write command*" {r test.call_argv set mykey myvalue}
    }

    test "CallArgv is synchronous and rejects ALLOW_BLOCK without executing" {
        assert_equal {OK 0 1} [r test.call_argv_status {} ping]
        assert_equal {OK 0 1} [r test.call_argv_status {} blpop missing-list 0]
        foreach flags {K KN EK EKT EKN EKR DK} {
            assert_equal {ERR EBADF 0} [r test.call_argv_status $flags set not-executed 1]
            assert_equal 0 [r exists not-executed]
        }
    }

    test "CallArgv rejects unsupported reply handlers versions before executing" {
        foreach flags {0 2 E0 E2 K0 K2 EK0 EK2} {
            assert_equal {ERR EBADF 0} [r test.call_argv_status $flags set invalid-version 1]
            assert_equal 0 [r exists invalid-version]
        }
        assert_equal {OK 0 1} [r test.call_argv_status {} ping]
        assert_equal {OK 0 0} [r test.call_argv_status N ping]
    }

    test "CallArgv rejects unknown flags and malformed arguments regardless of ERRORS_AS_REPLIES" {
        foreach flags {U EU DU H EH J Z EZ DZ Q EQ DQ} {
            assert_equal {ERR EBADF 0} [r test.call_argv_status $flags set malformed-call 1]
            assert_equal 0 [r exists malformed-call]
        }
    }

    test "CallArgv wrong arity respects ERRORS_AS_REPLIES" {
        assert_equal {ERR EINVAL 1} [r test.call_argv_status {} set]
        foreach flags {E ET} {
            assert_equal {OK EINVAL 1} [r test.call_argv_status $flags set]
            assert_equal {OK ENOENT 1} [r test.call_argv_status $flags no-such-command]
        }
    }

    test "CallArgv DRY_RUN implies ERRORS_AS_REPLIES before validation and execution" {
        foreach flags {D DT} {
            assert_equal {OK EINVAL 1} [r test.call_argv_status $flags set]
        }
        assert_equal {OK EINVAL 0} [r test.call_argv_status DN set]
        assert_match "-ERR wrong number*" [r test.call_argv_status DR set]
        assert_equal {ERR EBADF 0} [r test.call_argv_status DK set dry-run-key value]
        assert_equal {ERR EBADF 0} [r test.call_argv_status D0 set dry-run-key value]
        foreach flags {D DT DR} {
            assert_equal {OK 0 0} [r test.call_argv_status $flags set dry-run-key value]
        }
        assert_equal 0 [r exists dry-run-key]
    }

    test "CallArgv SCRIPT_MODE handles errors without ERRORS_AS_REPLIES" {
        assert_equal {ERR EINVAL 1} [r test.call_argv_status S set]
        assert_equal {ERR ENOENT 1} [r test.call_argv_status S no-such-command]
        assert_equal {ERR ESPIPE 1} [r test.call_argv_status S shutdown]
        assert_equal {OK EINVAL 1} [r test.call_argv_status SE set]
        assert_equal PONG [r ping]
    }

    test "CallArgv status replies when the raw callback did not run" {
        assert_equal {OK 0 0} [r test.call_argv_status RN ping]
        assert_equal {OK 0 0} [r test.call_argv_status RT ping]
        assert_equal {OK EINVAL 1} [r test.call_argv_status RET set]
        assert_equal {ERR EBADF 0} [r test.call_argv_status R0 ping]
        assert_equal {ERR EBADF 0} [r test.call_argv_status RK ping]
        assert_equal "+PONG\r\n" [r test.call_argv_status R ping]
    }

    test "Test CallArgvRaw" {
        set reply [r test.call_argv_raw ping hello]
        assert_equal "\$5\r\nhello\r\n" $reply

        set reply [r test.call_argv_raw ping]
        assert_equal "+PONG\r\n" $reply

        set reply [r test.call_argv_raw set mykey myvalue]
        assert_match "-ERR Write command*" $reply
    }

    test "Test CallArgv with null arrayStart still consumes children" {
        r set k1 v1
        r set k2 v2
        # The module calls MGET k1 k2 with arrayStart=NULL and counts how many
        # bulkString callbacks fired. Before the parser-desync fix the array
        # body was skipped entirely (result 0); with the fix children are always
        # consumed and bulkString fires once per element (result 2).
        assert_equal 2 [r test.call_argv_null_array_start k1 k2]
    }

    test "Test CallArgvScriptMode" {
        # SCRIPT_MODE blocks dangerous administrative commands like SHUTDOWN
        assert_error "ERR command 'shutdown' is not allowed on script mode" {r test.call_argv_s shutdown}

        # SCRIPT_MODE alone does not block writes
        assert_equal {OK} [r test.call_argv_s set x 1]
    }

    test "Unload the module - basics" {
        assert_equal {OK} [r module unload test]
    }
}

start_server {tags {"modules external:skip"} overrides {requirepass mypass enable-module-command no}} {
    test {module command disabled} {
        assert_error "NOAUTH *" {r module load $testmodule}

        r auth mypass
        assert_error "ERR *MODULE command not allowed*" {r module load $testmodule}
    }
}

start_server {tags {"modules" "external:skip"} overrides {save "" forkless-infrastructure-enabled yes bgsave-default-method forkless}} {
    r module load $testmodule forkless

    test "CallArgv infrastructure blocking respects ERRORS_AS_REPLIES without deferred writes" {
        r set held-key original
        r config set rdb-key-save-delay 2000000
        r bgsave
        wait_for_condition 100 10 {
            [getInfoProperty [r info debug] forkless_current_item_ms] > 0
        } else {
            fail "Forkless save did not acquire the key"
        }
        assert_equal forkless [s rdb_current_bgsave_type]

        foreach flags {{} N} {
            assert_equal {ERR EAGAIN 0} [r test.call_argv_status $flags set held-key changed]
        }
        foreach flags {E ET} {
            assert_equal {OK EAGAIN 1} [r test.call_argv_status $flags set held-key changed]
        }
        assert_equal {OK EAGAIN 0} [r test.call_argv_status EN set held-key changed]
        assert_equal "-INUSE Key is being processed\r\n" \
            [r test.call_argv_status ER set held-key changed]
        assert_equal original [r get held-key]

        waitForBgsave r
        assert_equal original [r get held-key]
        assert_equal {OK 0 1} [r test.call_argv_status {} set held-key changed]
        assert_equal changed [r get held-key]
    }

    test "Lua uses public CallArgv synchronously with an undeclared key during forkless save" {
        r bgsave
        wait_for_condition 100 10 {
            [getInfoProperty [r info debug] forkless_current_item_ms] > 0
        } else {
            fail "Forkless save did not acquire the key"
        }
        assert_equal OK [r eval {return redis.call('SET', 'held-key', 'from-lua')} 0]
        assert_equal from-lua [r get held-key]
        waitForBgsave r
    }
}

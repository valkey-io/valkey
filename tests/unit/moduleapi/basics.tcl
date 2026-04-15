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
        assert_error {ERR Error loading the extension. Please check the server logs.} {r module load $testmodule}
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

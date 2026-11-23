set ::num_tests 0
set ::num_passed 0
set ::num_failed 0
set ::num_skipped 0
set ::num_aborted 0
set ::tests_failed {}
set ::cur_test ""
# State changed by the current test that is automatically undone when it ends.
# Entries are added by pause_process and test_defer.
set ::test_paused_pids {}
set ::test_deferred {}

proc fail {msg} {
    error "assertion:$msg"
}

proc skip {msg} {
    error "skipped:$msg"
}

proc assert {condition} {
    if {![uplevel 1 [list expr $condition]]} {
        set context "(context: [info frame -1])"
        error "assertion:Expected [uplevel 1 [list subst -nocommands $condition]] $context"
    }
}

proc assert_no_match {pattern value} {
    if {[string match $pattern $value]} {
        set context "(context: [info frame -1])"
        error "assertion:Expected '$value' to not match '$pattern' $context"
    }
}

proc assert_match {pattern value {detail ""} {context ""}} {
    if {![string match $pattern $value]} {
        if {$context eq ""} {
            set context "(context: [info frame -1])"
        }
        error "assertion:Expected '$value' to match '$pattern' $context $detail"
    }
}

proc assert_failed {expected_err detail} {
     if {$detail ne ""} {
        set detail "(detail: $detail)"
     } else {
        set detail "(context: [info frame -2])"
     }
     error "assertion:$expected_err $detail"
}

proc assert_not_equal {value expected {detail ""}} {
    if {!($expected ne $value)} {
        assert_failed "Expected '$value' not equal to '$expected'" $detail
    }
}

proc assert_equal {value expected {detail ""}} {
    if {$expected ne $value} {
        assert_failed "Expected '$value' to be equal to '$expected'" $detail
    }
}

proc assert_lessthan {value expected {detail ""}} {
    if {!($value < $expected)} {
        assert_failed "Expected '$value' to be less than '$expected'" $detail
    }
}

proc assert_lessthan_equal {value expected {detail ""}} {
    if {!($value <= $expected)} {
        assert_failed "Expected '$value' to be less than or equal to '$expected'" $detail
    }
}

proc assert_morethan {value expected {detail ""}} {
    if {!($value > $expected)} {
        assert_failed "Expected '$value' to be more than '$expected'" $detail
    }
}

proc assert_morethan_equal {value expected {detail ""}} {
    if {!($value >= $expected)} {
        assert_failed "Expected '$value' to be more than or equal to '$expected'" $detail
    }
}

proc assert_range {value min max {detail ""}} {
    if {!($value <= $max && $value >= $min)} {
        assert_failed "Expected '$value' to be between to '$min' and '$max'" $detail
    }
}

proc assert_error {pattern code {detail ""}} {
    if {[catch {uplevel 1 $code} error]} {
        assert_match $pattern $error $detail
    } else {
        assert_failed "Expected an error matching '$pattern' but got '$error'" $detail
    }
}

proc assert_encoding {enc key} {
    if {$::ignoreencoding} {
        return
    }
    set val [r object encoding $key]
    assert_match $enc $val
}

proc assert_type {type key} {
    assert_equal $type [r type $key]
}

proc assert_refcount {ref key} {
    if {[lsearch $::denytags "needs:debug"] >= 0} {
        return
    }

    set val [r object refcount $key]
    assert_equal $ref $val
}

proc assert_refcount_morethan {key ref} {
    if {[lsearch $::denytags "needs:debug"] >= 0} {
        return
    }

    set val [r object refcount $key]
    assert_morethan $val $ref
}

# Wait for the specified condition to be true, with the specified number of
# max retries and delay between retries. Otherwise, the 'elsescript' is
# executed. If 'debugscript' is provided, it is executed after failure of
# the condition (before the retry delay).
proc wait_for_condition {maxtries delay e _else_ elsescript {_debug_ ""} {debugscript ""}} {
    while {[incr maxtries -1] >= 0} {
        set errcode [catch {uplevel 1 [list expr $e]} result]
        if {$errcode == 0} {
            if {$result} break
        } else {
            return -code $errcode $result
        }
        if {$_debug_ == "debug"} {
            uplevel 1 $debugscript
        }
        after $delay
    }
    if {$maxtries == -1} {
        set errcode [catch [uplevel 1 $elsescript] result]
        return -code $errcode $result
    }
}

proc verify_replica_online {master replica_idx max_retry} {
    set pause 100
    set count_down $max_retry
    while {$count_down} {
        set info [$master info]
        set pattern *slave$replica_idx:*state=online*
        if {[string match $pattern $info]} {
            break
        } else {
            incr count_down -1
            after $pause
        }
    }
    if {$count_down == 0} {
        set threshold [expr {$max_retry*$pause/1000}]
        error "assertion:Replica is not in sync after $threshold seconds"
    } 
}

proc replica_value_matches_or_is_loading {replica key expected} {
    if {[catch {$replica get $key} replica_val]} {
        if {[string match {LOADING *} $replica_val]} {
            return 0
        }
        return -code error $replica_val
    }
    return [expr {$replica_val eq $expected}]
}

proc wait_for_value_to_propagate_to_replica {master replica key} {
    set val [$master get $key]
    wait_for_condition 50 500 {
        [replica_value_matches_or_is_loading $replica $key $val]
    } else {
        catch {$replica get $key} replica_val
        error "Key $key did not propagate. Expected $val but got $replica_val"
    }
}

# try to match a value to a list of patterns that are either regex (starts with "/") or plain string.
# The caller can specify to use only glob-pattern match
proc search_pattern_list {value pattern_list {glob_pattern false}} {
    foreach el $pattern_list {
        if {[string length $el] == 0} { continue }
        if { $glob_pattern } {
            if {[string match $el $value]} {
                return 1
            }
            continue
        }
        if {[string equal / [string index $el 0]] && [regexp -- [string range $el 1 end] $value]} {
            return 1
        } elseif {[string equal $el $value]} {
            return 1
        }
    }
    return 0
}

# Register a script to run when the current test ends, whether it passed or
# failed. Deferred scripts run in reverse registration order, in the scope of
# the test body. Must be called from inside a test.
proc test_defer {script} {
    if {$::cur_test eq ""} {
        error "test_defer called outside a test"
    }
    lappend ::test_deferred [list always $script]
}

# Like test_defer, but the script only runs if the test body failed.
proc test_defer_fail {script} {
    if {$::cur_test eq ""} {
        error "test_defer_fail called outside a test"
    }
    lappend ::test_deferred [list fail $script]
}

# Set a config for the duration of the current test only. The previous value
# is restored when the test ends. 'client' is a command prefix such as r,
# {r -1}, {R 2} or a client handle. The server is identified by its run_id, so
# the restore is skipped if the client no longer reaches that same server, e.g.
# the test shut it down, or 'r' now points to an outer server.
#
# Don't use it on a server that the test restarts: a restart changes the
# run_id, so the restore is skipped.
proc test_config_set {client param value} {
    set old [lindex [uplevel 1 [list {*}$client config get $param]] 1]
    set run_id [uplevel 1 [list status $client run_id]]
    uplevel 1 [list {*}$client config set $param $value]
    test_defer [list _test_config_restore $client $param $old $run_id]
}

proc _test_config_restore {client param value run_id} {
    if {[catch {uplevel 1 [list status $client run_id]} id] || $id ne $run_id} {
        return
    }
    uplevel 1 [list {*}$client config set $param $value]
}

# Undo state left behind by the current test: resume paused processes first
# so that the servers can be reached, then run deferred scripts in reverse
# order. Scripts registered with test_defer_fail only run if 'failed' is true.
# Must be called with 'uplevel 1' from 'test' so the scripts run in the scope
# of the test body. Returns a list of errors encountered.
proc test_cleanup {failed} {
    set errors {}
    foreach pid $::test_paused_pids {
        # The test may have killed or restarted the paused process.
        if {![process_is_alive $pid]} continue
        if {[catch {exec kill -SIGCONT $pid} err]} {
            lappend errors "Failed to resume process $pid: $err"
        }
    }
    set ::test_paused_pids {}
    set deferred $::test_deferred
    set ::test_deferred {}
    foreach entry [lreverse $deferred] {
        lassign $entry when script
        if {$when eq "fail" && !$failed} continue
        if {[catch {uplevel 1 $script} err]} {
            lappend errors "Deferred cleanup '$script' failed: $err"
        }
    }
    return $errors
}

proc test {name code {okpattern undefined} {tags {}}} {
    # abort if test name in skiptests
    if {[search_pattern_list $name $::skiptests]} {
        incr ::num_skipped
        send_data_packet $::test_server_fd skip $name
        return
    }
    if {$::verbose > 1} {
        puts "starting test $name"
    }
    # abort if only_tests was set but test name is not included
    if {[llength $::only_tests] > 0 && ![search_pattern_list $name $::only_tests]} {
        incr ::num_skipped
        send_data_packet $::test_server_fd skip $name
        return
    }

    set old_singledb $::singledb
    check_subtags $tags
    set tags [concat $::tags $tags]
    if {![tags_acceptable $tags err]} {
        incr ::num_aborted
        send_data_packet $::test_server_fd ignore "$name: $err"
        return
    }

    if {[lsearch $tags singledb] >= 0} {
        set ::singledb 1
    }
    incr ::num_tests
    set details {}
    lappend details "$name in $::curfile"

    # set a cur_test global to be logged into new servers that are spawn
    # and log the test name in all existing servers
    set prev_test $::cur_test
    set ::cur_test "$name in $::curfile"
    if {$::external} {
        catch {
            set r [valkey [srv 0 host] [srv 0 port] 0 $::tls]
            catch {
                $r debug log "### Starting test $::cur_test"
            }
            $r close
        }
    } else {
        set servers {}
        foreach srv $::servers {
            set stdout [dict get $srv stdout]
            set fd [open $stdout "a+"]
            puts $fd "### Starting test $::cur_test"
            close $fd
            lappend servers $stdout
        }
        if {$::verbose > 1} {
            puts "### Starting test $::cur_test - with servers: $servers"
        }
    }

    send_data_packet $::test_server_fd testing $name

    # Tests can be nested; keep the outer test's cleanup state for it.
    set prev_paused_pids $::test_paused_pids
    set prev_deferred $::test_deferred
    set ::test_paused_pids {}
    set ::test_deferred {}

    set test_start_time [clock milliseconds]
    set code_failed [catch {set retval [uplevel 1 $code]} error]
    # ::errorInfo is only set for TCL_ERROR, not for return/break/continue.
    if {$code_failed == 1} {
        set saved_error_info $::errorInfo
    }

    # Run automatic cleanup regardless of the test outcome. A cleanup failure
    # only fails a test that passed. A skipped test does not count as failed.
    set body_failed [expr {$code_failed && ![string match "skipped:*" $error]}]
    set cleanup_errors [uplevel 1 [list test_cleanup $body_failed]]
    set ::test_paused_pids $prev_paused_pids
    set ::test_deferred $prev_deferred

    if {$code_failed} {
        if {[info exists saved_error_info]} {
            set ::errorInfo $saved_error_info
        }
    } elseif {[llength $cleanup_errors]} {
        set code_failed 1
        set error "assertion:[join $cleanup_errors "\n"]"
    }

    if {$code_failed} {
        set assertion [string match "assertion:*" $error]
        set skip [string match "skipped:*" $error]
        if {$skip} {
            incr ::num_skipped
            set msg [string range $error 8 end]
            send_data_packet $::test_server_fd skip "$::cur_test: $msg"
        } elseif {$assertion || $::durable} {
            # durable prevents the whole tcl test from exiting on an exception.
            # an assertion is handled gracefully anyway.
            set msg [string range $error 10 end]
            lappend details $msg
            if {!$assertion} {
                lappend details $::errorInfo
            }
            lappend ::tests_failed $details

            incr ::num_failed
            send_data_packet $::test_server_fd err [join $details "\n"]

            if {$::exit_on_failure} {
                puts "Test error (last server port:[srv port], log:[srv stdout]), test will exit now"
                flush stdout
                exit 1
            }
            if {$::stop_on_failure} {
                puts "Test error (last server port:[srv port], log:[srv stdout]), press enter to teardown the test."
                flush stdout
                gets stdin
            }
        } else {
            # Re-raise, let handler up the stack take care of this.
            error $error $::errorInfo
        }
    } else {
        if {$okpattern eq "undefined" || $okpattern eq $retval || [string match $okpattern $retval]} {
            incr ::num_passed
            set elapsed [expr {[clock milliseconds]-$test_start_time}]
            send_data_packet $::test_server_fd ok $name $elapsed
        } else {
            set msg "Expected '$okpattern' to equal or match '$retval'"
            lappend details $msg
            lappend ::tests_failed $details

            incr ::num_failed
            send_data_packet $::test_server_fd err [join $details "\n"]
        }
    }

    if {$::traceleaks} {
        set output [exec leaks valkey-server]
        if {![string match {*0 leaks*} $output]} {
            send_data_packet $::test_server_fd err "Detected a memory leak in test '$name': $output"
        }
    }
    set ::singledb $old_singledb
    set ::cur_test $prev_test
}

# Similar to 'test', but used for running setup and teardown blocks in fixtures.
# Returns 1 on success and 0 if an assertion was caught within the code.
proc test_block {name code} {
    if {$::verbose > 1} {
        puts "starting $name"
    }
    incr ::num_tests
    set details {}
    lappend details "$name in $::curfile"
    send_data_packet $::test_server_fd testing $name
    set test_start_time [clock milliseconds]
    if {[catch {set retval [uplevel 1 $code]} error]} {
        # Catch asserts and wait_for_condition.
        if {[string match "assertion:*" $error]} {
            set msg [string range $error 10 end]
            lappend details $msg
            lappend ::tests_failed $details
            incr ::num_failed
            send_data_packet $::test_server_fd err [join $details "\n"]
            return 0
        } else {
            # Re-raise, let handler up the stack take care of this.
            error $error $::errorInfo
        }
    } else {
        # Succeeded
        incr ::num_passed
        set elapsed [expr {[clock milliseconds] - $test_start_time}]
        send_data_packet $::test_server_fd ok $name $elapsed
        return 1
    }
}

proc test_fixture {name setup teardown code} {
    if {$setup ne {}} {
        set setup_ok [test_block "Setup $name" $setup]
    } else {
        set setup_ok 1
    }
    if {$setup_ok} {
        uplevel 1 $code
    }
    if {$teardown ne {}} {
        test_block "Teardown $name" $teardown
    }
}

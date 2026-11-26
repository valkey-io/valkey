tags {"external:skip"} {

set system_name [string tolower [exec uname -s]]
set backtrace_supported [system_backtrace_supported]
set threads_mngr_supported 0 ;# Do we support printing stack trace from all threads, not just the one that got the signal?
if {$system_name eq {linux}} {
    set threads_mngr_supported 1
}

# look for the DEBUG command in the backtrace, used when we triggered
# a stack trace print while we know the server is running that command.
proc check_log_backtrace_for_debug {log_pattern} {
    # search for the final line in the stacktraces generation to make sure it was completed.
    set pattern "* STACK TRACE DONE *"
    set res [wait_for_log_messages 0 \"$pattern\" 0 100 100]

    set res [wait_for_log_messages 0 \"$log_pattern\" 0 100 100]
    if {$::verbose} { puts $res}

    # If the stacktrace is printed more than once, it means the server crashed during crash report generation
    assert_equal [count_log_message 0 "STACK TRACE -"] 1

    upvar threads_mngr_supported threads_mngr_supported

    # the following checks are only done if we support printing stack trace from all threads
    if {$threads_mngr_supported} {
        assert_equal [count_log_message 0 "setupStacktracePipe failed"] 0
        assert_equal [count_log_message 0 "failed to open /proc/"] 0
        assert_equal [count_log_message 0 "failed to find SigBlk or/and SigIgn"] 0
        # the following are skipped since valgrind is slow and a timeout can happen
        if {!$::valgrind} {
            assert_equal [count_log_message 0 "wait_threads(): waiting threads timed out"] 0
            # make sure the server prints stack trace for all threads. we know 5 threads are idle in bio.c
            # Search for thread names (bio_*) which appear on all systems, including Alpine where
            # function names may not be resolved
            assert_equal [count_log_message 0 "bio_"] 5
            # Verify at least one stack frame was emitted (format: #<n> 0x...)
            # This format is specific to libbacktrace; execinfo.h uses a different format
            if {[catch {exec grep -a "initLibbacktraceFrameState" $::VALKEY_SERVER_BIN}] == 0} {
                assert_range [count_log_message 0 "#. 0x"] 1 999
            }
        }
    }
}

# used when backtrace_supported == 0
proc check_crash_log {log_pattern} {
    set res [wait_for_log_messages 0 \"$log_pattern\" 0 50 100]
    if {$::verbose} { puts $res }
}

# test the watchdog and the stack trace report from multiple threads
if {$backtrace_supported} {
    set server_path [tmpdir server.log]
    start_server [list overrides [list dir $server_path]] {
        test "Server is able to generate a stack trace on selected systems" {
            r config set watchdog-period 200
            r debug sleep 1

            check_log_backtrace_for_debug "*WATCHDOG TIMER EXPIRED*"
            # make sure the server is still alive
            assert_equal "PONG" [r ping]
        }
    }
}

if {$backtrace_supported} {
    set check_cb check_log_backtrace_for_debug
} else {
    set check_cb check_crash_log
}

# Valgrind will complain that the process terminated by a signal, skip it.
tags {"valgrind:skip"} {
    # test being killed by a SIGABRT from outside
    set server_path [tmpdir server1.log]
    start_server [list overrides [list dir $server_path crash-memcheck-enabled no]] {
        test "Crash report generated on SIGABRT" {
            set pid [s process_id]
            r deferred 1
            r debug sleep 10 ;# so that we see the function in the stack trace
            r flush
            after 100 ;# wait for the server to get into the sleep
            exec kill -SIGABRT $pid
            $check_cb "*crashed by signal*"
        }
    }

    # test DEBUG SEGFAULT
    set server_path [tmpdir server2.log]
    start_server [list overrides [list dir $server_path crash-memcheck-enabled no]] {
        test "Crash report generated on DEBUG SEGFAULT" {
            catch {r debug segfault}
            $check_cb "*crashed by signal*"
        }
    }

    # test DEBUG SIGALRM being non-fatal
    set server_path [tmpdir server3.log]
    start_server [list overrides [list dir $server_path]] {
        test "Stacktraces generated on SIGALRM" {
            set pid [s process_id]
            r deferred 1
            r debug sleep 10 ;# so that we see the function in the stack trace
            r flush
            after 100 ;# wait for the server to get into the sleep
            exec kill -SIGALRM $pid
            $check_cb "*Received SIGALRM*"
            r read
            r deferred 0
            # make sure the server is still alive
            assert_equal "PONG" [r ping]
        }
    }

    # test hide-user-data-from-log
    set server_path [tmpdir server5.log]
    start_server [list overrides [list dir $server_path crash-memcheck-enabled no hide-user-data-from-log no]] {
        test "Config hide-user-data-from-log is off" {
            r write "*3\r\n\$3\r\nSET\r\n\$1\r\nx\r\n\$blabla\r\n"
            r flush
            catch {r debug segfault}
            wait_for_log_messages 0 {"*Query buffer: *\$blabla*"} 0 10 1000
        }
    }

    set server_path [tmpdir server6.log]
    start_server [list overrides [list dir $server_path crash-memcheck-enabled no hide-user-data-from-log yes]] {
        test "Config hide-user-data-from-log is on" {
            r write "*3\r\n\$3\r\nSET\r\n\$1\r\nx\r\n\$blabla\r\n"
            r flush
            catch {r debug segfault}
            wait_for_log_messages 0 {"*Query buffer: *redacted*"} 0 10 1000
        }
    }
}

# test DEBUG ASSERT
if {$backtrace_supported} {
    set server_path [tmpdir server4.log]
    # Use exit() instead of abort() upon assertion so Valgrind tests won't fail.
    start_server [list overrides [list dir $server_path use-exit-on-panic yes crash-memcheck-enabled no]] {
        test "Generate stacktrace on assertion" {
            catch {r debug assert}
            check_log_backtrace_for_debug "*ASSERTION FAILED*"
        }
    }
}

# test JSON log format with special characters
start_server {overrides {log-format json}} {
    test {JSON log format escapes backslash and double quote} {
        set log_lines [count_log_lines 0]
        r debug log "Test \"quotes\" and \\backslash\\"
        verify_log_message 0 {*"message":"DEBUG LOG: Test \\"quotes\\" and \\\\backslash\\\\"*} $log_lines
    }

    test {JSON log format escapes tab character} {
        set log_lines [count_log_lines 0]
        r debug log "Test\ttab"
        verify_log_message 0 {*"message":"DEBUG LOG: Test\\ttab"*} $log_lines
    }

    test {JSON log format escapes carriage return} {
        set log_lines [count_log_lines 0]
        r debug log "Test\rcarriage"
        verify_log_message 0 {*"message":"DEBUG LOG: Test\\rcarriage"*} $log_lines
    }

    test {JSON log format escapes form feed} {
        set log_lines [count_log_lines 0]
        r debug log "Test\fformfeed"
        verify_log_message 0 {*"message":"DEBUG LOG: Test\\fformfeed"*} $log_lines
    }

    test {JSON log format escapes backspace} {
        set log_lines [count_log_lines 0]
        r debug log "Test\bbackspace"
        verify_log_message 0 {*"message":"DEBUG LOG: Test\\bbackspace"*} $log_lines
    }
}

# The ISO 8601 log timestamp must carry the actual local UTC offset, including
# daylight-saving shapes that are not a one-hour step forward: Lord Howe Island
# shifts by 30 minutes, and tzdata models Europe/Dublin's winter as negative DST.
# The server inherits TZ from the test runner, so it is set around start_server.
#
# The log line stamps the current time, so this checks the whole path (cached
# offset -> lock-free conversion -> "+HH:MM" suffix) at whatever season the test
# runs in; the seasonal cases themselves are pinned at fixed instants by the
# unit tests in src/unit/test_util.cpp.
foreach tz {Australia/Lord_Howe Europe/Dublin America/St_Johns} {
    # Skip zones this system's tz database does not know (libc would fall back to UTC).
    if {![file exists /usr/share/zoneinfo/$tz]} continue
    set saved_tz_set [info exists ::env(TZ)]
    if {$saved_tz_set} {set saved_tz $::env(TZ)}
    set ::env(TZ) $tz
    start_server {overrides {log-timestamp-format iso8601}} {
        test "ISO 8601 log timestamp carries the actual UTC offset ($tz)" {
            set log_lines [count_log_lines 0]
            r debug log "timezone probe"
            set line [lindex [wait_for_log_messages 0 {"*timezone probe*"} $log_lines 100 10] 0]
            assert {[regexp {\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}([+-]\d\d:\d\d)} $line -> got]}
            # Tcl's view of the same zone at the same instant, as +HH:MM.
            set z [clock format [clock seconds] -format %z -timezone :$tz]
            set expected "[string range $z 0 2]:[string range $z 3 4]"
            assert_equal $expected $got
        }
    }
    if {$saved_tz_set} {set ::env(TZ) $saved_tz} else {unset ::env(TZ)}
}

}

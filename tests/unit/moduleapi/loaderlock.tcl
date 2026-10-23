# Regression test for the crash-time symbolization deadlock in USE_LIBBACKTRACE builds.
# Linux-only: the loaderlock module relies on dl_iterate_phdr/_dl_load_lock.

set testmodule [file normalize tests/modules/loaderlock.so]

set libbacktrace_build 0
if {[string tolower [exec uname -s]] eq {linux} &&
    [system_backtrace_supported] &&
    [catch {exec grep -a "initLibbacktraceFrameState" $::VALKEY_SERVER_BIN}] == 0} {
    set libbacktrace_build 1
}

tags {"modules" "valgrind:skip"} {
    if {$libbacktrace_build} {
        set server_path [tmpdir server.loaderlock]
        start_server [list overrides [list dir $server_path crash-memcheck-enabled no loadmodule $testmodule]] {
            test "Crash report symbolizes frames while _dl_load_lock is held" {
                set pid [s process_id]

                assert_equal "OK" [r loaderlock.hold]
                exec kill -SIGABRT $pid

                wait_for_log_messages 0 {"*BUG REPORT END*"} 0 300 100

                assert_equal 1 [count_log_message 0 "STACK TRACE -"]

                assert_range [count_log_message 0 "#. 0x"] 1 999
            }
        }
    }
}

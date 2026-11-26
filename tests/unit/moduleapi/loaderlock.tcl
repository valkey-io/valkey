# The crash report must finish while another thread holds the dynamic linker
# lock (it's inside dl_iterate_phdr). Stack collection and symbolization may
# deadlock on that lock, so they run in forked children that are killed on
# timeout, and the report notes it instead of hanging.
# Linux-only: the loaderlock module relies on dl_iterate_phdr.

set testmodule [file normalize tests/modules/loaderlock.so]

set linux [expr {[string tolower [exec uname -s]] eq {linux}}]
# Only libbacktrace needs the lock held by dl_iterate_phdr, backtrace_symbols_fd() doesn't.
set libbacktrace_build [expr {[catch {exec grep -a "initLibbacktraceFrameState" $::VALKEY_SERVER_BIN}] == 0}]
# Only glibc's dl_iterate_phdr holds its lock while running the callback, musl's doesn't.
set glibc_build [expr {[catch {exec ldd $::VALKEY_SERVER_BIN} ldd] == 0 && [string match {*libc.so.6*} $ldd]}]

tags {"modules" "valgrind:skip"} {
    if {$linux} {
        start_server [list overrides [list crash-memcheck-enabled no loadmodule $testmodule]] {
            test "Crash report completes while dl_iterate_phdr holds the loader lock" {
                set pid [s process_id]

                assert_equal "OK" [r loaderlock.hold]
                exec kill -SIGABRT $pid

                wait_for_log_messages 0 {"*BUG REPORT END*"} 0 300 100

                # If the stacktrace is printed more than once, the server crashed during crash report generation
                assert_equal 1 [count_log_message 0 "STACK TRACE -"]
                assert_equal 1 [count_log_message 0 "STACK TRACE DONE"]
                if {$libbacktrace_build && $glibc_build} {
                    # Symbolization deadlocks on the held lock and is reported as timed out.
                    assert_range [count_log_message 0 "stack trace symbolization timed out"] 1 999
                }
            }
        }
    }
}

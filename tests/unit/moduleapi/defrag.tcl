set testmodule [file normalize tests/modules/defragtest.so]

start_server {tags {"modules"} overrides {{save ""}}} {
    # Load with 10000 global strings and a global defrag step limit of 100, so
    # the global callback must resume across many invocations via its cursor.
    r module load $testmodule 10000 100
    r config set active-defrag-ignore-bytes 1
    r config set active-defrag-threshold-lower 0
    r config set active-defrag-cycle-min 99

    # try to enable active defrag, it will fail if the server was compiled without it
    catch {r config set activedefrag yes} e
    if {[r config get activedefrag] eq "activedefrag yes"} {

        test {Module defrag: simple key defrag works} {
            r frag.create key1 1 1000 0

            after 2000
            set info [r info defragtest_stats]
            assert {[getInfoProperty $info defragtest_datatype_attempts] > 0}
            assert_equal 0 [getInfoProperty $info defragtest_datatype_resumes]
        }

        test {Module defrag: late defrag with cursor works} {
            r flushdb
            r frag.resetstats

            # key can only be defragged in no less than 10 iterations
            # due to maxstep
            r frag.create key2 10000 100 1000

            after 2000
            set info [r info defragtest_stats]
            assert {[getInfoProperty $info defragtest_datatype_resumes] > 10}
            assert_equal 0 [getInfoProperty $info defragtest_datatype_wrong_cursor]
        }

        test {Module defrag: global defrag works} {
            r flushdb
            r frag.resetstats

            after 2000
            set info [r info defragtest_stats]
            assert {[getInfoProperty $info defragtest_global_attempts] > 0}
        }

        test {Module defrag: global defrag resumes via cursor} {
            r flushdb
            r frag.resetstats

            # With the module's global step limit, the 10000 global strings
            # can't be defragged in one invocation, so the callback must be
            # re-invoked and resume from its saved cursor. This exercises the
            # endtime + per-module cursor forwarded to the global callback.
            after 2000
            set info [r info defragtest_stats]
            assert {[getInfoProperty $info defragtest_global_resumes] > 0}
            assert_equal 0 [getInfoProperty $info defragtest_global_wrong_cursor]
        }

        test {Module defrag: global defrag is revisited on later cycles} {
            r flushdb
            r frag.resetstats

            # Once the callback finishes a pass it resets its cursor to 0 (done).
            # A finished module is skipped for the rest of that cycle but must be
            # revisited on later cycles, so over time the callback runs many more
            # times than the single pass needed to walk all global strings.
            after 3000
            set info [r info defragtest_stats]
            assert {[getInfoProperty $info defragtest_global_attempts] > 10000}
            assert_equal 0 [getInfoProperty $info defragtest_global_wrong_cursor]
        }

        # The next DbKeys stage must not look up a deferred key that FLUSHDB ASYNC removed.
        test {Module defrag: FLUSHDB ASYNC while a deferred key is defragged} {
            # db 9 (the harness default) is scanned right after db 0; keep it empty.
            r flushdb
            r frag.resetstats
            r frag.stall 1

            # bigkey is too big to defrag inline, so it is deferred; the stall keeps defrag on it.
            r select 0
            r frag.create bigkey 10000 100 0
            wait_for_condition 50 100 {
                [getInfoProperty [r info defragtest_stats] defragtest_datatype_attempts] > 0
            } else {
                fail "defrag never reached the deferred key"
            }

            # Flush while defrag is stuck on bigkey, resetting the counters in the same step.
            r multi
            r config resetstat
            r frag.resetstats
            r flushdb async
            r exec

            # The globals stage runs after db 9 is scanned. All dbs are empty, so any key hit
            # or miss is a lookup of the flushed bigkey.
            wait_for_condition 50 100 {
                [getInfoProperty [r info defragtest_stats] defragtest_global_attempts] > 0
            } else {
                fail "defrag did not get past the flushed db"
            }
            assert_equal 0 [expr {[s active_defrag_key_hits] + [s active_defrag_key_misses]}]

            r frag.stall 0
            r select 9
        }
    }
}

# Global defrag resume state must survive MODULE UNLOAD. defragunload holds its
# pass open with cursor 1 until released, and defragtest is loaded after it, so
# a cursor that lands on the wrong module shows up as global_wrong_cursor.
set unloadmodule [file normalize tests/modules/defragunload.so]
set infomodule [file normalize tests/modules/infotest.so]

# Returns 0 if the server was compiled without active defrag.
proc start_held_global_defrag {} {
    r config set active-defrag-ignore-bytes 1
    r config set active-defrag-threshold-lower 0
    r config set active-defrag-cycle-min 99
    catch {r config set activedefrag yes}
    if {[r config get activedefrag] ne "activedefrag yes"} {return 0}
    wait_for_condition 100 50 {
        [r defragunload.calls] > 0
    } else {
        fail "global defrag never reached defragunload"
    }
    return 1
}

proc assert_defragtest_cursor_intact {} {
    wait_for_condition 100 50 {
        [getInfoProperty [r info defragtest_stats] defragtest_global_attempts] > 0
    } else {
        fail "global defrag never reached defragtest"
    }
    assert_equal 0 [getInfoProperty [r info defragtest_stats] defragtest_global_wrong_cursor]
}

start_server {tags {"modules"} overrides {{save ""}}} {
    r module load $unloadmodule
    r module load $testmodule 100
    if {[start_held_global_defrag]} {
        test {Module defrag: unloading the module being defragged does not pass its cursor on} {
            r module unload defragunload
            assert_defragtest_cursor_intact
        }
    }
}

start_server {tags {"modules"} overrides {{save ""}}} {
    r module load $infomodule
    r module load $unloadmodule
    r module load $testmodule 100
    if {[start_held_global_defrag]} {
        test {Module defrag: unloading an earlier module does not skip the module being defragged} {
            r module unload infotest

            # defragunload must still be the module being defragged.
            set calls [r defragunload.calls]
            wait_for_condition 100 50 {
                [r defragunload.calls] > $calls
            } else {
                fail "defragunload was skipped after unloading infotest"
            }
            assert_equal 0 [getInfoProperty [r info defragtest_stats] defragtest_global_attempts]

            r defragunload.release
            assert_defragtest_cursor_intact
        }
    }
}

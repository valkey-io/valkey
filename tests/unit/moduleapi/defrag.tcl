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
    }
}

set busymodule [file normalize tests/modules/defragglobalbusy.so]

start_server {tags {"modules"} overrides {{save ""}}} {
    # Load the "busy" module first so it sits ahead of defragtest in the module
    # list. Its global defrag callback consumes the whole deadline every call
    # and never finishes. defragtest is loaded second with a small step limit.
    r module load $busymodule
    r module load $testmodule 10000 100
    r config set active-defrag-ignore-bytes 1
    r config set active-defrag-threshold-lower 0
    r config set active-defrag-cycle-min 99

    catch {r config set activedefrag yes} e
    if {[r config get activedefrag] eq "activedefrag yes"} {

        test {Module defrag: a busy global callback does not starve others} {
            r frag.resetstats

            # The busy module always leaves work, so if the stage restarted at
            # the head every time it would run only the busy module forever.
            # Round-robin scheduling advances past it, so defragtest's global
            # callback still gets invoked.
            after 3000
            set busyinfo [r info defragglobalbusy_stats]
            set testinfo [r info defragtest_stats]
            assert {[getInfoProperty $busyinfo defragglobalbusy_busy_calls] > 0}
            assert {[getInfoProperty $testinfo defragtest_global_attempts] > 0}
        }

        test {Module defrag: aborting a cycle discards the module cursor} {
            # The busy module stores a non-zero cursor on every invocation and never
            # finishes, so once it has run it always resumes from a saved position.
            # Aborting the cycle must discard that position, because it refers to
            # module state that may be rebuilt before defrag next runs. Observed via
            # busy_fresh_starts, which only increments when the callback sees a zero
            # cursor, i.e. the start of a genuinely fresh pass.
            set before [getInfoProperty [r info defragglobalbusy_stats] \
                            defragglobalbusy_busy_fresh_starts]
            assert {$before > 0}

            # Abnormal termination: this is the endDefragCycle(false) path.
            r config set activedefrag no
            r config set activedefrag yes

            wait_for_condition 50 100 {
                [getInfoProperty [r info defragglobalbusy_stats] \
                     defragglobalbusy_busy_fresh_starts] > $before
            } else {
                fail "module cursor was not discarded when the defrag cycle was aborted"
            }
        }

        test {Module defrag: unloading a module releases an outstanding cursor} {
            # The busy module never finishes, so it always has a cursor allocated. Unloading it must
            # free that cursor rather than leak it, and must not leave the defrag stage referring to
            # a module that is gone.
            assert {[getInfoProperty [r info defragglobalbusy_stats] \
                         defragglobalbusy_busy_calls] > 0}

            assert_equal {OK} [r module unload defragglobalbusy]

            # The remaining module must keep being defragged afterwards.
            r frag.resetstats
            wait_for_condition 50 100 {
                [getInfoProperty [r info defragtest_stats] defragtest_global_attempts] > 0
            } else {
                fail "global defrag stopped working after a module was unloaded"
            }
        }
    }
}

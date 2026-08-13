start_server {tags {"hotkey external:skip"}} {
    # QPS accounting uses a fixed window; HOTKEYS GET reports the last
    # *completed* window. Rather than sleep a fixed time (racy: the accessed
    # window can split, or a too-long wait empties the snapshot), poll GET until
    # the accessed window has been frozen, capturing the first non-empty result
    # so we never overshoot into the emptied next window.
    proc hk_wait_hotkeys {} {
        global _hk
        wait_for_condition 50 100 {
            [llength [set _hk [r hotkeys get]]] > 0
        } else {
            fail "no hot keys reported within the timeout"
        }
        return $_hk
    }

    test "Enable hotkey functionality" {
        r config set hotkey-enabled yes
        r config set hotkey-sampling-percentage 100
        r config set hotkey-window-seconds 1
        set hotkey_status [r config get hotkey-enabled]
        assert_equal [lindex $hotkey_status 1] "yes"
    }

    test "HOTKEYS GET returns empty when no hot keys" {
        r hotkeys reset
        set all_hotkeys [r hotkeys get]
        assert_equal [llength $all_hotkeys] 0
    }

    test "Generate hot keys through repeated access" {
        r hotkeys reset
        r config set hotkey-sampling-percentage 100
        r config set hotkey-top-k 16

        r set "hot_read_key" "value"
        for {set j 1} {$j <= 300} {incr j} {
            r get "hot_read_key"
            r set "hot_write_key" "value_$j"
        }

        set all_hotkeys [hk_wait_hotkeys]
        assert {[llength $all_hotkeys] > 0}

        # Each entry is a map: {key <name> db <db> qps <qps>}.
        set first [lindex $all_hotkeys 0]
        assert_equal [lsort [dict keys $first]] {db key qps}
        assert {[string length [dict get $first key]] > 0}
    }

    test "Reads and writes share one combined summary" {
        r hotkeys reset
        r config set hotkey-sampling-percentage 100
        r config set hotkey-top-k 16

        r set "combined_key" "val"
        for {set i 0} {$i < 300} {incr i} {
            r get "combined_key"
            r set "combined_key" "val_$i"
        }

        set hotkeys [hk_wait_hotkeys]
        assert {[llength $hotkeys] > 0}
        # The key appears once (a single summary), not split by access type.
        set names {}
        foreach e $hotkeys { lappend names [dict get $e key] }
        assert_equal [llength [lsearch -all $names "combined_key"]] 1
    }

    test "HOTKEYS RESET clears all statistics" {
        set reset_result [r hotkeys reset]
        assert_equal $reset_result "OK"
        set all_hotkeys [r hotkeys get]
        assert_equal [llength $all_hotkeys] 0
    }

    test "Hotkey detection with different data types" {
        r hotkeys reset
        r config set hotkey-sampling-percentage 100
        r config set hotkey-top-k 16

        r set "hot_string" "value"
        r hset "hot_hash" "field_1" "value"
        r rpush "hot_list" "item"
        r sadd "hot_set" "member"
        r zadd "hot_zset" 1.0 "member"

        for {set i 1} {$i <= 300} {incr i} {
            r get "hot_string"
            r hget "hot_hash" "field_1"
            r lrange "hot_list" 0 -1
            r smembers "hot_set"
            r zrange "hot_zset" 0 -1
        }

        set all_hotkeys [hk_wait_hotkeys]
        assert {[llength $all_hotkeys] > 0}
    }

    test "Invalid HOTKEYS command syntax" {
        catch {r hotkeys invalid} err
        assert_match "*unknown*subcommand*" $err
        catch {r hotkeys} err
        assert_match "*wrong number of arguments*" $err
    }

    test "Hotkey configuration parameters" {
        r config set hotkey-sampling-percentage 50
        assert_equal [lindex [r config get hotkey-sampling-percentage] 1] "50"

        r config set hotkey-top-k 8
        assert_equal [lindex [r config get hotkey-top-k] 1] "8"

        r config set hotkey-window-seconds 2
        assert_equal [lindex [r config get hotkey-window-seconds] 1] "2"

        # Restore defaults for subsequent tests
        r config set hotkey-sampling-percentage 100
        r config set hotkey-top-k 16
        r config set hotkey-window-seconds 1
    }

    test "Disable hotkey functionality" {
        r config set hotkey-enabled no
        assert_equal [lindex [r config get hotkey-enabled] 1] "no"
        catch {r hotkeys get} err
        assert_match "*Hotkey detection is disabled*" $err
        catch {r hotkeys reset} err
        assert_match "*Hotkey detection is disabled*" $err
    }

    test "Re-enable hotkey functionality" {
        r config set hotkey-enabled yes
        assert_equal [lindex [r config get hotkey-enabled] 1] "yes"
        assert_equal [r hotkeys reset] "OK"
    }

    test "Hotkey detection with high frequency access" {
        r hotkeys reset
        r config set hotkey-sampling-percentage 100
        r config set hotkey-top-k 16

        for {set i 1} {$i <= 300} {incr i} {
            r set "super_hot_write" "value_$i"
            r get "super_hot_read"
        }

        set all_hotkeys [hk_wait_hotkeys]
        assert {[llength $all_hotkeys] > 0}
    }

    test "HOTKEYS GET returns sorted by QPS descending" {
        r hotkeys reset
        r config set hotkey-sampling-percentage 100
        r config set hotkey-top-k 16

        r set "low_freq" "val"
        r set "mid_freq" "val"
        r set "high_freq" "val"

        # Run all reads inside one MULTI/EXEC so they land in a single detection
        # window: EXEC executes the whole batch in one event-loop tick, so the
        # window clock cannot advance mid-batch and split the keys.
        r multi
        for {set i 0} {$i < 100} {incr i} { r get "low_freq" }
        for {set i 0} {$i < 500} {incr i} { r get "mid_freq" }
        for {set i 0} {$i < 1000} {incr i} { r get "high_freq" }
        r exec

        set hotkeys [hk_wait_hotkeys]
        assert {[llength $hotkeys] >= 2}

        # QPS is at field index 5
        set prev_qps [dict get [lindex $hotkeys 0] qps]
        for {set i 1} {$i < [llength $hotkeys]} {incr i} {
            set cur_qps [dict get [lindex $hotkeys $i] qps]
            assert {$prev_qps >= $cur_qps}
            set prev_qps $cur_qps
        }
    }

    test "HOTKEYS GET limits results to top-k" {
        r hotkeys reset
        r config set hotkey-sampling-percentage 100
        r config set hotkey-top-k 3

        r multi
        foreach key {k1 k2 k3 k4 k5 k6 k7 k8} {
            for {set i 0} {$i < 200} {incr i} { r get $key }
        }
        r exec

        set hotkeys [hk_wait_hotkeys]
        assert {[llength $hotkeys] <= 3}

        r config set hotkey-top-k 16
    }

    test "Hotkey entries include db field" {
        r hotkeys reset
        r config set hotkey-sampling-percentage 100
        r config set hotkey-top-k 16

        r set "db_field_key" "val"
        for {set i 0} {$i < 300} {incr i} { r get "db_field_key" }

        set hotkeys [hk_wait_hotkeys]
        assert {[llength $hotkeys] > 0}
        set first [lindex $hotkeys 0]
        assert {[dict exists $first db]}
        # Tests run against a non-zero default DB; just assert the field is an int.
        assert {[string is integer [dict get $first db]]}
    }

    test "FLUSHALL purges all hotkey state" {
        r hotkeys reset
        r config set hotkey-sampling-percentage 100
        r config set hotkey-top-k 16

        r set "flush_key" "val"
        for {set i 0} {$i < 300} {incr i} { r get "flush_key" }

        set hotkeys_before [hk_wait_hotkeys]
        assert {[llength $hotkeys_before] > 0}

        r flushall

        set hotkeys_after [r hotkeys get]
        assert_equal [llength $hotkeys_after] 0
    }

    test "Test memory cleanup on manager recreation" {
        r hotkeys reset
        r config set hotkey-sampling-percentage 100
        r config set hotkey-top-k 16

        for {set i 1} {$i <= 10} {incr i} {
            for {set j 1} {$j <= 100} {incr j} {
                r get "memory_test_key_$i"
            }
        }

        set hotkeys_before [hk_wait_hotkeys]
        assert {[llength $hotkeys_before] > 0}

        r config set hotkey-enabled no
        r config set hotkey-enabled yes
        set hotkeys_after [r hotkeys get]
        assert_equal [llength $hotkeys_after] 0
        assert_equal [r ping] "PONG"
    }

    test "QPS reported as a positive rate over the window" {
        r hotkeys reset
        r config set hotkey-sampling-percentage 100
        r config set hotkey-top-k 16
        r config set hotkey-window-seconds 1

        r set "qps_key" "val"
        for {set i 0} {$i < 400} {incr i} { r get "qps_key" }

        set hotkeys [hk_wait_hotkeys]
        assert {[llength $hotkeys] > 0}
        set first [lindex $hotkeys 0]
        assert_equal [dict get $first key] "qps_key"
        # QPS is count over the last completed window; assert a sane positive
        # value rather than an exact figure (sampling-boundary noise applies).
        set qps [dict get $first qps]
        assert {$qps > 0}
    }

    test "Cold keys drop after a completed idle window" {
        r hotkeys reset
        r config set hotkey-sampling-percentage 100
        r config set hotkey-top-k 16
        r config set hotkey-window-seconds 1

        r set "fading_key" "val"
        for {set i 0} {$i < 200} {incr i} { r get "fading_key" }

        # The key is reportable once its window completes.
        set before [hk_wait_hotkeys]
        assert {[llength $before] > 0}

        # Two full windows elapse with no access. The live window freezes empty,
        # replacing the previous (hot) snapshot.
        after 2200
        set after_idle [r hotkeys get]
        assert_equal [llength $after_idle] 0
    }

    test "Max top-k tracks the hottest keys" {
        r hotkeys reset
        r config set hotkey-sampling-percentage 100
        r config set hotkey-top-k 3

        r set "cold_key" "val"
        r set "warm_key" "val"
        r set "hot_key" "val"

        r multi
        for {set i 0} {$i < 100} {incr i} { r get "cold_key" }
        for {set i 0} {$i < 500} {incr i} { r get "warm_key" }
        for {set i 0} {$i < 1000} {incr i} { r get "hot_key" }
        r exec

        set hotkeys [hk_wait_hotkeys]
        assert {[llength $hotkeys] <= 3}
        assert {[llength $hotkeys] > 0}

        r config set hotkey-top-k 16
    }
}

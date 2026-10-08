start_server {tags {"traffic external:skip"}} {
    # Bytes accounting uses a fixed window; TRAFFIC GET reports the last
    # *completed* window. Rather than sleep a fixed time (racy: the accessed
    # window can split, or a too-long wait empties the snapshot), poll GET until
    # the accessed window has been frozen, capturing the first non-empty result
    # so we never overshoot into the emptied next window.
    proc tr_wait_traffic {} {
        global _tr
        wait_for_condition 50 100 {
            [llength [set _tr [r traffic get]]] > 0
        } else {
            fail "no traffic reported within the timeout"
        }
        return $_tr
    }

    # The entry for a given key, or an empty string if it was not reported.
    proc tr_entry {traffic key} {
        foreach e $traffic {
            if {[dict get $e key] eq $key} { return $e }
        }
        return ""
    }

    # A value of roughly `bytes` characters.
    proc tr_value {bytes} {
        set unit "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" ;# 32 chars
        set v ""
        for {set i 0} {$i < $bytes/32} {incr i} { append v $unit }
        return $v
    }

    proc tr_enable {} {
        r config set traffic-top-k 16
        r config set traffic-sampling-percentage 100
        r config set traffic-window-seconds 1
    }

    # Local copy of the hotkeys wait helper: this file must not depend on
    # hotkeys.tcl procs.
    proc tr_wait_hotkeys {} {
        global _trhk
        wait_for_condition 50 100 {
            [llength [set _trhk [r hotkeys get]]] > 0
        } else {
            fail "no hot keys reported within the timeout"
        }
        return $_trhk
    }

    test "Default traffic window is 10 seconds" {
        # Assert before anything in this file touches the config: the default
        # must be human-friendly (a fresh report every ~11s), while monitoring
        # users align it to their scrape interval.
        assert_equal [lindex [r config get traffic-window-seconds] 1] "10"
        assert_equal [lindex [r config get traffic-top-k] 1] "0"
    }

    test "Enable traffic tracking" {
        tr_enable
        assert_equal [lindex [r config get traffic-top-k] 1] "16"
        assert_equal [lindex [r config get traffic-sampling-percentage] 1] "100"
        assert_equal [lindex [r config get traffic-window-seconds] 1] "1"
        assert_equal [r traffic reset] "OK"
    }

    test "TRAFFIC GET returns empty when tracking is disabled" {
        r config set traffic-top-k 0
        assert_equal [r traffic get] {}
        # Disabled RESET still replies OK, as HOTKEYS RESET does.
        assert_equal [r traffic reset] "OK"
        tr_enable
    }

    test "Traffic tracking works with hot key detection disabled" {
        # The two dimensions are independent: hotkeys-top-k stays 0 throughout
        # this file, and traffic must be fully functional anyway.
        assert_equal [lindex [r config get hotkeys-top-k] 1] "0"

        r traffic reset
        r set tr_ind_key [tr_value 100000]
        for {set i 0} {$i < 50} {incr i} { r get tr_ind_key }
        set traffic [tr_wait_traffic]
        assert {[llength $traffic] > 0}
        assert {[dict get [tr_entry $traffic tr_ind_key] bytes_per_second] > 0}
    }

    test "Hot key detection still works while traffic tracking is on" {
        r config set hotkeys-top-k 16
        r config set hotkeys-sampling-percentage 100
        r traffic reset
        r hotkeys reset

        r set tr_hk_key "v"
        for {set i 0} {$i < 300} {incr i} { r get tr_hk_key }

        # Both dimensions report their own view of the same activity: QPS for
        # hot keys, bytes for traffic.
        set hotkeys [tr_wait_hotkeys]
        assert {[llength $hotkeys] > 0}
        set traffic [tr_wait_traffic]
        assert {[llength $traffic] > 0}
        r config set hotkeys-top-k 0
    }

    test "Traffic entries are maps sorted by bytes per second" {
        r traffic reset
        r set tr_big [tr_value 100000]
        r set tr_mid [tr_value 10000]
        r set tr_small "y"
        for {set i 0} {$i < 40} {incr i} { r get tr_big }
        for {set i 0} {$i < 40} {incr i} { r get tr_mid }
        for {set i 0} {$i < 40} {incr i} { r get tr_small }

        set traffic [tr_wait_traffic]
        set first [lindex $traffic 0]
        assert_equal [lsort [dict keys $first]] \
            {bytes_per_second db key read_bytes_per_second unattributed_bytes_per_second write_bytes_per_second}

        # Descending by bytes_per_second.
        set prev -1
        foreach e $traffic {
            set b [dict get $e bytes_per_second]
            assert {$b <= $prev || $prev == -1}
            set prev $b
        }
    }

    test "A large value with few reads outranks a small value with many reads" {
        # The scenario the traffic dimension exists for: 100KB x 30 reads must
        # beat 1 byte x 3000 reads — the reverse of the QPS ranking.
        r traffic reset
        r set tr_heavy [tr_value 100000]
        r set tr_tiny "z"
        for {set i 0} {$i < 3000} {incr i} {
            r get tr_tiny
            if {$i % 100 == 0} { r get tr_heavy }
        }

        set traffic [tr_wait_traffic]
        set heavy [tr_entry $traffic tr_heavy]
        set tiny [tr_entry $traffic tr_tiny]
        assert {$heavy ne ""} ;# a few heavy reads must still be tracked
        assert {$tiny ne ""}
        assert {[dict get $heavy bytes_per_second] > [dict get $tiny bytes_per_second] * 100}
        assert_equal [dict get [lindex $traffic 0] key] "tr_heavy"
    }

    test "Reads charge the value's size" {
        r traffic reset
        r set tr_read [tr_value 100000]
        for {set i 0} {$i < 100} {incr i} { r get tr_read }

        set e [tr_entry [tr_wait_traffic] tr_read]
        # 100 reads of ~100KB inside a ~1s window: at least a fifth of the
        # reads must have landed in the captured window (loose bound against
        # window splits).
        assert {[dict get $e bytes_per_second] > 20 * 100000}
    }

    test "Entries split bytes into read and write directions" {
        r traffic reset
        set big [tr_value 100000]
        r set tr_dir_read $big
        r set tr_dir_write a
        # Drop the setup charges; keep only the deliberate load below.
        r traffic reset
        for {set i 0} {$i < 50} {incr i} {
            r get tr_dir_read
            r set tr_dir_write $big
        }

        set traffic [tr_wait_traffic]
        set rd [tr_entry $traffic tr_dir_read]
        set wr [tr_entry $traffic tr_dir_write]
        assert {$rd ne "" && $wr ne ""}

        # Read-heavy: all egress, no writes since the reset.
        assert {[dict get $rd write_bytes_per_second] == 0}
        assert {[dict get $rd read_bytes_per_second] > 10 * 100000}

        # Write-heavy: ingress dominates; write lookups charge no egress.
        assert {[dict get $wr write_bytes_per_second] > 10 * 100000}
        assert {[dict get $wr write_bytes_per_second] > [dict get $wr read_bytes_per_second] * 10}

        # The split adds up to the combined figure, including the
        # eviction-inherited share.
        foreach e $traffic {
            assert_equal [dict get $e bytes_per_second] \
                [expr {[dict get $e read_bytes_per_second] + [dict get $e write_bytes_per_second] +
                       [dict get $e unattributed_bytes_per_second]}]
        }
    }

    test "Eviction-inherited uncertainty is not booked as reads" {
        r traffic reset
        r config set traffic-top-k 1
        set big [tr_value 100000]
        r set ev_a $big
        r traffic reset
        # A single read fills the one slot; the write of another key then
        # evicts it, so B's combined estimate inherits A's mass while B itself
        # was only ever written to.
        r get ev_a
        r set ev_b x
        set traffic [tr_wait_traffic]
        set b [tr_entry $traffic ev_b]
        assert {$b ne ""}
        assert_equal [dict get $b read_bytes_per_second] 0 "B was never read"
        assert {[dict get $b write_bytes_per_second] < 100}
        assert {[dict get $b unattributed_bytes_per_second] > 40000}
        assert_equal [dict get $b bytes_per_second] \
            [expr {[dict get $b read_bytes_per_second] + [dict get $b write_bytes_per_second] +
                   [dict get $b unattributed_bytes_per_second]}]
        r config set traffic-top-k 16
    }

    test "INFO traffic exposes the live window" {
        r traffic reset
        r set tr_live [tr_value 100000]
        r traffic reset
        # Charge the live window immediately before reading it; retry in case a
        # rotation lands between the access and the read.
        set live 0
        for {set i 0} {$i < 20} {incr i} {
            r get tr_live
            if {[regexp {traffic_live_window_bytes:(\d+)} [r info traffic] - live] && $live > 0} break
            after 50
        }
        assert {$live > 0}
    }

    test "A fresh key's write is captured without any read" {
        # Write-heavy loads on new keys move input bytes; the setKey hook sees
        # them even though the lookup is a miss.
        r traffic reset
        r set tr_fresh [tr_value 200000]
        set e [tr_entry [tr_wait_traffic] tr_fresh]
        assert {[dict get $e bytes_per_second] > 100000}
    }

    test "An overwrite charges the written value" {
        r traffic reset
        r set tr_over "a"
        r set tr_over "a"
        for {set i 0} {$i < 100} {incr i} { r set tr_over [tr_value 100000] }

        set e [tr_entry [tr_wait_traffic] tr_over]
        # ~100 writes of 100KB, not the original single byte.
        assert {[dict get $e bytes_per_second] > 20 * 100000}
    }

    test "Deletion moves no bytes" {
        r traffic reset
        r set tr_del [tr_value 100000]
        # RESET drops the write charge; the DEL itself must not re-add any.
        r traffic reset
        r del tr_del
        after 1300
        assert_equal [r traffic get] {}
    }

    test "All data types produce traffic" {
        r traffic reset
        r set tr_string [tr_value 10000]
        r hset tr_hash f [tr_value 10000]
        r rpush tr_list [tr_value 10000]
        r sadd tr_set [tr_value 5000]
        r zadd tr_zset 1 [tr_value 5000]
        r xadd tr_stream * f [tr_value 5000]

        for {set i 0} {$i < 30} {incr i} {
            r get tr_string
            r hgetall tr_hash
            r lrange tr_list 0 -1
            r smembers tr_set
            r zrange tr_zset 0 -1
            r xrange tr_stream - +
        }

        set traffic [tr_wait_traffic]
        foreach key {tr_string tr_hash tr_list tr_set tr_zset tr_stream} {
            assert {[tr_entry $traffic $key] ne ""}
        }
    }

    test "Introspection lookups are not charged" {
        r traffic reset
        r set tr_introspect [tr_value 100000]
        r traffic reset
        for {set i 0} {$i < 300} {incr i} { r object encoding tr_introspect }
        after 1300
        assert_equal [r traffic get] {}
    }

    test "Misses are not charged" {
        r traffic reset
        for {set i 0} {$i < 300} {incr i} { r get tr_never_set }
        after 1300
        assert_equal [r traffic get] {}
    }

    test "EXISTS and TYPE count as reads" {
        r traffic reset
        r set tr_exists [tr_value 100000]
        r traffic reset
        for {set i 0} {$i < 400} {incr i} { r exists tr_exists ; r type tr_exists }
        # No GETs: only the read-only existence probes charge, but they charge
        # the value size, so 400 x 100KB is far above zero.
        set e [tr_entry [tr_wait_traffic] tr_exists]
        assert {[dict get $e bytes_per_second] > 0}
    }

    test "CLIENT NO-TOUCH does not suppress accounting" {
        r traffic reset
        r set tr_notouch [tr_value 100000]
        r traffic reset
        r client no-touch on
        for {set i 0} {$i < 100} {incr i} { r get tr_notouch }
        r client no-touch off
        set e [tr_entry [tr_wait_traffic] tr_notouch]
        assert {[dict get $e bytes_per_second] > 0}
    }

    test "Entries report the database they were accessed in" {
        r select 1
        r traffic reset
        r set tr_db1 [tr_value 100000]
        for {set i 0} {$i < 50} {incr i} { r get tr_db1 }
        set e [tr_entry [tr_wait_traffic] tr_db1]
        assert_equal [dict get $e db] 1
        r select 0
    }

    test "Traffic decays after an idle window" {
        r traffic reset
        r set tr_idle [tr_value 100000]
        for {set i 0} {$i < 50} {incr i} { r get tr_idle }
        assert {[llength [tr_wait_traffic]] > 0}
        # Wait past one full window with no access: the frozen window becomes
        # the empty idle one. Poll until empty to tolerate rotation lag.
        wait_for_condition 50 100 {
            [llength [r traffic get]] == 0
        } else {
            fail "traffic did not decay after an idle window"
        }
    }

    test "TRAFFIC RESET clears all statistics" {
        assert_equal [r traffic reset] "OK"
        assert_equal [r traffic get] {}
    }

    test "top-k limits the report length" {
        r traffic reset
        r config set traffic-top-k 3
        for {set i 0} {$i < 8} {incr i} {
            r set tr_limit_$i [tr_value 10000]
            for {set j 0} {$j < 20} {incr j} { r get tr_limit_$i }
        }
        set traffic [tr_wait_traffic]
        assert_equal [llength $traffic] 3
        r config set traffic-top-k 16
    }

    test "Shrinking top-k keeps the last completed window" {
        r traffic reset
        r set tr_shrink [tr_value 100000]
        for {set i 0} {$i < 50} {incr i} { r get tr_shrink }
        assert {[llength [tr_wait_traffic]] > 0}
        # A config change keeps the frozen window readable, as HOTKEYS does.
        r config set traffic-top-k 8
        assert {[llength [r traffic get]] > 0}
    }

    test "FLUSHALL purges all traffic state" {
        r traffic reset
        r set tr_flush [tr_value 100000]
        for {set i 0} {$i < 50} {incr i} { r get tr_flush }
        assert {[llength [tr_wait_traffic]] > 0}
        r flushall
        assert_equal [r traffic get] {}
    }

    test "FLUSHDB purges only the flushed database" {
        r flushall
        r traffic reset
        # Traffic in db 0 and db 2 within the same window.
        r set tr_keep [tr_value 100000]
        for {set i 0} {$i < 50} {incr i} { r get tr_keep }
        r select 2
        r set tr_drop [tr_value 100000]
        for {set i 0} {$i < 50} {incr i} { r get tr_drop }
        set traffic [tr_wait_traffic]
        assert {[tr_entry $traffic tr_keep] ne ""}
        assert {[tr_entry $traffic tr_drop] ne ""}
        # Flushing db 2 drops only db 2's entries, from both windows.
        r flushdb
        set after_flush [r traffic get]
        assert {[tr_entry $after_flush tr_drop] eq ""}
        assert {[tr_entry $after_flush tr_keep] ne ""}
        r select 0
    }

    test "INFO traffic reports the last window" {
        r traffic reset
        r set tr_info [tr_value 100000]
        for {set i 0} {$i < 50} {incr i} { r get tr_info }
        assert {[llength [tr_wait_traffic]] > 0}
        set info [r info traffic]
        assert {[regexp {traffic_last_window_bytes:(\d+)} $info - bytes]}
        assert {$bytes > 0}
        assert {[regexp {traffic_last_window_duration_ms:(\d+)} $info - ms]}
        assert {$ms >= 1000}
    }

    test "Invalid TRAFFIC command syntax" {
        catch {r traffic invalid} err
        assert_match "*unknown*subcommand*" $err
        catch {r traffic} err
        assert_match "*wrong number of arguments*" $err
    }

    test "TRAFFIC HELP lists the subcommands" {
        set help [r traffic help]
        assert_match "*GET*" $help
        assert_match "*RESET*" $help
    }

    test "TRAFFIC GET CLIENTS falls back to ip:port for unnamed clients" {
        # The test connection has no name yet, so its traffic is attributed to
        # its ip:port.
        r traffic reset
        set big [tr_value 100000]
        r set cl_unnamed $big
        r traffic reset
        for {set i 0} {$i < 50} {incr i} { r get cl_unnamed }
        set clients {}
        wait_for_condition 50 100 {
            [llength [set clients [r traffic get clients]]] > 0
        } else {
            fail "no client traffic reported within the timeout"
        }
        set e [lindex $clients 0]
        assert_equal [lsort [dict keys $e]] \
            {bytes_per_second client read_bytes_per_second unattributed_bytes_per_second write_bytes_per_second}
        assert {[regexp {(127\.0\.0\.1|unix)} [dict get $e client]]}
        assert {[dict get $e read_bytes_per_second] > 20 * 100000}
        # The same observation charged the by-key view too.
        set keys [r traffic get]
        assert {[tr_entry $keys cl_unnamed] ne ""}
    }

    test "TRAFFIC GET CLIENTS attributes traffic by client name" {
        r traffic reset
        set big [tr_value 100000]
        r set cl_named $big
        r traffic reset
        r client setname traffic_test_app
        for {set i 0} {$i < 50} {incr i} { r get cl_named }
        set clients {}
        wait_for_condition 50 100 {
            [llength [set clients [r traffic get clients]]] > 0
        } else {
            fail "no client traffic reported within the timeout"
        }
        set e [lindex $clients 0]
        assert_equal [dict get $e client] "traffic_test_app"
        assert_equal [dict get $e write_bytes_per_second] 0
        assert_equal [dict get $e unattributed_bytes_per_second] 0
        r client setname ""
    }

    test "Invalid TRAFFIC GET argument" {
        catch {r traffic get bogus} err
        assert_match "*syntax*" $err
    }

    test "TRAFFIC GET is callable from scripts" {
        # redis_exporter and similar agents collect through EVAL, so the
        # command must stay script-callable (no no-script flag).
        r traffic reset
        r set tr_eval [tr_value 100000]
        for {set i 0} {$i < 50} {incr i} { r get tr_eval }
        assert {[llength [tr_wait_traffic]] > 0}
        set from_script [r eval {return redis.call('TRAFFIC', 'GET')} 0]
        assert {[llength $from_script] > 0}
        # And the disabled state is script-visible as an empty list too.
        r config set traffic-top-k 0
        assert_equal [r eval {return redis.call('TRAFFIC', 'GET')} 0] {}
        r config set traffic-top-k 16
    }
}

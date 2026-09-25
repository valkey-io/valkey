proc fake_primary_attempt_times {count_file} {
    if {![file exists $count_file]} {return {}}
    set fd [open $count_file r]
    set times [string trim [read $fd]]
    close $fd
    if {$times eq ""} {return {}}
    return [split $times "\n"]
}

proc start_fullsync_sequence_primary {rdb_payload outcomes} {
    set rdb_file [tmpfile fullsync-sequence-rdb]
    set count_file [tmpfile fullsync-sequence-count]
    write_binary_file $rdb_file $rdb_payload
    set port [find_available_port $::baseport $::portcount]
    set pid [exec [info nameofexecutable] tests/helpers/fake_primary_fullsync_sequence.tcl \
                 $port $rdb_file $count_file $outcomes &]
    wait_for_condition 50 50 {
        [ping_server 127.0.0.1 $port]
    } else {
        fail "Failed to start fake primary"
    }
    return [list $pid $port $count_file]
}

start_server {tags {"repl external:skip tls:skip valgrind:skip"} overrides {save "" enable-debug-command local}} {
    set primary [srv 0 client]
    $primary set key value
    $primary save
    set rdb_payload [read_binary_file [server_rdb_path $primary]]

    start_server {overrides {save "" enable-debug-command local}} {
        set replica [srv 0 client]

        test {Full sync failures back off exponentially, stay jittered at the cap, and reset on success} {
            set fake_pid ""
            with_cleanup {
                $replica config set repl-sync-backoff-base-time 1
                $replica config set repl-sync-backoff-max-time 4
                lassign [start_fullsync_sequence_primary $rdb_payload F,F,F,S,F,H] fake_pid fake_port count_file
                set log_lines [count_log_lines 0]
                $replica replicaof 127.0.0.1 $fake_port

                wait_for_condition 300 100 {
                    [llength [fake_primary_attempt_times $count_file]] >= 6
                } else {
                    fail "replica did not complete the full sync retry sequence"
                }

                set times [fake_primary_attempt_times $count_file]
                set first_delay [expr {[lindex $times 1] - [lindex $times 0]}]
                set second_delay [expr {[lindex $times 2] - [lindex $times 1]}]
                set capped_delay [expr {[lindex $times 3] - [lindex $times 2]}]
                set reset_delay [expr {[lindex $times 5] - [lindex $times 4]}]
                assert {$first_delay >= 800 && $first_delay <= 5000}
                assert {$second_delay >= 800 && $second_delay <= 5000}
                # Equal jitter keeps capped retries distributed in the 2-4 second range.
                assert {$capped_delay >= 1800 && $capped_delay <= 7000}
                assert {$reset_delay >= 800 && $reset_delay <= 5000}
                lassign [wait_for_log_messages 0 {"*after 1 consecutive failures*"} $log_lines 50 100] _ first_failure_line
                lassign [wait_for_log_messages 0 {"*after 2 consecutive failures*"} $first_failure_line 50 100] _ second_failure_line
                wait_for_log_messages 0 {"*after 3 consecutive failures*"} $second_failure_line 50 100
                wait_for_log_messages 0 {"*after 1 consecutive failures*"} $second_failure_line 50 100
            } {
                $replica replicaof no one
                if {$fake_pid ne ""} {catch {exec kill $fake_pid}}
            }
        }

        test {Disabling full sync backoff cancels an outstanding retry delay} {
            set fake_pid ""
            with_cleanup {
                $replica config set repl-sync-backoff-base-time 10
                $replica config set repl-sync-backoff-max-time 10
                lassign [start_fullsync_sequence_primary $rdb_payload F,H] fake_pid fake_port count_file
                $replica replicaof 127.0.0.1 $fake_port

                wait_for_condition 50 100 {
                    [llength [fake_primary_attempt_times $count_file]] >= 1
                } else {
                    fail "replica did not start the failed full sync"
                }
                $replica config set repl-sync-backoff-max-time 0
                wait_for_condition 30 100 {
                    [llength [fake_primary_attempt_times $count_file]] >= 2
                } else {
                    fail "disabling backoff did not trigger a retry"
                }

                set times [fake_primary_attempt_times $count_file]
                assert {[expr {[lindex $times 1] - [lindex $times 0]}] < 3000}
            } {
                $replica replicaof no one
                if {$fake_pid ne ""} {catch {exec kill $fake_pid}}
            }
        }

        test {Lowering the full sync backoff maximum clamps an outstanding retry delay} {
            set fake_pid ""
            with_cleanup {
                $replica config set repl-sync-backoff-base-time 10
                $replica config set repl-sync-backoff-max-time 60
                lassign [start_fullsync_sequence_primary $rdb_payload F,H] fake_pid fake_port count_file
                $replica replicaof 127.0.0.1 $fake_port

                wait_for_condition 50 100 {
                    [llength [fake_primary_attempt_times $count_file]] >= 1
                } else {
                    fail "replica did not start the failed full sync"
                }
                $replica config set repl-sync-backoff-max-time 5
                wait_for_condition 80 100 {
                    [llength [fake_primary_attempt_times $count_file]] >= 2
                } else {
                    fail "lowering backoff maximum did not trigger a clamped retry"
                }

                set times [fake_primary_attempt_times $count_file]
                assert {[expr {[lindex $times 1] - [lindex $times 0]}] < 7000}
            } {
                $replica replicaof no one
                if {$fake_pid ne ""} {catch {exec kill $fake_pid}}
            }
        }
    }
}

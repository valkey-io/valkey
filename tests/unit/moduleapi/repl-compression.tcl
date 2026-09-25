set testmodule [file normalize tests/modules/blockedclient.so]

tags {"modules repl external:skip"} {

# repl_uncompressed_bytes= from the replica line of the primary's INFO replication.
proc replica_line_uncompressed_bytes {primary} {
    set info [$primary info replication]
    assert {[regexp {repl_uncompressed_bytes=([0-9]+)} $info -> uncompressed_bytes]}
    return $uncompressed_bytes
}

start_server {overrides {save "" repl-compression lz4}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    test {Compressed replication keeps decoding while a module command yields} {
        $primary flushall

        start_server [list overrides [list save "" repl-compression lz4 repl-diskless-load swapdb \
                                           busy-reply-threshold 10 loadmodule $testmodule]] {
            set replica [srv 0 client]
            set replica_pid [srv 0 pid]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 200 {
                [s 0 master_link_status] eq {up} &&
                [string match {*state=online*repl_compression=lz4*} [$primary info replication]]
            } else {
                fail "Compressed replication not established"
            }

            set sync_full_before [status $primary sync_full]
            set sync_partial_before [status $primary sync_partial_ok]
            set busy [valkey_deferring_client]
            set busy_started 0
            set replica_paused 0
            set test_code [catch {
                $busy slow_fg_command 0
                $busy flush
                set busy_started 1
                wait_for_condition 50 20 {
                    [catch {$replica ping} busy_error] && [string match {*BUSY*} $busy_error]
                } else {
                    fail "Module command did not enter its yielding loop"
                }

                set read_offset_before [$replica get_repl_read_offset]
                set uncompressed_before [replica_line_uncompressed_bytes $primary]
                set payload [string repeat x [expr {2 * 1024 * 1024}]]

                # Queue the complete compressed write in the socket before the
                # replica resumes, so no later read event is needed to finish it.
                pause_process $replica_pid
                set replica_paused 1
                $primary set blocked:large $payload
                wait_for_condition 50 20 {
                    [replica_line_uncompressed_bytes $primary] >=
                        $uncompressed_before + [string length $payload]
                } else {
                    fail "Primary did not finish writing the compressed batches"
                }
                resume_process $replica_pid
                set replica_paused 0

                # Replicated commands are not applied while the module command
                # is busy, but decoding must continue past the first 1 MiB
                # scheduling slice while the module yields to the event loop.
                wait_for_condition 50 20 {
                    [$replica get_repl_read_offset] > $read_offset_before + 1024 * 1024
                } else {
                    fail "Compressed replication stopped decoding during a yielding command"
                }
            } test_result test_options]

            if {$replica_paused} {
                resume_process $replica_pid
            }
            if {$busy_started} {
                $replica stop_slow_fg_command
                $busy read
            }
            $busy close
            if {$test_code} {
                return -options $test_options $test_result
            }

            # A fresh read event lets the primary client apply the data that
            # was intentionally left unparsed while the module was busy.
            $primary set blocked:probe delivered
            wait_for_condition 50 100 {
                [$replica get blocked:large] eq $payload &&
                [$replica get blocked:probe] eq {delivered}
            } else {
                fail "Write decoded during the yielding command was not applied"
            }
            assert_equal $sync_full_before [status $primary sync_full]
            assert_equal $sync_partial_before [status $primary sync_partial_ok]

            $replica replicaof no one
        }
    }
}

}

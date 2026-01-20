
start_server {config "minimal.conf" tags {"external:skip"} overrides {enable-debug-command {yes} io-threads 5}} {
    set server_pid [s process_id]
    # Skip if non io-threads mode - as it is relevant only for io-threads mode
    if {[r config get io-threads] ne "io-threads 1"} {
        test {Force prefetching via IO threads and assert active io_thread usage} {
            # Create 16 (prefetch batch size) + 1 clients
            for {set i 0} {$i < 16} {incr i} {
                set rd$i [valkey_deferring_client]
            }
            # set a key that will be later be prefetch
            r set a 0
            # Create a batch of commands by suspending the server for a while
            # before responding to the first command
            pause_process $server_pid
            # Send set commands for all clients except the first
            for {set i 1} {$i < 16} {incr i} {
                [set rd$i] set a $i
                [set rd$i] flush
            }
            # Resume the server
            resume_process $server_pid
            
            # Wait until active io-threads count is 1
            wait_for_condition 1000 50 {
                [getInfoProperty [r info server] io_threads_active] eq 1
            } else {
                fail "Failed to wait for active io_threads to come down to 1"
            }
            
            # Verify non-zero io-threads active usage metric even when there are no currently active io_threads.
            set info [r info]
            set active_threads [getInfoProperty $info io_threads_active]
            assert_equal {1} $active_threads
            set io_threads_count [dict get [r config get io-threads] io-threads]
            for {set i 1} {$i <= $io_threads_count} {incr i} {
                set used_active_time [getInfoProperty $info used_active_time_io_thread_$i]
                if {$i == 1} {
                    assert {$used_active_time > 0}
                } elseif {$i < $io_threads_count} {
                    assert {$used_active_time ne ""}
                } else {
                    assert {$used_active_time eq ""}
                }
            }
        }
    }
}
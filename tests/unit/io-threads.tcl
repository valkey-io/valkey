
start_server {config "minimal.conf" tags {"external:skip"} overrides {enable-debug-command {yes} io-threads 5}} {
    set server_pid [s process_id]
    # Skip if non io-threads mode - as it is relevant only for io-threads mode
    assert_equal {io-threads 5} [r config get io-threads]
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
        
        # Wait until active io_threads are no longer active
        wait_for_condition 1000 50 {
            [getInfoProperty [r info server] io_threads_active] eq 0
        } else {
            fail "Failed to wait until no io_threads are active"
        }
        set info [r info]
        set io_threads_count [dict get [r config get io-threads] io-threads]
        array set initial_active_times {}
        for {set i 1} {$i <= $io_threads_count} {incr i} {
            set used_active_time [getInfoProperty $info used_active_time_io_thread_$i]
            if {$i < $io_threads_count} {
                assert {$used_active_time > 0}
                set initial_active_times($i) $used_active_time
            } else {
                assert {$used_active_time eq ""}
            }
        }

        # Adjust io-threads to a lower value and assert that active io_threads fields are >= values found initially
        assert_equal {OK} [r config set io-threads 1]
        set info [r info]
        wait_for_condition 1000 50 {
            [getInfoProperty [r info server] io_threads_active] eq 0
        } else {
            fail "Failed to wait until no io_threads are active"
        }
        set used_active_time_1 [getInfoProperty $info used_active_time_io_thread_1]
        assert {$used_active_time_1 eq ""}

        # Re-adjust io-threads to a higher value and assert that active io_threads field values aren't reset to 0.
        assert_equal {OK} [r config set io-threads 5]
        set info [r info]
        for {set i 1} {$i <= $io_threads_count} {incr i} {
            set used_active_time [getInfoProperty $info used_active_time_io_thread_$i]
            if {$i < $io_threads_count} {
                assert {$used_active_time >= $initial_active_times($i)}
            } else {
                assert {$used_active_time eq ""}
            }
        }
    }
}
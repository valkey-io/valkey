proc activate_io_threads_and_wait {} {
    set server_pid [s process_id]
    # Create 16 clients
    for {set i 0} {$i < 16} {incr i} {
        set rd($i) [valkey_deferring_client]
    }
    r set a 0
    # Create a batch of commands by suspending the server for a while
    # before responding to the first command
    pause_process $server_pid
    # Send set commands for all clients except the first
    for {set i 1} {$i < 16} {incr i} {
        $rd($i) incr a
        $rd($i) flush
    }
    # Resume the server
    resume_process $server_pid

    # Wait until all the client commands have executed
    wait_for_condition 1000 50 {
        [r get a] eq 15
    } else {
        fail "Failed to apply the incr command for all clients"
    }

    # Wait until active io_threads are no longer active
    wait_for_condition 1000 50 {
        [getInfoProperty [r info server] io_threads_active] eq 0
    } else {
        fail "Failed to wait until no io_threads are active"
    }

    for {set i 0} {$i < 16} {incr i} {
        $rd($i) close
    }
}

start_server {config "minimal.conf" tags {"external:skip" "valgrind:skip"} overrides {enable-debug-command {yes} io-threads 5}} {
    # Skip if non io-threads mode - as it is relevant only for io-threads mode
    assert_equal {io-threads 5} [r config get io-threads]
    test {Force the use of IO threads and assert active IO thread usage} {
        activate_io_threads_and_wait
        set info [r info]
        set io_threads_count [dict get [r config get io-threads] io-threads]
        array set initial_active_times {}
        for {set i 1} {$i <= $io_threads_count} {incr i} {
            set used_active_time [getInfoProperty $info used_active_time_io_thread_$i]
            if {$i < $io_threads_count} {
                assert_morethan $used_active_time 0
                set initial_active_times($i) $used_active_time
            } else {
                assert_equal $used_active_time {}
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
        assert_equal $used_active_time_1 {}

        # Re-adjust io-threads to the previous value.
        assert_equal {OK} [r config set io-threads 5]

        set info [r info]
        for {set i 1} {$i <= $io_threads_count} {incr i} {
            set used_active_time [getInfoProperty $info used_active_time_io_thread_$i]
            if {$i < $io_threads_count} {
                # Assert active thread usage isn't reset to 0.
                assert_morethan $used_active_time 0
            } else {
                assert_equal $used_active_time {}
            }
        }

        # Sleep for a time duration that is significantly longer than how much
        # time each of the io_threads would be active again when reactivated.
        set sleep_time_ms 1000
        after $sleep_time_ms

        # Reactivate io-threads and wait for execution
        activate_io_threads_and_wait

        set info [r info]
        for {set i 1} {$i <= $io_threads_count} {incr i} {
            set used_active_time [getInfoProperty $info used_active_time_io_thread_$i]
            if {$i < $io_threads_count} {
                assert {($used_active_time - $initial_active_times($i)) < ($sleep_time_ms/1000)}
                # Assert that total active time is lower than the sleep duration assumed
                assert {$used_active_time < ($sleep_time_ms/1000)}
            } else {
                assert_equal $used_active_time {}
            }
        }
    }
}
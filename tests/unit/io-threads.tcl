
start_server {config "minimal.conf" tags {"external:skip"} overrides {enable-debug-command {yes}}} {
    set server_pid [s process_id]
    # Skip if non io-threads mode - as it is relevant only for io-threads mode
    if {[r config get io-threads] ne "io-threads 1"} {
        test {Force prefetching via IO threads and assert active io_thread usage and uptime metrics} {
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
            # Verify the final state
            $rd15 get a
            assert_equal {OK} [$rd15 read]
            assert_equal {15} [$rd15 read]
            set info [r info]
            # Verify non-zero io-threads active usage metric
            set used_active_time_io_thread_1 [getInfoProperty $info used_active_time_io_thread_1]
            assert {$used_active_time_io_thread_1 > 0}
        }
    }
}
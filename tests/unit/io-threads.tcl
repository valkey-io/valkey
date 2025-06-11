source tests/support/cli.tcl

if {$::io_threads} {

    start_server {config "minimal.conf" tags {"external:skip"} overrides {enable-debug-command {yes}}} {
        
        set server_pid [s process_id]
        
        test {prefetch works as expected when killing a client from the middle of prefetch commands batch} {
            # Create 16 (prefetch batch size) +1 clients
            for {set i 0} {$i < 16} {incr i} {
                set rd$i [valkey_deferring_client]
            }
        
            # set a key that will be later be prefetch
            r set a 0
        
            # Get the client ID of rd4
            $rd4 client id
            set rd4_id [$rd4 read]
        
            # Create a batch of commands by suspending the server for a while
            # before responding to the first command
            pause_process $server_pid
        
            # The first client will kill the fourth client
            $rd0 client kill id $rd4_id
        
            # Send set commands for all clients except the first
            for {set i 1} {$i < 16} {incr i} {
                [set rd$i] set a $i
                [set rd$i] flush
            }
        
            # Resume the server
            resume_process $server_pid
        
            # Read the results
            assert_equal {1} [$rd0 read]
            catch {$rd4 read} err
            assert_match {I/O error reading reply} $err
        
            # verify the prefetch stats are as expected
            set info [r info stats]
            set prefetch_entries [getInfoProperty $info io_threaded_total_prefetch_entries]
            assert_range $prefetch_entries 2 15; # With slower machines, the number of prefetch entries can be lower
            set prefetch_batches [getInfoProperty $info io_threaded_total_prefetch_batches]
            assert_range $prefetch_batches 1 7; # With slower machines, the number of batches can be higher
        
            # Verify the final state
            $rd15 get a
            assert_equal {OK} [$rd15 read]
            assert_equal {15} [$rd15 read]
        }
        
        test {prefetch works as expected when changing the batch size while executing the commands batch} {
            # Create 16 (default prefetch batch size) clients
            for {set i 0} {$i < 16} {incr i} {
                set rd$i [valkey_deferring_client]
            }
        
            # Create a batch of commands by suspending the server for a while
            # before responding to the first command
            pause_process $server_pid
        
            # Send set commands for all clients the 5th client will change the prefetch batch size
            for {set i 0} {$i < 16} {incr i} {
                if {$i == 4} {
                    [set rd$i] config set prefetch-batch-max-size 1
                }
                [set rd$i] set a $i
                [set rd$i] flush
            }
            # Resume the server
            resume_process $server_pid
            # Read the results
            for {set i 0} {$i < 16} {incr i} {
                assert_equal {OK} [[set rd$i] read]
            }
            
            # assert the configured prefetch batch size was changed
            assert {[r config get prefetch-batch-max-size] eq "prefetch-batch-max-size 1"}
        }
          
        test {no prefetch when the batch size is set to 0} {
            # set the batch size to 0
            r config set prefetch-batch-max-size 0
            # save the current value of prefetch entries
            set info [r info stats]
            set prefetch_entries [getInfoProperty $info io_threaded_total_prefetch_entries]
            
            # Create 16 (default prefetch batch size) clients
            for {set i 0} {$i < 16} {incr i} {
                set rd$i [valkey_deferring_client]
            }
        
            # Create a batch of commands by suspending the server for a while
            # before responding to the first command
            pause_process $server_pid
        
            # Send set commands for all clients
            for {set i 0} {$i < 16} {incr i} {
                [set rd$i] set a $i
                [set rd$i] flush
            }
        
            # Resume the server
            resume_process $server_pid
        
            # Read the results
            for {set i 0} {$i < 16} {incr i} {
                assert_equal {OK} [[set rd$i] read]
            }
            
            # assert the prefetch entries did not change
            set info [r info stats]
            set new_prefetch_entries [getInfoProperty $info io_threaded_total_prefetch_entries]
            assert_equal $prefetch_entries $new_prefetch_entries
        }
    }

    start_server {} {
        start_server {} {
            test {replicas writes are offloaded to IO threads} {
                set primary [srv -1 client]
                set primary_host [srv -1 host]
                set primary_port [srv -1 port]
        
                set replica [srv 0 client]
                $replica replicaof $primary_host $primary_port
    
            wait_for_condition 500 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }
            
            # get the current io_threaded_writes_processed
            set info [$primary info stats]
            set io_threaded_writes_processed [getInfoProperty $info io_threaded_writes_processed]
            
            # Send a write command to the primary
            $primary set a 1
    
            # Wait for the write to be propagated to the replica
            wait_for_condition 50 100 {
                [$replica get a] eq {1}
            } else {
                fail "Replication not propagated."
            }
            
            # Get the new io_threaded_writes_processed
            set info [$primary info stats]
            set new_io_threaded_writes_processed [getInfoProperty $info io_threaded_writes_processed]
            # Assert new is old + 3, 3 for the write to the info-client, set-client and to the replica.
            assert {$new_io_threaded_writes_processed >= $io_threaded_writes_processed + 3} ;
    
            # Verify the write was propagated to the replica
            assert_equal {1} [$replica get a]
        }
    }
}


    ### Commands Offloading tests ###

    proc get_offloaded_commands {r} {
        # Get the current IO thread stats
        set info [$r info stats]
        return [getInfoProperty $info io_threaded_commands_processed]
    }
    
    start_cluster 1 0 {config "minimal.conf" tags {"external:skip"} overrides {enable-debug-command {yes}}} {
        wait_for_cluster_state ok
        set server_pid [s process_id]
        
        # Skip if non io-threads mode - as it is relevant only for io-threads mode
        test {Pipeline commands are not offloaded with IO threads} {
            # This test verifies that pipeline commands are not offloaded to IO threads

            set initial_offloaded_commands [get_offloaded_commands r]
            # Create a client and send pipeline commands
            set rd [valkey_deferring_client]

            # Send a batch of commands in pipeline mode
            $rd write [format_command GET nonexistent_key]
            $rd write [format_command SET test value1]
            $rd write [format_command GET test]
            $rd write [format_command INCR counter]
            $rd write [format_command GET counter]
            $rd flush
            
            # Read all responses
            assert_equal {} [$rd read] "GET nonexistent_key should return empty"
            assert_equal {OK} [$rd read]
            assert_equal {value1} [$rd read]
            assert_equal {1} [$rd read]
            assert_equal {1} [$rd read]
    
            # Get the updated IO thread stats
            set updated_offloaded_commands [get_offloaded_commands r]
            # The pipeline commands should not have been offloaded to IO threads only the last GET command
            # So the number of commands processed by IO threads should be increment by 1
            assert_equal [expr {$initial_offloaded_commands + 1}] $updated_offloaded_commands;

            # Verify that non-pipelined commands are offloaded
            assert_equal {OK} [r SET regular_command value2]
            assert_equal {value2} [r GET regular_command]

            # The non-pipelined command should have been offloaded
            # So the number of commands processed by IO threads should increase
            set final_offloaded_commands [get_offloaded_commands r]
            assert {$final_offloaded_commands > $updated_offloaded_commands}
        }

        test {Offloaded command returns wrong type error for incorrect key type} {
            set errs_cnt 0
            set updated_errs_cnt 0
            
            # Get initial stats
            set info_stats [r info]
            regexp {count=(\d*)} [getInfoProperty $info_stats errorstat_WRONGTYPE] _ errs_cnt
            set initial_posted_jobs [getInfoProperty $info_stats io_threaded_postponed_jobs_to_mainthread]
            set initial_offloaded_commands [get_offloaded_commands r]

            # set a HASH with a key and value
            r HSET key_1 field_a val_a
    
            # Verify we get error response for a GET command on a HASH type key
            assert_error {*WRONGTYPE*} {r GET key_1}

            # Get updated stats
            set updated_info_stats [r INFO]    
            set updated_posted_jobs [getInfoProperty $updated_info_stats io_threaded_postponed_jobs_to_mainthread]
            regexp {count=(\d*)} [getInfoProperty $updated_info_stats errorstat_WRONGTYPE] _ updated_errs_cnt
            set updated_offloaded_commands [get_offloaded_commands r]

            # Check that the get command was offloaded to io thread and error stat was updated
            assert_equal 1 [expr {$updated_posted_jobs - $initial_posted_jobs}]
            assert_equal 1 [expr {$updated_errs_cnt - $errs_cnt}]
            assert_equal 1 [expr {$updated_offloaded_commands - $initial_offloaded_commands}]
        }

        test {Read commands are offloaded to IO threads} {
            # Set up some test data
            r SET key1 value1
            r SET key2 value2
            r HSET hash_key field1 value1 field2 value2
            
            # Get the initial IO thread stats
            set initial_offloaded_commands [get_offloaded_commands r]
            # Execute a series of read commands that should be offloaded
            r GET key1
            r GET key2
            r HGET hash_key field1

            # Verify that the commands were offloaded (processed count should increase by 3)
            set updated_offloaded_commands [get_offloaded_commands r]
            assert_equal [expr {$initial_offloaded_commands + 3}] $updated_offloaded_commands
        }

        test {Write commands are not offloaded to IO threads} {
            # Get the initial IO thread stats
            set initial_offloaded_commands [get_offloaded_commands r]

            # Execute a series of write commands
            r SET write_key1 value1
            r HSET write_hash field1 value1
            r INCR write_counter

            # Verify that the write commands were not offloaded
            set updated_offloaded_commands [get_offloaded_commands r]
            assert_equal $initial_offloaded_commands $updated_offloaded_commands
        }

        test {Commands not marked to be offloaded are not offloaded} {
            # Get the initial IO thread stats
            set initial_offloaded_commands [get_offloaded_commands r]
    
            # Execute commands with side effects
            r PUBLISH channel message
            r CLIENT LIST
            r INFO

            # Verify that these commands were not offloaded
            set updated_offloaded_commands [get_offloaded_commands r]
            assert_equal $initial_offloaded_commands $updated_offloaded_commands
        }
        
        test {Command offloading can be disabled via configuration} {
            # This test verifies that command offloading can be disabled        
            # Set up test data
            r set config_test_key value
            
            # Get the initial IO thread stats
            set initial_offloaded_commands [get_offloaded_commands r]
    
            # Execute a read command that would normally be offloaded
            r get config_test_key

            # Get the stats after first command
            set mid_offloaded_commands [get_offloaded_commands r]

            # Verify the command was offloaded
            assert_equal [expr {$initial_offloaded_commands + 1}] $mid_offloaded_commands

            # Disable command offloading
            r config set io-threads-do-commands-offloading no

            # Execute another read command
            r get config_test_key

            # Get the final stats
            set final_offloaded_commands [get_offloaded_commands r]

            # Verify the command was not offloaded after disabling
            assert_equal $mid_offloaded_commands $final_offloaded_commands

            # Re-enable command offloading for other tests
            r config set io-threads-do-commands-offloading yes
        }

        test {Key expiry is postponed when read from main thread} {
            # This test verifies that when a key with expiry is read from the IO thread,
            # its expiry deletion is postponed to the main-thread to prevent race conditions with the main-thread
            set updated_info_stats [r INFO]    
            set initial_postponed [getInfoProperty $updated_info_stats io_threaded_postponed_jobs_to_mainthread]

            # Set a key with a short expiry time (2 seconds)
            r set key "val"
            r expire key 2

            # Verify the key exists
            assert_equal "val" [r get key]

            # Wait for the key to be expired
            after 2000

            # Read the key (this should trigger expiry postponement)
            # The answer should be empty
            assert_equal {} [r get key]

            # Check if postponed jobs counter increased
            set updated_info_stats [r INFO]    
            set updated_postponed [getInfoProperty $updated_info_stats io_threaded_postponed_jobs_to_mainthread]
            # The postponed jobs counter should have increased if expiry was postponed
            assert {$updated_postponed > $initial_postponed}
        }

        test "test io-threads are runtime modifiable" {
            # Randomly set the number of threads between 1 and 5
            for {set i 0} {$i < 100} {incr i} {
                set random_num [expr {int(rand() * 5) + 1}]
                r config set io-threads $random_num
                set thread_num [lindex [r config get io-threads] 1]
                assert_equal $random_num $thread_num
            }
        }
    }
}

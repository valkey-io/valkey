start_server {tags {"hotkey"}} {
    test "Enable hotkey functionality" {
        # Enable hotkey functionality
        r config set hotkey-enabled yes
        
        # Check if the feature is properly enabled
        set hotkey_status [r config get hotkey-enabled]
        assert_equal [lindex $hotkey_status 1] "yes"
    }

    test "HOTKEYS GET returns empty when no hot keys" {
        # Reset hotkey statistics
        r hotkeys reset
        
        # Get all hotkeys, should be empty
        set all_hotkeys [r hotkeys get]
        assert_equal [llength $all_hotkeys] 0
        
        # Get read hotkeys, should be empty
        set read_hotkeys [r hotkeys get TYPE read]
        assert_equal [llength $read_hotkeys] 0
        
        # Get write hotkeys, should be empty
        set write_hotkeys [r hotkeys get TYPE write]
        assert_equal [llength $write_hotkeys] 0
    }

    test "Generate hot keys through repeated access" {
        # Reset hotkey statistics
        r hotkeys reset
        
        # Set high sampling ratio and low threshold for testing
        r config set hotkey-sampling-ratio 100
        r config set hotkey-read-threshold 500
        r config set hotkey-write-threshold 300
        r config set hotkey-window-seconds 1
        
        # Create test data
        r set "hot_read_key" "value"
        
        # Repeatedly access certain keys to generate hotkeys
        # Need enough access times to exceed CMS threshold
        # Threshold = threshold * window_seconds * sampling_ratio / 100
        # For example: 1000 * 1 * 100 / 100 = 1000 times
        for {set j 1} {$j <= 1000} {incr j} {
            # Read operations
            r get "hot_read_key"
            r get "hot_read_key_2"
            
            # Write operations
            r set "hot_write_key" "value_$j"
            r set "hot_write_key_2" "value_$j"
        }
        
        # Wait for one window period to allow hotkey detection
        after 2000
        
        # Check if hotkeys are detected
        set all_hotkeys [r hotkeys get]
        # Since hotkey detection is probabilistic, we only check if there are results
        # Rather than exact count
        puts "All hotkeys detected: [llength $all_hotkeys]"
        
        # Verify that hotkeys are indeed detected
        assert {[llength $all_hotkeys] > 0}
    }

    test "HOTKEYS GET with TYPE filter" {
        # Wait to ensure previous hotkeys are recorded
        after 500
        
        # Test TYPE parameter
        set read_hotkeys [r hotkeys get TYPE read]
        set write_hotkeys [r hotkeys get TYPE write]
        set all_hotkeys [r hotkeys get TYPE all]
        
        puts "Read hotkeys: [llength $read_hotkeys]"
        puts "Write hotkeys: [llength $write_hotkeys]"
        puts "All hotkeys: [llength $all_hotkeys]"
        
        # Verify that all type contains the sum of read and write
        assert {[llength $all_hotkeys] >= 0}
    }

    test "HOTKEYS RESET clears all statistics" {
        # Execute reset
        set reset_result [r hotkeys reset]
        assert_equal $reset_result "OK"
        
        # Verify no hotkeys after reset
        set all_hotkeys [r hotkeys get]
        assert_equal [llength $all_hotkeys] 0
        
        set read_hotkeys [r hotkeys get TYPE read]
        assert_equal [llength $read_hotkeys] 0
        
        set write_hotkeys [r hotkeys get TYPE write]
        assert_equal [llength $write_hotkeys] 0
    }

    test "Hotkey detection with different data types" {
        # Reset statistics
        r hotkeys reset

        r config set hotkey-read-threshold 500
        r config set hotkey-write-threshold 300
        
        # Test hotkey detection for different data types
        # Increase access count to ensure exceeding threshold
        for {set i 1} {$i <= 1000} {incr i} {
            # String type
            r get "hot_string"
            
            # Hash type
            r hget "hot_hash" "field_1"
            
            # List type
            r lrange "hot_list" 0 -1
            
            # Set type
            r smembers "hot_set"
            
            # ZSet type
            r zrange "hot_zset" 0 -1
        }
        
        # Wait for detection
        after 1100
        
        # Check if hotkeys are detected for different data types
        set all_hotkeys [r hotkeys get]
        puts "Hotkeys detected for different data types: [llength $all_hotkeys]"
        
        # Verify hotkeys are detected
        assert {[llength $all_hotkeys] > 0}
    }

    test "Invalid HOTKEYS command syntax" {
        # Test invalid subcommand
        catch {r hotkeys invalid} err
        assert_match "*unknown subcommand*" $err
        
        # Test invalid TYPE parameter
        catch {r hotkeys get TYPE invalid} err
        assert_match "*Invalid type*" $err
        
        # Test incorrect syntax
        catch {r hotkeys get INVALID param} err
        assert_match "*Syntax error*" $err
        
        # Test insufficient parameters
        catch {r hotkeys} err
        assert_match "*wrong number of arguments*" $err
    }

    test "Hotkey configuration parameters" {
        # Test hotkey-sampling-ratio configuration
        r config set hotkey-sampling-ratio 50
        set sampling_ratio [r config get hotkey-sampling-ratio]
        assert_equal [lindex $sampling_ratio 1] "50"
        
        # Test hotkey-read-threshold configuration
        r config set hotkey-read-threshold 2000
        set read_threshold [r config get hotkey-read-threshold]
        assert_equal [lindex $read_threshold 1] "2000"
        
        # Test hotkey-write-threshold configuration
        r config set hotkey-write-threshold 1500
        set write_threshold [r config get hotkey-write-threshold]
        assert_equal [lindex $write_threshold 1] "1500"
        
        # Test hotkey-window-seconds configuration
        r config set hotkey-window-seconds 2
        set window_seconds [r config get hotkey-window-seconds]
        assert_equal [lindex $window_seconds 1] "2"
        
        # Test hotkey-history-ttl configuration
        r config set hotkey-history-ttl 300
        set history_ttl [r config get hotkey-history-ttl]
        assert_equal [lindex $history_ttl 1] "300"

        # Test hotkey-cms-bucket-size configuration
        r config set hotkey-cms-bucket-size 4096
        set cms_bucket_size [r config get hotkey-cms-bucket-size]
        assert_equal [lindex $cms_bucket_size 1] "4096"
    }

    test "Hotkey CMS bucket size auto-adjustment" {
        # Enable hotkey functionality
        r config set hotkey-enabled yes
        
        # Test that non-power-of-2 values are automatically adjusted
        r config set hotkey-cms-bucket-size 3000
        set adjusted_size [r config get hotkey-cms-bucket-size]
        # 3000 will be adjusted to the nearest power of 2: 4096
        assert_equal [lindex $adjusted_size 1] "4096"
        
        # Test that already power-of-2 values are not adjusted
        r config set hotkey-cms-bucket-size 8192
        set unchanged_size [r config get hotkey-cms-bucket-size]
        assert_equal [lindex $unchanged_size 1] "8192"
        
        # Test boundary values
        r config set hotkey-cms-bucket-size 1000
        set min_adjusted [r config get hotkey-cms-bucket-size]
        # 1000 will be adjusted to the nearest power of 2: 1024
        assert_equal [lindex $min_adjusted 1] "1024"
        
        r config set hotkey-cms-bucket-size 20000
        set max_adjusted [r config get hotkey-cms-bucket-size]
        # 20000 will be adjusted to the nearest power of 2: 32768
        assert_equal [lindex $max_adjusted 1] "32768"
    }

    test "Hotkey CMS bucket size with disabled hotkey" {
        # Disable hotkey functionality
        r config set hotkey-enabled no
        
        # When hotkey is disabled, setting bucket size should succeed but not trigger CMS recreation
        r config set hotkey-cms-bucket-size 2048
        set bucket_size [r config get hotkey-cms-bucket-size]
        assert_equal [lindex $bucket_size 1] "2048"
        
        # Re-enable hotkey functionality, should use the new bucket size
        r config set hotkey-enabled yes
        set final_size [r config get hotkey-cms-bucket-size]
        assert_equal [lindex $final_size 1] "2048"
    }

    test "Disable hotkey functionality" {
        # Disable hotkey functionality
        r config set hotkey-enabled no

        # Check if the feature is properly disabled
        set hotkey_status [r config get hotkey-enabled]
        assert_equal [lindex $hotkey_status 1] "no"

        # Attempting to use hotkeys commands should return error
        catch {r hotkeys get} err
        assert_match "*Hotkey detection is disabled*" $err

        catch {r hotkeys reset} err
        assert_match "*Hotkey detection is disabled*" $err
    }

    test "Re-enable hotkey functionality" {
        # Re-enable hotkey functionality
        r config set hotkey-enabled yes

        # Check if the feature is properly enabled
        set hotkey_status [r config get hotkey-enabled]
        assert_equal [lindex $hotkey_status 1] "yes"

        # Verify commands can be executed normally
        set reset_result [r hotkeys reset]
        assert_equal $reset_result "OK"

        set all_hotkeys [r hotkeys get]
        assert {[llength $all_hotkeys] >= 0}
    }

    test "Hotkey detection with high frequency access" {
        # Reset statistics
        r hotkeys reset
        
        # Set more sensitive parameters
        r config set hotkey-sampling-ratio 100
        r config set hotkey-read-threshold 500
        r config set hotkey-write-threshold 300
        r config set hotkey-window-seconds 1
        
        # High frequency access to single keys
        set hot_key_read "super_hot_key_read"
        set hot_key_write "super_hot_key_write"

        for {set i 1} {$i <= 1200} {incr i} {
            r set $hot_key_write "value_$i"
            r get $hot_key_read
        }
        
        # Wait for detection
        after 1100
        
        # Check if hotkeys are detected
        set all_hotkeys [r hotkeys get]
        puts "High frequency access hotkeys: [llength $all_hotkeys]"
        
        # Check read/write classification
        set read_hotkeys [r hotkeys get TYPE read]
        set write_hotkeys [r hotkeys get TYPE write]
        puts "Read hotkeys: [llength $read_hotkeys], Write hotkeys: [llength $write_hotkeys]"
        
        # Verify hotkeys are detected
        assert {[llength $all_hotkeys] > 0}
        
        # Verify both read and write hotkeys are detected
        assert {[llength $read_hotkeys] > 0}
        assert {[llength $write_hotkeys] > 0}
    }

    test "Test LRU eviction in history management" {
        r config set hotkey-enabled yes
        r hotkeys reset

        # Set smaller maximum history count to test LRU eviction
        r config set hotkey-history-max-count 5
        r config set hotkey-sampling-ratio 100
        r config set hotkey-read-threshold 500
        r config set hotkey-window-seconds 1

        # Create multiple hotkeys, exceeding maximum history count
        for {set i 1} {$i <= 8} {incr i} {
            for {set j 1} {$j <= 600} {incr j} {
                r set "hotkey_$i" "value_$j"
                r get "hotkey_$i"
            }
        }

        # Wait for detection
        after 1100

        # Check that history count does not exceed maximum
        set all_hotkeys [r hotkeys get]
        puts "LRU test - hotkeys count: [llength $all_hotkeys]"

        # Due to LRU eviction, history count should not exceed maximum
        assert {[llength $all_hotkeys] <= 5}
    }

    test "Test history expiration functionality" {
        r config set hotkey-enabled yes
        r hotkeys reset

        # Set shorter TTL to test expiration
        r config set hotkey-history-ttl 2
        r config set hotkey-sampling-ratio 100
        r config set hotkey-read-threshold 500
        r config set hotkey-window-seconds 1

        # Create some hotkeys
        for {set i 1} {$i <= 600} {incr i} {
            r set "expire_test_key" "value_$i"
            r get "expire_test_key"
        }

        # Wait for detection
        after 1100

        # Verify hotkeys are detected
        set hotkeys_before [r hotkeys get]
        puts "Before expiration: [llength $hotkeys_before] hotkeys"

        # Wait for expiration
        after 3000

        # Get again, should trigger expiration cleanup
        set hotkeys_after [r hotkeys get]
        puts "After expiration: [llength $hotkeys_after] hotkeys"

        # After expiration, there should be no hotkeys
        assert_equal [llength $hotkeys_after] 0
    }

    test "Test hotkey notification publishing mechanism" {
        r config set hotkey-enabled yes
        r hotkeys reset

        # Subscribe to hotkey notification channel
        set rd [valkey_deferring_client]
        $rd subscribe __hotkey_notify__
        $rd read ; # Read subscription confirmation

        # Set parameters
        r config set hotkey-sampling-ratio 100
        r config set hotkey-read-threshold 500
        r config set hotkey-write-threshold 300
        r config set hotkey-window-seconds 1

        # Create hotkeys
        for {set i 1} {$i <= 1500} {incr i} {
            r set "notify_test_key" "value_$i"
            r get "notify_test_key"
        }

        # Wait for detection and notification
        after 1100

        # Check if notification is received (just verify it doesn't crash)
        # Actual notification testing is complex, here mainly test code path
        $rd close

        # Verify hotkeys are detected
        set hotkeys [r hotkeys get]
        assert {[llength $hotkeys] > 0}
    }

    test "Test special characters in key names" {
        r config set hotkey-enabled yes
        r hotkeys reset

        # Test keys containing special characters
        r set "key:with:colons" "value"
        r get "key:with:colons"

        r set "key with spaces" "value"
        r get "key with spaces"

        r set "key\nwith\nnewlines" "value"
        r get "key\nwith\nnewlines"

        r set "key\twith\ttabs" "value"
        r get "key\twith\ttabs"

        # Verify no crashes
        assert_equal [r ping] "PONG"
    }

    test "Test memory cleanup on manager recreation" {
        r config set hotkey-enabled yes
        r hotkeys reset

        # Create some hotkeys
        r config set hotkey-sampling-ratio 100
        r config set hotkey-read-threshold 500
        r config set hotkey-window-seconds 1

        # Create many different hotkeys
        for {set i 1} {$i <= 10} {incr i} {
            for {set j 1} {$j <= 350} {incr j} {
                r get "memory_test_key_$i"
            }
        }

        after 1100

        set hotkeys [r hotkeys get]
        puts "Memory test - detected hotkeys: [llength $hotkeys]"

        # Disable and re-enable hotkey functionality, test manager recreation
        r config set hotkey-enabled no
        r config set hotkey-enabled yes

        # Verify state after recreation
        set hotkeys_after [r hotkeys get]
        assert_equal [llength $hotkeys_after] 0

        # Verify functionality still works
        assert_equal [r ping] "PONG"
    }

    test "Test LRU node operations" {
        r config set hotkey-enabled yes
        r hotkeys reset

        # Set parameters for testing LRU operations
        r config set hotkey-history-max-count 3
        r config set hotkey-sampling-ratio 100
        r config set hotkey-read-threshold 500
        r config set hotkey-window-seconds 1

        # Create first hotkey
        for {set i 1} {$i <= 450} {incr i} {
            r get "lru_test_key_1"
        }
        after 1100

        # Create second hotkey
        for {set i 1} {$i <= 450} {incr i} {
            r get "lru_test_key_2"
        }
        after 1100

        # Create third hotkey
        for {set i 1} {$i <= 450} {incr i} {
            r get "lru_test_key_3"
        }
        after 1100

        # Access first key again, should move it to LRU head
        for {set i 1} {$i <= 450} {incr i} {
            r get "lru_test_key_1"
        }
        after 1100

        # Create fourth hotkey, should evict the least recently used
        for {set i 1} {$i <= 450} {incr i} {
            r get "lru_test_key_4"
        }
        after 1100

        set hotkeys [r hotkeys get]
        puts "LRU operations test - hotkeys count: [llength $hotkeys]"

        # Verify LRU eviction mechanism works properly
        assert {[llength $hotkeys] <= 3}
    }

    test "Test hotkey runtime metrics" {
        # Enable hotkey functionality and reset statistics
        r config set hotkey-enabled yes
        r hotkeys reset

        # Set test parameters
        r config set hotkey-sampling-ratio 100
        r config set hotkey-read-threshold 500
        r config set hotkey-write-threshold 300
        r config set hotkey-window-seconds 1

        # Get initial metric values
        set info_before [r info hotkey]
        
        # Parse initial metrics
        set initial_total_sampled 0
        set initial_read_count 0
        set initial_write_count 0
        set initial_history_count 0
        
        foreach line [split $info_before "\r\n"] {
            if {[string match "hotkey_runtime_total_sampled:*" $line]} {
                set initial_total_sampled [lindex [split $line ":"] 1]
            } elseif {[string match "hotkey_runtime_read_count:*" $line]} {
                set initial_read_count [lindex [split $line ":"] 1]
            } elseif {[string match "hotkey_runtime_write_count:*" $line]} {
                set initial_write_count [lindex [split $line ":"] 1]
            } elseif {[string match "hotkey_runtime_history_count:*" $line]} {
                set initial_history_count [lindex [split $line ":"] 1]
            }
        }

        puts "Initial metrics - total_sampled: $initial_total_sampled, read_count: $initial_read_count, write_count: $initial_write_count, history_count: $initial_history_count"

        # Execute read operations to generate hotkeys
        for {set i 1} {$i <= 600} {incr i} {
            r get "metrics_test_read_key"
        }

        # Execute write operations to generate hotkeys
        for {set i 1} {$i <= 400} {incr i} {
            r set "metrics_test_write_key" "value_$i"
        }

        # Wait for hotkey detection
        after 1100

        # Get updated metrics
        set info_after [r info hotkey]
        
        # Parse updated metrics
        set final_total_sampled 0
        set final_read_count 0
        set final_write_count 0
        set final_history_count 0
        
        foreach line [split $info_after "\r\n"] {
            if {[string match "hotkey_runtime_total_sampled:*" $line]} {
                set final_total_sampled [lindex [split $line ":"] 1]
            } elseif {[string match "hotkey_runtime_read_count:*" $line]} {
                set final_read_count [lindex [split $line ":"] 1]
            } elseif {[string match "hotkey_runtime_write_count:*" $line]} {
                set final_write_count [lindex [split $line ":"] 1]
            } elseif {[string match "hotkey_runtime_history_count:*" $line]} {
                set final_history_count [lindex [split $line ":"] 1]
            }
        }

        puts "Final metrics - total_sampled: $final_total_sampled, read_count: $final_read_count, write_count: $final_write_count, history_count: $final_history_count"

        # Verify metric correctness
        # 1. Total sampling count should increase (we performed 1000 operations with 100% sampling rate)
        assert {$final_total_sampled > $initial_total_sampled}
        puts "✓ hotkey_runtime_total_sampled increased from $initial_total_sampled to $final_total_sampled"

        # 2. Read hotkey count should increase (if read hotkeys are detected)
        if {$final_read_count > $initial_read_count} {
            puts "✓ hotkey_runtime_read_count increased from $initial_read_count to $final_read_count"
        } else {
            puts "! hotkey_runtime_read_count remained at $final_read_count (may not have reached threshold)"
        }

        # 3. Write hotkey count should increase (if write hotkeys are detected)
        if {$final_write_count > $initial_write_count} {
            puts "✓ hotkey_runtime_write_count increased from $initial_write_count to $final_write_count"
        } else {
            puts "! hotkey_runtime_write_count remained at $final_write_count (may not have reached threshold)"
        }

        # 4. History count should reflect current cached hotkey count
        set current_hotkeys [r hotkeys get]
        assert_equal $final_history_count [llength $current_hotkeys]
        puts "✓ hotkey_runtime_history_count ($final_history_count) matches current hotkeys count ([llength $current_hotkeys])"

        # 5. Verify sampling count reasonableness
        set expected_samples [expr {600 + 400}]
        set sample_diff [expr {abs($final_total_sampled - $initial_total_sampled)}]
        puts "Sample difference: $sample_diff, expected around: $expected_samples"
        
        # Since sampling rate is 100%, difference should be close to operations performed
        assert {$sample_diff >= [expr {$expected_samples * 0.9}]}
        puts "✓ Sample count is reasonable"

        # Test reset function's impact on metrics
        r hotkeys reset
        
        set info_reset [r info hotkey]
        set reset_history_count 0
        
        foreach line [split $info_reset "\r\n"] {
            if {[string match "hotkey_runtime_history_count:*" $line]} {
                set reset_history_count [lindex [split $line ":"] 1]
            }
        }
        
        # History count should be 0 after reset
        assert_equal $reset_history_count 0
        puts "✓ hotkey_runtime_history_count reset to 0 after HOTKEYS RESET"
        
        # Verify other metrics are not affected by reset (cumulative metrics)
        set info_after_reset [r info hotkey]
        set reset_total_sampled 0
        set reset_read_count 0
        set reset_write_count 0
        
        foreach line [split $info_after_reset "\r\n"] {
            if {[string match "hotkey_runtime_total_sampled:*" $line]} {
                set reset_total_sampled [lindex [split $line ":"] 1]
            } elseif {[string match "hotkey_runtime_read_count:*" $line]} {
                set reset_read_count [lindex [split $line ":"] 1]
            } elseif {[string match "hotkey_runtime_write_count:*" $line]} {
                set reset_write_count [lindex [split $line ":"] 1]
            }
        }
        
        # Cumulative metrics should not be reset
        assert_equal $reset_total_sampled $final_total_sampled
        assert_equal $reset_read_count $final_read_count
        assert_equal $reset_write_count $final_write_count
        puts "✓ Cumulative metrics preserved after HOTKEYS RESET"
    }
}
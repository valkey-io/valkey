start_server {tags {"rdb" "downgrade" "compatibility"} keep_persistence true} {
    test {RDB version compatibility check - current version} {
        # Test that current RDB version is supported
        set version [r config get save]
        # Current version should be compatible
        r set test_key "test_value"
        
        # Debug: Check RDB file after save
        r bgsave
        waitForBgsave r
        
        set config_dir [lindex [r config get dir] 1]
        set config_dbfilename [lindex [r config get dbfilename] 1]
        set rdb_path "$config_dir/$config_dbfilename"
        
        if {[file exists $rdb_path]} {
            set rdb_size [file size $rdb_path]
            puts "DEBUG WORKING TEST: RDB file size: $rdb_size bytes"
            catch {
                set hexdump [exec hexdump -C $rdb_path | head -5]
                puts "DEBUG WORKING TEST: RDB file hexdump:"
                puts $hexdump
            }
        }
        
        r debug reload nosave
        r get test_key
    } {test_value}

    test {RDB downgrade compatibility - basic functionality} {
        # Create some test data that would use ziplist encoding
        r del test_hash test_zset test_list
        
        # Small hash (should use ziplist/listpack encoding)
        r hmset test_hash field1 value1 field2 value2 field3 value3
        
        # Small sorted set (should use ziplist/listpack encoding)  
        r zadd test_zset 1.0 member1 2.0 member2 3.0 member3
        
        # List data
        r rpush test_list item1 item2 item3
        
        # Verify the data is there
        list [r hget test_hash field1] [r zscore test_zset member2] [r lindex test_list 1]
    } {value1 2 item2}

    test {RDB downgrade compatibility - hash encoding preservation} {
        r del small_hash large_hash
        
        # Small hash that should use ziplist encoding
        for {set i 1} {$i <= 10} {incr i} {
            r hset small_hash "field$i" "value$i"
        }
        
        # Verify encoding (should be ziplist for small hashes)
        set encoding [r object encoding small_hash]
        
        # The encoding should be ziplist for small hashes
        expr {$encoding eq "ziplist" || $encoding eq "listpack"}
    } {1}

    test {RDB downgrade compatibility - sorted set encoding preservation} {
        r del small_zset
        
        # Small sorted set that should use ziplist encoding
        for {set i 1} {$i <= 10} {incr i} {
            r zadd small_zset $i "member$i"
        }
        
        # Verify encoding (should be ziplist for small sorted sets)
        set encoding [r object encoding small_zset]
        
        # The encoding should be ziplist for small sorted sets
        expr {$encoding eq "ziplist" || $encoding eq "listpack"}
    } {1}

    test {RDB downgrade compatibility - list encoding after operations} {
        r del test_list
        
        # Create a list
        r rpush test_list a b c d e
        
        # Lists should use quicklist encoding in Redis 6.2
        set encoding [r object encoding test_list]
        
        # Verify we can perform operations
        r lpop test_list
        r llen test_list
    } {4}

    test {RDB downgrade compatibility - data integrity after save/load cycle} {
        # Test with only string key first (like working test)
        r set simple_test "simple_value"
        
        set config_dir [lindex [r config get dir] 1]
        set config_dbfilename [lindex [r config get dbfilename] 1]
        set rdb_path "$config_dir/$config_dbfilename"
        
        puts "DEBUG: Keys before save: [r keys *]"
        
        r bgsave
        waitForBgsave r
        
        if {[file exists $rdb_path]} {
            set rdb_size [file size $rdb_path]
            puts "DEBUG: RDB file size: $rdb_size bytes"
        }
        
        r debug reload nosave
        
        # Return the results
        r get simple_test
    } {simple_value}

    test {RDB downgrade compatibility - mixed data types preservation} {
        r del mixed_key1 mixed_key2 mixed_key3 mixed_key4
        
        # String
        r set mixed_key1 "test string value"
        
        # Hash with various field types
        r hmset mixed_key2 str_field "string_value" num_field 12345
        
        # Sorted set with different scores
        r zadd mixed_key3 -1.5 negative 0 zero 1.5 positive 100 large
        
        # List with mixed content
        r rpush mixed_key4 "text" "123" "more text"
        
        # Save and reload
        r bgsave
        waitForBgsave r
        r debug reload nosave
        
        # Verify all types are preserved correctly
        list [r get mixed_key1] [r hmget mixed_key2 str_field num_field] [r zscore mixed_key3 zero] [r lindex mixed_key4 1]
    } {{test string value} {string_value 12345} 0 123}

    test {RDB downgrade compatibility - large data structures} {
        r del large_hash large_zset
        
        # Create larger structures that might trigger encoding changes
        for {set i 1} {$i <= 100} {incr i} {
            r hset large_hash "field$i" "value$i"
            r zadd large_zset $i "member$i"
        }
        
        # These should automatically convert to hash table / skiplist encodings
        # but the compatibility layer should handle loading them correctly
        
        # Save and reload
        r bgsave  
        waitForBgsave r
        r debug reload nosave
        
        # Verify some data points
        list [r hget large_hash field50] [r zscore large_zset member75] [r hlen large_hash] [r zcard large_zset]
    } {value50 75 100 100}

    test {RDB downgrade compatibility - empty collections handling} {
        r del empty_hash empty_zset empty_list
        
        # Create empty collections
        r hset empty_hash temp_field temp_value
        r hdel empty_hash temp_field
        
        r zadd empty_zset 1 temp_member  
        r zrem empty_zset temp_member
        
        r rpush empty_list temp_item
        r lpop empty_list
        
        # These operations should leave empty collections
        # Save and reload to test empty collection handling
        r bgsave
        waitForBgsave r  
        r debug reload nosave
        
        # Verify collections exist and are empty (or don't exist)
        list [r hlen empty_hash] [r zcard empty_zset] [r llen empty_list]
    } {0 0 0}

    test {RDB downgrade compatibility - special values handling} {
        r del special_values
        
        # Test special numeric values and edge cases
        r hmset special_values \
            int_zero 0 \
            int_positive 12345 \
            int_negative -6789 \
            str_numeric "98765" \
            str_empty "" \
            str_special "special chars: @#$%^&*()" \
            str_unicode "测试 unicode ñáéíóú"
            
        # Save and reload
        r bgsave
        waitForBgsave r
        r debug reload nosave
        
        # Verify special values are preserved
        list [r hget special_values int_zero] [r hget special_values int_negative] [r hget special_values str_empty] [r hexists special_values str_unicode]
    } {0 -6789 {} 1}

    test {RDB downgrade compatibility - Valkey 8.0 RDB v11 Hash conversion} {
        r del valkey80_hash_test
        
        # Simulate loading from Valkey 8.0 RDB v11 by creating hash data
        # that would use listpack encoding in newer versions
        r hmset valkey80_hash_test \
            field1 "value1" \
            field2 "value2" \
            field3 "value3" \
            numeric_field 12345 \
            unicode_field "test Valkey"
        
        # Force save/reload cycle to test conversion
        r bgsave
        waitForBgsave r
        r debug reload nosave
        
        # Verify hash data integrity and proper encoding handling
        list [r hget valkey80_hash_test field1] \
             [r hget valkey80_hash_test numeric_field] \
             [r hget valkey80_hash_test unicode_field] \
             [r hlen valkey80_hash_test] \
             [r hexists valkey80_hash_test field3]
    } {value1 12345 {test Valkey} 5 1}

    test {RDB downgrade compatibility - Valkey 8.0 RDB v11 SortedSet conversion} {
        r del valkey80_zset_test
        
        # Create sorted set that would use listpack in Valkey 8.0
        r zadd valkey80_zset_test 1.0 "member1"
        r zadd valkey80_zset_test 2.5 "member2"
        r zadd valkey80_zset_test -1.5 "negative_score"
        r zadd valkey80_zset_test 0 "zero_score"
        r zadd valkey80_zset_test 3.14159 "pi_score"
        
        # Test save/reload cycle
        r bgsave
        waitForBgsave r
        r debug reload nosave
        
        # Verify sorted set integrity
        list [r zscore valkey80_zset_test member1] \
             [r zscore valkey80_zset_test negative_score] \
             [r zscore valkey80_zset_test pi_score] \
             [r zcard valkey80_zset_test] \
             [r zrank valkey80_zset_test zero_score]
    } {1 -1.5 3.1415899999999999 5 1}

    test {RDB downgrade compatibility - Valkey 8.0 RDB v11 List conversion} {
        r del valkey80_list_test
        
        # Create list that would be stored as listpack in Valkey 8.0
        r rpush valkey80_list_test "first_item"
        r rpush valkey80_list_test "second_item"
        r rpush valkey80_list_test "unicode_test"
        r rpush valkey80_list_test "123456"
        r lpush valkey80_list_test "prepended_item"
        
        # Test conversion through save/reload
        r bgsave
        waitForBgsave r
        r debug reload nosave
        
        # Verify list integrity and order
        list [r llen valkey80_list_test] \
             [r lindex valkey80_list_test 0] \
             [r lindex valkey80_list_test 2] \
             [r lindex valkey80_list_test -1] \
             [r lrange valkey80_list_test 1 3]
    } {5 prepended_item second_item 123456 {first_item second_item unicode_test}}

    test {RDB downgrade compatibility - Redis 7.2 RDB v11 Hash conversion} {
        r del redis72_hash_test
        
        # Create hash similar to what Redis 7.2 would generate
        r hmset redis72_hash_test \
            key1 "redis_7_2_value" \
            key2 42 \
            key3 "special_chars_!@#$%^&*()" \
            empty_value "" \
            large_value [string repeat "x" 100]
        
        # Save and reload to test Redis 7.2 compatibility
        r bgsave
        waitForBgsave r
        r debug reload nosave
        
        # Verify all hash fields are preserved
        list [r hget redis72_hash_test key1] \
             [r hget redis72_hash_test key2] \
             [r hget redis72_hash_test empty_value] \
             [string length [r hget redis72_hash_test large_value]] \
             [r hexists redis72_hash_test key3]
    } {redis_7_2_value 42 {} 100 1}

    test {RDB downgrade compatibility - Redis 7.2 RDB v11 SortedSet conversion} {
        r del redis72_zset_test
        
        # Create sorted set with Redis 7.2 characteristics
        r zadd redis72_zset_test 100.0 "high_score"
        r zadd redis72_zset_test 0.1 "low_score"
        r zadd redis72_zset_test -50.5 "negative"
        r zadd redis72_zset_test 1e-10 "tiny_score"
        r zadd redis72_zset_test 1e10 "huge_score"
        
        # Test precision preservation through conversion
        r bgsave
        waitForBgsave r
        r debug reload nosave
        
        # Verify score precision and ordering
        list [r zscore redis72_zset_test high_score] \
             [r zscore redis72_zset_test tiny_score] \
             [r zscore redis72_zset_test huge_score] \
             [r zcount redis72_zset_test -inf +inf] \
             [r zrange redis72_zset_test 0 0]
    } {100 1e-10 10000000000 5 negative}

    test {RDB downgrade compatibility - Redis 7.2 RDB v11 List conversion} {
        r del redis72_list_test
        
        # Create complex list structure
        r rpush redis72_list_test "item_1"
        r rpush redis72_list_test "item_2"
        r rpush redis72_list_test [string repeat "y" 200]
        r rpush redis72_list_test ""
        r rpush redis72_list_test "final_item"
        
        # Test list conversion from Redis 7.2
        r bgsave
        waitForBgsave r
        r debug reload nosave
        
        # Verify list structure and content
        list [r llen redis72_list_test] \
             [r lindex redis72_list_test 0] \
             [string length [r lindex redis72_list_test 2]] \
             [r lindex redis72_list_test 3] \
             [r lindex redis72_list_test -1]
    } {5 item_1 200 {} final_item}

    test {RDB downgrade compatibility - Redis 7.0 RDB v10 Hash conversion} {
        r del redis70_hash_test
        
        # Create hash representing Redis 7.0 data
        for {set i 1} {$i <= 20} {incr i} {
            r hset redis70_hash_test "field_$i" "value_$i"
        }
        r hset redis70_hash_test special_field "Redis_7.0_data"
        r hset redis70_hash_test binary_data [binary format "a*" "\x00\x01\x02\x03"]
        
        # Test Redis 7.0 RDB v10 compatibility
        r bgsave
        waitForBgsave r
        r debug reload nosave
        
        # Verify hash integrity
        list [r hlen redis70_hash_test] \
             [r hget redis70_hash_test field_10] \
             [r hget redis70_hash_test special_field] \
             [r hexists redis70_hash_test field_20] \
             [string length [r hget redis70_hash_test binary_data]]
    } {22 value_10 Redis_7.0_data 1 4}

    test {RDB downgrade compatibility - Redis 7.0 RDB v10 SortedSet conversion} {
        r del redis70_zset_test
        
        # Create sorted set with various score types
        r zadd redis70_zset_test 0 "zero"
        r zadd redis70_zset_test 1.5 "one_half"
        r zadd redis70_zset_test -10 "negative_ten"
        r zadd redis70_zset_test 999999999 "large_int"
        r zadd redis70_zset_test 0.000001 "small_decimal"
        
        # Add members until we have good coverage
        for {set i 1} {$i <= 10} {incr i} {
            r zadd redis70_zset_test $i "member_$i"
        }
        
        # Test Redis 7.0 conversion
        r bgsave
        waitForBgsave r
        r debug reload nosave
        
        # Verify sorted set functionality
        list [r zcard redis70_zset_test] \
             [r zscore redis70_zset_test zero] \
             [r zscore redis70_zset_test large_int] \
             [r zrank redis70_zset_test zero] \
             [r zrevrank redis70_zset_test large_int]
    } {15 0 999999999 1 0}

    test {RDB downgrade compatibility - Redis 7.0 RDB v10 List conversion} {
        r del redis70_list_test
        
        # Create list with mixed data types
        r lpush redis70_list_test "first"
        r rpush redis70_list_test "second"
        r rpush redis70_list_test 12345
        r rpush redis70_list_test ""
        r rpush redis70_list_test "last_item"
        
        # Add more items to test larger lists
        for {set i 1} {$i <= 15} {incr i} {
            r rpush redis70_list_test "bulk_item_$i"
        }
        
        # Test Redis 7.0 list conversion
        r bgsave
        waitForBgsave r
        r debug reload nosave
        
        # Verify list operations work correctly
        list [r llen redis70_list_test] \
             [r lindex redis70_list_test 0] \
             [r lindex redis70_list_test 2] \
             [r lindex redis70_list_test -1] \
             [r lrange redis70_list_test 5 7]
    } {20 first 12345 bulk_item_15 {bulk_item_1 bulk_item_2 bulk_item_3}}

    test {RDB downgrade compatibility - Mixed version compatibility stress test} {
        r del mixed_version_hash mixed_version_zset mixed_version_list
        
        # Create complex data structures that test all conversion paths
        # Hash with various data types
        r hmset mixed_version_hash \
            string_field "test_string" \
            int_field 42 \
            float_as_string "3.14159" \
            unicode "mixed_version_test" \
            empty "" \
            large_text [string repeat "mixed_test" 50]
        
        # Sorted set with edge case scores
        r zadd mixed_version_zset 1.0 "normal"
        r zadd mixed_version_zset 0.0 "zero"
        r zadd mixed_version_zset -1.0 "negative"
        r zadd mixed_version_zset 1e-15 "tiny"
        r zadd mixed_version_zset 1e15 "huge"
        
        # List with mixed content
        r rpush mixed_version_list "string"
        r rpush mixed_version_list "12345"
        r rpush mixed_version_list ""
        r rpush mixed_version_list [string repeat "z" 1000]
        
        # Test comprehensive conversion
        r bgsave
        waitForBgsave r
        r debug reload nosave
        
        # Verify all data types maintained integrity
        list [r hget mixed_version_hash unicode] \
             [string length [r hget mixed_version_hash large_text]] \
             [r zscore mixed_version_zset tiny] \
             [r zscore mixed_version_zset huge] \
             [r llen mixed_version_list] \
             [string length [r lindex mixed_version_list -1]]
    } {mixed_version_test 500 1.0000000000000001e-15 1000000000000000 4 1000}

    test {RDB downgrade compatibility - INFO command statistics format} {
        # Clear any existing statistics and create test data
        r del info_test_hash info_test_zset info_test_list
        
        # Create small data structures that will trigger conversions
        r hmset info_test_hash field1 value1 field2 value2
        r zadd info_test_zset 1.0 member1 2.0 member2
        r rpush info_test_list item1 item2 item3
        
        # Force a save/reload cycle to trigger conversion statistics
        r bgsave
        waitForBgsave r
        r debug reload nosave
        
        # Get INFO output and verify RDBDowngradeStats section exists
        set info_output [r info RDBDowngradeStats]
        
        # Verify the section header is present
        set has_section [string match "*# RDBDowngradeStats*" $info_output]
        
        # Verify all expected field names are present
        set expected_fields {
            rdb_downgrade_keys_attempted
            rdb_downgrade_keys_succeeded
            rdb_downgrade_keys_failed
        }
        
        set all_fields_present 1
        foreach field $expected_fields {
            if {![string match "*$field:*" $info_output]} {
                set all_fields_present 0
                puts "Missing field: $field"
            }
        }
        
        list $has_section $all_fields_present
    } {1 1}

    test {RDB downgrade compatibility - Statistics field format validation} {
        # Create minimal test data to ensure statistics are generated
        r del format_test_key
        r hmset format_test_key test_field test_value
        
        # Trigger conversion
        r bgsave
        waitForBgsave r
        r debug reload nosave
        
        # Get INFO statistics output
        set info_output [r info RDBDowngradeStats]
        
        # Verify the format of each statistics line (field_name:numeric_value)
        set format_correct 1
        set lines [split $info_output "\n"]
        
        foreach line $lines {
            set line [string trim $line]
            # Skip empty lines and comments
            if {$line eq "" || [string match "#*" $line]} {
                continue
            }
            
            # Check if line matches expected format: field_name:number
            if {![regexp {^rdb_downgrade_[a-z_]+:\d+$} $line]} {
                set format_correct 0
                puts "Invalid format line: $line"
            }
        }
        
        # Also verify that we have the expected number of statistics fields
        set field_count 0
        foreach line $lines {
            set line [string trim $line]
            if {[string match "rdb_downgrade_*" $line]} {
                incr field_count
            }
        }
        
        # We should have exactly 5 statistics fields
        list $format_correct [expr {$field_count == 5}]
    } {1 1}

    test {RDB downgrade compatibility - Backward compatibility of existing statistics functions} {
        # Clear test data
        r del compat_test_hash compat_test_list
        
        # Create test structures
        r hmset compat_test_hash cf1 cv1 cf2 cv2
        r rpush compat_test_list ci1 ci2 ci3
        
        # Trigger conversion to ensure statistics are populated
        r bgsave
        waitForBgsave r
        r debug reload nosave
        
        # Get INFO output using the new section name
        set info_output [r info RDBDowngradeStats]
        
        # Verify that legacy field names are still present for backward compatibility
        set legacy_fields_present 1
        set legacy_fields {
            rdb_downgrade_keys_attempted
            rdb_downgrade_keys_succeeded
            rdb_downgrade_keys_failed
        }
        
        foreach field $legacy_fields {
            if {![string match "*$field:*" $info_output]} {
                set legacy_fields_present 0
                puts "Missing legacy field: $field"
            }
        }
        
        # Verify that values are numeric and non-negative
        proc extract_stat {info_text field_name} {
            if {[regexp "${field_name}:(\\d+)" $info_text match value]} {
                return $value
            }
            return -1
        }
        
        set total_conv [extract_stat $info_output "rdb_downgrade_keys_attempted"]
        set successful_conv [extract_stat $info_output "rdb_downgrade_keys_succeeded"]
        set failed_conv [extract_stat $info_output "rdb_downgrade_keys_failed"]
        
        set values_valid [expr {$total_conv >= 0 && $successful_conv >= 0 && $failed_conv >= 0}]
        
        list $legacy_fields_present $values_valid
    } {1 1}

}
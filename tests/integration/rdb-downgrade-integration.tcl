tags {"rdb downgrade integration"} {

# Test loading RDB version 9 encodings
set server_path [tmpdir "server.rdb-v9-encodings"]
exec cp tests/assets/encodings-rdb-version-9.rdb $server_path/dump.rdb
start_server [list overrides [list "dir" $server_path "dbfilename" "dump.rdb"]] {
    test "Load RDB v9 encodings and verify keys" {
        r select 0
        
        # Verify string keys
        assert_equal [r get string] "Hello World"
        assert_equal [r get number] "10"
        assert_match "a*" [r get compressible]
        
        # Verify hash keys
        assert_equal [r type hash] "hash"
        assert_equal [r hget hash a] "1"
        assert_equal [r hget hash eee] "5000000000"
        assert_equal [r type hash_zipped] "hash"
        assert_equal [r hget hash_zipped a] "1"
        
        # Verify set keys
        assert_equal [r type set] "set"
        assert [r sismember set "1"]
        assert [r sismember set "a"]
        assert_equal [r type set_zipped_1] "set"
        assert [r sismember set_zipped_1 "1"]
        assert_equal [r type set_zipped_2] "set"
        assert [r sismember set_zipped_2 "100000"]
        assert_equal [r type set_zipped_3] "set"
        assert [r sismember set_zipped_3 "1000000000"]
        
        # Verify list keys
        assert_equal [r type list] "list"
        assert [expr {[r llen list] > 0}]
        assert_equal [r type list_zipped] "list"
        assert [expr {[r llen list_zipped] > 0}]
        
        # Verify zset keys
        assert_equal [r type zset] "zset"
        assert_equal [r zscore zset a] "1"
        assert_equal [r type zset_zipped] "zset"
        assert_equal [r zscore zset_zipped a] "1"
        
        # Verify RDBDowngradeStats
        set info [r info RDBDowngradeStats]
        assert_match "*rdb_downgrade_keys_attempted:*" $info
        assert_match "*rdb_downgrade_keys_succeeded:*" $info
        assert_match "*rdb_downgrade_bytes_converted:*" $info
    }
}

# Test loading RDB version 10 encodings
set server_path [tmpdir "server.rdb-v10-encodings"]
exec cp tests/assets/encodings-rdb-version-10.rdb $server_path/dump.rdb
start_server [list overrides [list "dir" $server_path "dbfilename" "dump.rdb"]] {
    test "Load RDB v10 encodings and verify keys" {
        r select 0
        
        # Verify string keys
        assert_equal [r get string] "Hello World"
        assert_equal [r get number] "10"
        assert_match "a*" [r get compressible]
        
        # Verify hash keys and values
        assert_equal [r type hash] "hash"
        assert_equal [r hget hash a] "1"
        assert_equal [r hget hash b] "2"
        assert_equal [r hget hash eee] "5000000000"
        assert_equal [r type hash_zipped] "hash"
        assert_equal [r hget hash_zipped a] "1"
        assert_equal [r hget hash_zipped c] "3"
        
        # Verify set keys and members
        assert_equal [r type set] "set"
        assert [r sismember set "1"]
        assert [r sismember set "a"]
        assert [r sismember set "6000000000"]
        assert_equal [r type set_zipped_1] "set"
        assert [r sismember set_zipped_1 "1"]
        assert_equal [r type set_zipped_2] "set"
        assert [r sismember set_zipped_2 "100000"]
        assert_equal [r type set_zipped_3] "set"
        assert [r sismember set_zipped_3 "1000000000"]
        
        # Verify list keys and elements
        assert_equal [r type list] "list"
        assert [expr {[r llen list] > 0}]
        assert_equal [r lindex list 0] "1"
        assert_equal [r type list_zipped] "list"
        assert [expr {[r llen list_zipped] > 0}]
        assert_equal [r lindex list_zipped 0] "1"
        
        # Verify zset keys and scores
        assert_equal [r type zset] "zset"
        assert_equal [r zscore zset a] "1"
        assert_equal [r zscore zset bbbb] "5000000000"
        assert_equal [r type zset_zipped] "zset"
        assert_equal [r zscore zset_zipped a] "1"
        assert_equal [r zscore zset_zipped c] "3"
        
        # Verify RDBDowngradeStats
        set info [r info RDBDowngradeStats]
        assert_match "*rdb_downgrade_keys_attempted:*" $info
        assert_match "*rdb_downgrade_keys_succeeded:*" $info
        assert_match "*rdb_downgrade_bytes_converted:*" $info
    }
}

# Test loading RDB version 11 encodings
set server_path [tmpdir "server.rdb-v11-encodings"]
exec cp tests/assets/encodings-rdb-version-11.rdb $server_path/dump.rdb
start_server [list overrides [list "dir" $server_path "dbfilename" "dump.rdb"]] {
    test "Load RDB v11 encodings and verify keys" {
        r select 0
        
        # Verify string keys
        assert_equal [r get string] "Hello World"
        assert_equal [r get number] "10"
        assert_match "a*" [r get compressible]
        
        # Verify hash structures and specific fields
        assert_equal [r type hash] "hash"
        assert_equal [r hget hash a] "1"
        assert_equal [r hget hash aa] "10"
        assert_equal [r hget hash aaa] "100"
        assert_equal [r hget hash ddd] "400"
        assert_equal [r hget hash eee] "5000000000"
        assert_equal [r type hash_zipped] "hash"
        assert_equal [r hget hash_zipped a] "1"
        assert_equal [r hget hash_zipped b] "2"
        assert_equal [r hget hash_zipped c] "3"
        
        # Verify set structures and memberships
        assert_equal [r type set] "set"
        assert [r sismember set "1"]
        assert [r sismember set "2"]
        assert [r sismember set "a"]
        assert [r sismember set "6000000000"]
        assert_equal [r type set_zipped_1] "set"
        assert [r sismember set_zipped_1 "1"]
        assert_equal [r type set_zipped_2] "set"
        assert [r sismember set_zipped_2 "100000"]
        assert_equal [r type set_zipped_3] "set"
        assert [r sismember set_zipped_3 "1000000000"]
        
        # Verify list structures and elements
        assert_equal [r type list] "list"
        assert [expr {[r llen list] > 0}]
        assert_equal [r lindex list 0] "1"
        assert_equal [r lindex list 3] "a"
        assert_equal [r type list_zipped] "list"
        assert [expr {[r llen list_zipped] > 0}]
        assert_equal [r lindex list_zipped 0] "1"
        
        # Verify sorted set structures and scores
        assert_equal [r type zset] "zset"
        assert_equal [r zscore zset a] "1"
        assert_equal [r zscore zset b] "2"
        assert_equal [r zscore zset cccc] "123456789"
        assert_equal [r zscore zset bbbb] "5000000000"
        assert_equal [r type zset_zipped] "zset"
        assert_equal [r zscore zset_zipped a] "1"
        assert_equal [r zscore zset_zipped c] "3"
        
        # Verify RDBDowngradeStats
        set info [r info RDBDowngradeStats]
        assert_match "*rdb_downgrade_keys_attempted:*" $info
        assert_match "*rdb_downgrade_keys_succeeded:*" $info
        assert_match "*rdb_downgrade_bytes_converted:*" $info
    }
}

# Test loading RDB version 11 from Valkey encodings
set server_path [tmpdir "server.rdb-v11-valkey-encodings"]
exec cp tests/assets/encodings-rdb-valkey-11.rdb $server_path/dump.rdb
start_server [list overrides [list "dir" $server_path "dbfilename" "dump.rdb"]] {
    test "Load RDB v11 from Valkey encodings and verify keys" {
        r select 0
        
        # Verify string keys from Valkey RDB
        assert_equal [r get string] "Hello World"
        assert_equal [r get number] "10"
        assert_match "a*" [r get compressible]
        
        # Verify hash keys from Valkey
        assert_equal [r type hash] "hash"
        assert_equal [r hget hash a] "1"
        assert_equal [r hget hash c] "3"
        assert_equal [r hget hash eee] "5000000000"
        assert_equal [r type hash_zipped] "hash"
        assert_equal [r hget hash_zipped a] "1"
        assert_equal [r hget hash_zipped c] "3"
        
        # Verify set keys from Valkey
        assert_equal [r type set] "set"
        assert [r sismember set "1"]
        assert [r sismember set "a"]
        assert_equal [r type set_zipped_1] "set"
        assert [r sismember set_zipped_1 "1"]
        assert [r sismember set_zipped_1 "4"]
        
        # Verify list keys from Valkey
        assert_equal [r type list] "list"
        assert [expr {[r llen list] > 0}]
        assert_equal [r lindex list 0] "1"
        assert_equal [r type list_zipped] "list"
        assert_equal [r lindex list_zipped 2] "3"
        
        # Verify zset keys from Valkey
        assert_equal [r type zset] "zset"
        assert_equal [r zscore zset a] "1"
        assert_equal [r zscore zset bbbb] "5000000000"
        assert_equal [r type zset_zipped] "zset"
        assert_equal [r zscore zset_zipped b] "2"
        
        # Verify RDBDowngradeStats
        set info [r info RDBDowngradeStats]
        assert_match "*rdb_downgrade_keys_attempted:*" $info
        assert_match "*rdb_downgrade_keys_succeeded:*" $info
        assert_match "*rdb_downgrade_bytes_converted:*" $info
    }
}

# Test loading RDB version 987 (future version) - should succeed with default settings
set server_path [tmpdir "server.rdb-v987"]
exec cp tests/assets/encodings-rdb-version-987.rdb $server_path/dump.rdb
start_server [list overrides [list "dir" $server_path "dbfilename" "dump.rdb"]] {
    test "RDB v987 future version loading with relaxed check" {
        r select 0
        
        # Verify all keys from future version RDB are loaded correctly
        assert_equal [r get string] "Hello World"
        assert_equal [r get number] "10"
        assert_match "a*" [r get compressible]
        
        # Verify hash keys from future version
        assert_equal [r type hash] "hash"
        assert_equal [r hget hash a] "1"
        assert_equal [r hget hash b] "2"
        assert_equal [r hget hash c] "3"
        assert_equal [r hget hash eee] "5000000000"
        assert_equal [r type hash_zipped] "hash"
        assert_equal [r hget hash_zipped a] "1"
        assert_equal [r hget hash_zipped b] "2"
        assert_equal [r hget hash_zipped c] "3"
        
        # Verify set keys from future version
        assert_equal [r type set] "set"
        assert [r sismember set "1"]
        assert [r sismember set "a"]
        assert [r sismember set "6000000000"]
        assert_equal [r type set_zipped_1] "set"
        assert [r sismember set_zipped_1 "1"]
        assert_equal [r type set_zipped_2] "set"
        assert [r sismember set_zipped_2 "100000"]
        assert_equal [r type set_zipped_3] "set"
        assert [r sismember set_zipped_3 "1000000000"]
        
        # Verify list keys from future version
        assert_equal [r type list] "list"
        assert_equal [r lindex list 0] "1"
        assert_equal [r lindex list 3] "a"
        assert_equal [r type list_zipped] "list"
        assert_equal [r lindex list_zipped 0] "1"
        
        # Verify zset keys from future version
        assert_equal [r type zset] "zset"
        assert_equal [r zscore zset a] "1"
        assert_equal [r zscore zset b] "2"
        assert_equal [r zscore zset bbbb] "5000000000"
        assert_equal [r type zset_zipped] "zset"
        assert_equal [r zscore zset_zipped a] "1"
        assert_equal [r zscore zset_zipped b] "2"
        assert_equal [r zscore zset_zipped c] "3"
        
        # Verify RDBDowngradeStats for future version
        set info [r info RDBDowngradeStats]
        assert_match "*rdb_downgrade_keys_attempted:*" $info
        assert_match "*rdb_downgrade_keys_succeeded:*" $info
        assert_match "*rdb_downgrade_bytes_converted:*" $info
    }
}
# Test loading Redis 6.0.16 no-patch RDB
set server_path [tmpdir "server.redis-6.0.16-no-patch"]
exec cp tests/assets/redis-6.0.16-no-patch.rdb $server_path/dump.rdb
start_server [list overrides [list "dir" $server_path "dbfilename" "dump.rdb"]] {
    test "Load Redis 6.0.16 no-patch RDB and verify keys" {
        r select 0
        
        # Verify string keys
        assert_equal [r get string] "Hello World"
        assert_equal [r get number] "10"
        assert_equal [r get a] "1"
        set compressible_value [r get compressible]
        assert_match "a*" $compressible_value
        assert_equal [string length $compressible_value] 130
        
        # Verify sorted sets
        assert_equal [r type zset] "zset"
        assert_equal [r zcard zset] 12
        assert_equal [r zscore zset a] "1"
        assert_equal [r zscore zset aa] "10"
        assert_equal [r zscore zset aaa] "100"
        assert_equal [r zscore zset aaaa] "1000"
        assert_equal [r zscore zset b] "2"
        assert_equal [r zscore zset bb] "20"
        assert_equal [r zscore zset bbb] "200"
        assert_equal [r zscore zset bbbb] "5000000000"
        assert_equal [r zscore zset c] "3"
        assert_equal [r zscore zset cc] "30"
        assert_equal [r zscore zset ccc] "300"
        assert_equal [r zscore zset cccc] "123456789"
        
        assert_equal [r type zset_zipped] "zset"
        assert_equal [r zcard zset_zipped] 3
        assert_equal [r zscore zset_zipped a] "1"
        assert_equal [r zscore zset_zipped b] "2"
        assert_equal [r zscore zset_zipped c] "3"
        
        # Verify sets
        assert_equal [r type set_zipped_1] "set"
        assert_equal [r scard set_zipped_1] 4
        assert [r sismember set_zipped_1 "1"]
        assert [r sismember set_zipped_1 "2"]
        assert [r sismember set_zipped_1 "3"]
        assert [r sismember set_zipped_1 "4"]
        
        assert_equal [r type set_zipped_2] "set"
        assert_equal [r scard set_zipped_2] 6
        assert [r sismember set_zipped_2 "100000"]
        assert [r sismember set_zipped_2 "200000"]
        assert [r sismember set_zipped_2 "300000"]
        assert [r sismember set_zipped_2 "400000"]
        assert [r sismember set_zipped_2 "500000"]
        assert [r sismember set_zipped_2 "600000"]
        
        assert_equal [r type set_zipped_3] "set"
        assert_equal [r scard set_zipped_3] 6
        assert [r sismember set_zipped_3 "1000000000000"]
        assert [r sismember set_zipped_3 "2000000000000"]
        assert [r sismember set_zipped_3 "3000000000000"]
        assert [r sismember set_zipped_3 "4000000000000"]
        assert [r sismember set_zipped_3 "5000000000000"]
        assert [r sismember set_zipped_3 "6000000000000"]
        
        assert_equal [r type set] "set"
        assert_equal [r scard set] 4
        assert [r sismember set "6000000000"]
        assert [r sismember set "a"]
        assert [r sismember set "b"]
        assert [r sismember set "c"]
        
        # Verify lists (LPUSH order is reversed)
        assert_equal [r type list_zipped] "list"
        assert_equal [r llen list_zipped] 6
        assert_equal [r lindex list_zipped 0] "c"
        assert_equal [r lindex list_zipped 1] "b"
        assert_equal [r lindex list_zipped 2] "a"
        assert_equal [r lindex list_zipped 3] "3"
        assert_equal [r lindex list_zipped 4] "2"
        assert_equal [r lindex list_zipped 5] "1"
        
        assert_equal [r type list] "list"
        assert_equal [r llen list] 7
        assert_equal [r lindex list 0] "c"
        assert_equal [r lindex list 1] "b"
        assert_equal [r lindex list 2] "a"
        assert_equal [r lindex list 3] "3"
        assert_equal [r lindex list 4] "2"
        assert_equal [r lindex list 5] "1"
        assert_equal [r lindex list 6] "6000000000"
        
        # Verify hashes
        assert_equal [r type hash_zipped] "hash"
        assert_equal [r hlen hash_zipped] 3
        assert_equal [r hget hash_zipped a] "1"
        assert_equal [r hget hash_zipped b] "2"
        assert_equal [r hget hash_zipped c] "3"
        
        assert_equal [r type hash] "hash"
        assert_equal [r hlen hash] 10
        assert_equal [r hget hash a] "10"
        assert_equal [r hget hash aa] "100"
        assert_equal [r hget hash b] "2"
        assert_equal [r hget hash bb] "20"
        assert_equal [r hget hash bbb] "200"
        assert_equal [r hget hash c] "3"
        assert_equal [r hget hash cc] "30"
        assert_equal [r hget hash ccc] "300"
        assert_equal [r hget hash ddd] "400"
        assert_equal [r hget hash eee] "5000000000"
        
        # Verify total key count
        assert_equal [r dbsize] 14
        
        # Verify RDBDowngradeStats
        set info [r info RDBDowngradeStats]
        assert_match "*rdb_downgrade_keys_attempted:*" $info
        assert_match "*rdb_downgrade_keys_succeeded:*" $info
        assert_match "*rdb_downgrade_bytes_converted:*" $info
    }
}

# Test loading Redis 6.0.16 with-patch RDB
set server_path [tmpdir "server.redis-6.0.16-with-patch"]
exec cp tests/assets/redis-6.0.16-with-patch.rdb $server_path/dump.rdb
start_server [list overrides [list "dir" $server_path "dbfilename" "dump.rdb"]] {
    test "Load Redis 6.0.16 with-patch RDB and verify keys" {
        r select 0
        
        # Verify string keys
        assert_equal [r get string] "Hello World"
        assert_equal [r get number] "10"
        assert_equal [r get a] "1"
        set compressible_value [r get compressible]
        assert_match "a*" $compressible_value
        assert_equal [string length $compressible_value] 130
        
        # Verify sorted sets
        assert_equal [r type zset] "zset"
        assert_equal [r zcard zset] 12
        assert_equal [r zscore zset a] "1"
        assert_equal [r zscore zset aa] "10"
        assert_equal [r zscore zset aaa] "100"
        assert_equal [r zscore zset aaaa] "1000"
        assert_equal [r zscore zset b] "2"
        assert_equal [r zscore zset bb] "20"
        assert_equal [r zscore zset bbb] "200"
        assert_equal [r zscore zset bbbb] "5000000000"
        assert_equal [r zscore zset c] "3"
        assert_equal [r zscore zset cc] "30"
        assert_equal [r zscore zset ccc] "300"
        assert_equal [r zscore zset cccc] "123456789"
        
        assert_equal [r type zset_zipped] "zset"
        assert_equal [r zcard zset_zipped] 3
        assert_equal [r zscore zset_zipped a] "1"
        assert_equal [r zscore zset_zipped b] "2"
        assert_equal [r zscore zset_zipped c] "3"
        
        # Verify sets
        assert_equal [r type set_zipped_1] "set"
        assert_equal [r scard set_zipped_1] 4
        assert [r sismember set_zipped_1 "1"]
        assert [r sismember set_zipped_1 "2"]
        assert [r sismember set_zipped_1 "3"]
        assert [r sismember set_zipped_1 "4"]
        
        assert_equal [r type set_zipped_2] "set"
        assert_equal [r scard set_zipped_2] 6
        assert [r sismember set_zipped_2 "100000"]
        assert [r sismember set_zipped_2 "200000"]
        assert [r sismember set_zipped_2 "300000"]
        assert [r sismember set_zipped_2 "400000"]
        assert [r sismember set_zipped_2 "500000"]
        assert [r sismember set_zipped_2 "600000"]
        
        assert_equal [r type set_zipped_3] "set"
        assert_equal [r scard set_zipped_3] 6
        assert [r sismember set_zipped_3 "1000000000000"]
        assert [r sismember set_zipped_3 "2000000000000"]
        assert [r sismember set_zipped_3 "3000000000000"]
        assert [r sismember set_zipped_3 "4000000000000"]
        assert [r sismember set_zipped_3 "5000000000000"]
        assert [r sismember set_zipped_3 "6000000000000"]
        
        assert_equal [r type set] "set"
        assert_equal [r scard set] 4
        assert [r sismember set "6000000000"]
        assert [r sismember set "a"]
        assert [r sismember set "b"]
        assert [r sismember set "c"]
        
        # Verify lists (LPUSH order is reversed)
        assert_equal [r type list_zipped] "list"
        assert_equal [r llen list_zipped] 6
        assert_equal [r lindex list_zipped 0] "c"
        assert_equal [r lindex list_zipped 1] "b"
        assert_equal [r lindex list_zipped 2] "a"
        assert_equal [r lindex list_zipped 3] "3"
        assert_equal [r lindex list_zipped 4] "2"
        assert_equal [r lindex list_zipped 5] "1"
        
        assert_equal [r type list] "list"
        assert_equal [r llen list] 7
        assert_equal [r lindex list 0] "c"
        assert_equal [r lindex list 1] "b"
        assert_equal [r lindex list 2] "a"
        assert_equal [r lindex list 3] "3"
        assert_equal [r lindex list 4] "2"
        assert_equal [r lindex list 5] "1"
        assert_equal [r lindex list 6] "6000000000"
        
        # Verify hashes
        assert_equal [r type hash_zipped] "hash"
        assert_equal [r hlen hash_zipped] 3
        assert_equal [r hget hash_zipped a] "1"
        assert_equal [r hget hash_zipped b] "2"
        assert_equal [r hget hash_zipped c] "3"
        
        assert_equal [r type hash] "hash"
        assert_equal [r hlen hash] 10
        assert_equal [r hget hash a] "10"
        assert_equal [r hget hash aa] "100"
        assert_equal [r hget hash b] "2"
        assert_equal [r hget hash bb] "20"
        assert_equal [r hget hash bbb] "200"
        assert_equal [r hget hash c] "3"
        assert_equal [r hget hash cc] "30"
        assert_equal [r hget hash ccc] "300"
        assert_equal [r hget hash ddd] "400"
        assert_equal [r hget hash eee] "5000000000"
        
        # Verify total key count
        assert_equal [r dbsize] 14
        
        # Verify RDBDowngradeStats (patch version should show downgrade activity)
        set info [r info RDBDowngradeStats]
        assert_match "*rdb_downgrade_keys_attempted:*" $info
        assert_match "*rdb_downgrade_keys_succeeded:*" $info
        assert_match "*rdb_downgrade_bytes_converted:*" $info
    }
}

# Test loading Redis 6.2.7 no-patch RDB
set server_path [tmpdir "server.redis-6.2.7-no-patch"]
exec cp tests/assets/redis-6.2.7-no-patch.rdb $server_path/dump.rdb
start_server [list overrides [list "dir" $server_path "dbfilename" "dump.rdb"]] {
    test "Load Redis 6.2.7 no-patch RDB and verify keys" {
        r select 0
        
        # Verify string keys
        assert_equal [r get string] "Hello World"
        assert_equal [r get number] "10"
        assert_equal [r get a] "1"
        set compressible_value [r get compressible]
        assert_match "a*" $compressible_value
        assert_equal [string length $compressible_value] 130
        
        # Verify sorted sets
        assert_equal [r type zset] "zset"
        assert_equal [r zcard zset] 12
        assert_equal [r zscore zset a] "1"
        assert_equal [r zscore zset aa] "10"
        assert_equal [r zscore zset aaa] "100"
        assert_equal [r zscore zset aaaa] "1000"
        assert_equal [r zscore zset b] "2"
        assert_equal [r zscore zset bb] "20"
        assert_equal [r zscore zset bbb] "200"
        assert_equal [r zscore zset bbbb] "5000000000"
        assert_equal [r zscore zset c] "3"
        assert_equal [r zscore zset cc] "30"
        assert_equal [r zscore zset ccc] "300"
        assert_equal [r zscore zset cccc] "123456789"
        
        assert_equal [r type zset_zipped] "zset"
        assert_equal [r zcard zset_zipped] 3
        assert_equal [r zscore zset_zipped a] "1"
        assert_equal [r zscore zset_zipped b] "2"
        assert_equal [r zscore zset_zipped c] "3"
        
        # Verify sets
        assert_equal [r type set_zipped_1] "set"
        assert_equal [r scard set_zipped_1] 4
        assert [r sismember set_zipped_1 "1"]
        assert [r sismember set_zipped_1 "2"]
        assert [r sismember set_zipped_1 "3"]
        assert [r sismember set_zipped_1 "4"]
        
        assert_equal [r type set_zipped_2] "set"
        assert_equal [r scard set_zipped_2] 6
        assert [r sismember set_zipped_2 "100000"]
        assert [r sismember set_zipped_2 "200000"]
        assert [r sismember set_zipped_2 "300000"]
        assert [r sismember set_zipped_2 "400000"]
        assert [r sismember set_zipped_2 "500000"]
        assert [r sismember set_zipped_2 "600000"]
        
        assert_equal [r type set_zipped_3] "set"
        assert_equal [r scard set_zipped_3] 6
        assert [r sismember set_zipped_3 "1000000000000"]
        assert [r sismember set_zipped_3 "2000000000000"]
        assert [r sismember set_zipped_3 "3000000000000"]
        assert [r sismember set_zipped_3 "4000000000000"]
        assert [r sismember set_zipped_3 "5000000000000"]
        assert [r sismember set_zipped_3 "6000000000000"]
        
        assert_equal [r type set] "set"
        assert_equal [r scard set] 4
        assert [r sismember set "6000000000"]
        assert [r sismember set "a"]
        assert [r sismember set "b"]
        assert [r sismember set "c"]
        
        # Verify lists (LPUSH order is reversed)
        assert_equal [r type list_zipped] "list"
        assert_equal [r llen list_zipped] 6
        assert_equal [r lindex list_zipped 0] "c"
        assert_equal [r lindex list_zipped 1] "b"
        assert_equal [r lindex list_zipped 2] "a"
        assert_equal [r lindex list_zipped 3] "3"
        assert_equal [r lindex list_zipped 4] "2"
        assert_equal [r lindex list_zipped 5] "1"
        
        assert_equal [r type list] "list"
        assert_equal [r llen list] 7
        assert_equal [r lindex list 0] "c"
        assert_equal [r lindex list 1] "b"
        assert_equal [r lindex list 2] "a"
        assert_equal [r lindex list 3] "3"
        assert_equal [r lindex list 4] "2"
        assert_equal [r lindex list 5] "1"
        assert_equal [r lindex list 6] "6000000000"
        
        # Verify hashes
        assert_equal [r type hash_zipped] "hash"
        assert_equal [r hlen hash_zipped] 3
        assert_equal [r hget hash_zipped a] "1"
        assert_equal [r hget hash_zipped b] "2"
        assert_equal [r hget hash_zipped c] "3"
        
        assert_equal [r type hash] "hash"
        assert_equal [r hlen hash] 10
        assert_equal [r hget hash a] "10"
        assert_equal [r hget hash aa] "100"
        assert_equal [r hget hash b] "2"
        assert_equal [r hget hash bb] "20"
        assert_equal [r hget hash bbb] "200"
        assert_equal [r hget hash c] "3"
        assert_equal [r hget hash cc] "30"
        assert_equal [r hget hash ccc] "300"
        assert_equal [r hget hash ddd] "400"
        assert_equal [r hget hash eee] "5000000000"
        
        # Verify total key count
        assert_equal [r dbsize] 14
        
        # Verify RDBDowngradeStats
        set info [r info RDBDowngradeStats]
        assert_match "*rdb_downgrade_keys_attempted:*" $info
        assert_match "*rdb_downgrade_keys_succeeded:*" $info
        assert_match "*rdb_downgrade_bytes_converted:*" $info
    }
}

# Test loading Redis 6.2.7 with-patch RDB
set server_path [tmpdir "server.redis-6.2.7-with-patch"]
exec cp tests/assets/redis-6.2.7-with-patch.rdb $server_path/dump.rdb
start_server [list overrides [list "dir" $server_path "dbfilename" "dump.rdb"]] {
    test "Load Redis 6.2.7 with-patch RDB and verify keys" {
        r select 0
        
        # Verify string keys
        assert_equal [r get string] "Hello World"
        assert_equal [r get number] "10"
        assert_equal [r get a] "1"
        set compressible_value [r get compressible]
        assert_match "a*" $compressible_value
        assert_equal [string length $compressible_value] 130
        
        # Verify sorted sets
        assert_equal [r type zset] "zset"
        assert_equal [r zcard zset] 12
        assert_equal [r zscore zset a] "1"
        assert_equal [r zscore zset aa] "10"
        assert_equal [r zscore zset aaa] "100"
        assert_equal [r zscore zset aaaa] "1000"
        assert_equal [r zscore zset b] "2"
        assert_equal [r zscore zset bb] "20"
        assert_equal [r zscore zset bbb] "200"
        assert_equal [r zscore zset bbbb] "5000000000"
        assert_equal [r zscore zset c] "3"
        assert_equal [r zscore zset cc] "30"
        assert_equal [r zscore zset ccc] "300"
        assert_equal [r zscore zset cccc] "123456789"
        
        assert_equal [r type zset_zipped] "zset"
        assert_equal [r zcard zset_zipped] 3
        assert_equal [r zscore zset_zipped a] "1"
        assert_equal [r zscore zset_zipped b] "2"
        assert_equal [r zscore zset_zipped c] "3"
        
        # Verify sets
        assert_equal [r type set_zipped_1] "set"
        assert_equal [r scard set_zipped_1] 4
        assert [r sismember set_zipped_1 "1"]
        assert [r sismember set_zipped_1 "2"]
        assert [r sismember set_zipped_1 "3"]
        assert [r sismember set_zipped_1 "4"]
        
        assert_equal [r type set_zipped_2] "set"
        assert_equal [r scard set_zipped_2] 6
        assert [r sismember set_zipped_2 "100000"]
        assert [r sismember set_zipped_2 "200000"]
        assert [r sismember set_zipped_2 "300000"]
        assert [r sismember set_zipped_2 "400000"]
        assert [r sismember set_zipped_2 "500000"]
        assert [r sismember set_zipped_2 "600000"]
        
        assert_equal [r type set_zipped_3] "set"
        assert_equal [r scard set_zipped_3] 6
        assert [r sismember set_zipped_3 "1000000000000"]
        assert [r sismember set_zipped_3 "2000000000000"]
        assert [r sismember set_zipped_3 "3000000000000"]
        assert [r sismember set_zipped_3 "4000000000000"]
        assert [r sismember set_zipped_3 "5000000000000"]
        assert [r sismember set_zipped_3 "6000000000000"]
        
        assert_equal [r type set] "set"
        assert_equal [r scard set] 4
        assert [r sismember set "6000000000"]
        assert [r sismember set "a"]
        assert [r sismember set "b"]
        assert [r sismember set "c"]
        
        # Verify lists (LPUSH order is reversed)
        assert_equal [r type list_zipped] "list"
        assert_equal [r llen list_zipped] 6
        assert_equal [r lindex list_zipped 0] "c"
        assert_equal [r lindex list_zipped 1] "b"
        assert_equal [r lindex list_zipped 2] "a"
        assert_equal [r lindex list_zipped 3] "3"
        assert_equal [r lindex list_zipped 4] "2"
        assert_equal [r lindex list_zipped 5] "1"
        
        assert_equal [r type list] "list"
        assert_equal [r llen list] 7
        assert_equal [r lindex list 0] "c"
        assert_equal [r lindex list 1] "b"
        assert_equal [r lindex list 2] "a"
        assert_equal [r lindex list 3] "3"
        assert_equal [r lindex list 4] "2"
        assert_equal [r lindex list 5] "1"
        assert_equal [r lindex list 6] "6000000000"
        
        # Verify hashes
        assert_equal [r type hash_zipped] "hash"
        assert_equal [r hlen hash_zipped] 3
        assert_equal [r hget hash_zipped a] "1"
        assert_equal [r hget hash_zipped b] "2"
        assert_equal [r hget hash_zipped c] "3"
        
        assert_equal [r type hash] "hash"
        assert_equal [r hlen hash] 10
        assert_equal [r hget hash a] "10"
        assert_equal [r hget hash aa] "100"
        assert_equal [r hget hash b] "2"
        assert_equal [r hget hash bb] "20"
        assert_equal [r hget hash bbb] "200"
        assert_equal [r hget hash c] "3"
        assert_equal [r hget hash cc] "30"
        assert_equal [r hget hash ccc] "300"
        assert_equal [r hget hash ddd] "400"
        assert_equal [r hget hash eee] "5000000000"
        
        # Verify total key count
        assert_equal [r dbsize] 14
        
        # Verify RDBDowngradeStats (patch version should show downgrade activity)
        set info [r info RDBDowngradeStats]
        assert_match "*rdb_downgrade_keys_attempted:*" $info
        assert_match "*rdb_downgrade_keys_succeeded:*" $info
        assert_match "*rdb_downgrade_bytes_converted:*" $info
    }
}

# Test loading Redis 7.0.15 no-patch RDB
set server_path [tmpdir "server.redis-7.0.15-no-patch"]
exec cp tests/assets/redis-7.0.15-no-patch.rdb $server_path/dump.rdb
start_server [list overrides [list "dir" $server_path "dbfilename" "dump.rdb"]] {
    test "Load Redis 7.0.15 no-patch RDB and verify keys" {
        r select 0
        
        # Verify string keys
        assert_equal [r get string] "Hello World"
        assert_equal [r get number] "10"
        assert_equal [r get a] "1"
        set compressible_value [r get compressible]
        assert_match "a*" $compressible_value
        assert_equal [string length $compressible_value] 130
        
        # Verify sorted sets
        assert_equal [r type zset] "zset"
        assert_equal [r zcard zset] 12
        assert_equal [r zscore zset a] "1"
        assert_equal [r zscore zset aa] "10"
        assert_equal [r zscore zset aaa] "100"
        assert_equal [r zscore zset aaaa] "1000"
        assert_equal [r zscore zset b] "2"
        assert_equal [r zscore zset bb] "20"
        assert_equal [r zscore zset bbb] "200"
        assert_equal [r zscore zset bbbb] "5000000000"
        assert_equal [r zscore zset c] "3"
        assert_equal [r zscore zset cc] "30"
        assert_equal [r zscore zset ccc] "300"
        assert_equal [r zscore zset cccc] "123456789"
        
        assert_equal [r type zset_zipped] "zset"
        assert_equal [r zcard zset_zipped] 3
        assert_equal [r zscore zset_zipped a] "1"
        assert_equal [r zscore zset_zipped b] "2"
        assert_equal [r zscore zset_zipped c] "3"
        
        # Verify sets
        assert_equal [r type set_zipped_1] "set"
        assert_equal [r scard set_zipped_1] 4
        assert [r sismember set_zipped_1 "1"]
        assert [r sismember set_zipped_1 "2"]
        assert [r sismember set_zipped_1 "3"]
        assert [r sismember set_zipped_1 "4"]
        
        assert_equal [r type set_zipped_2] "set"
        assert_equal [r scard set_zipped_2] 6
        assert [r sismember set_zipped_2 "100000"]
        assert [r sismember set_zipped_2 "200000"]
        assert [r sismember set_zipped_2 "300000"]
        assert [r sismember set_zipped_2 "400000"]
        assert [r sismember set_zipped_2 "500000"]
        assert [r sismember set_zipped_2 "600000"]
        
        assert_equal [r type set_zipped_3] "set"
        assert_equal [r scard set_zipped_3] 6
        assert [r sismember set_zipped_3 "1000000000000"]
        assert [r sismember set_zipped_3 "2000000000000"]
        assert [r sismember set_zipped_3 "3000000000000"]
        assert [r sismember set_zipped_3 "4000000000000"]
        assert [r sismember set_zipped_3 "5000000000000"]
        assert [r sismember set_zipped_3 "6000000000000"]
        
        assert_equal [r type set] "set"
        assert_equal [r scard set] 4
        assert [r sismember set "6000000000"]
        assert [r sismember set "a"]
        assert [r sismember set "b"]
        assert [r sismember set "c"]
        
        # Verify lists (LPUSH order is reversed)
        assert_equal [r type list_zipped] "list"
        assert_equal [r llen list_zipped] 6
        assert_equal [r lindex list_zipped 0] "c"
        assert_equal [r lindex list_zipped 1] "b"
        assert_equal [r lindex list_zipped 2] "a"
        assert_equal [r lindex list_zipped 3] "3"
        assert_equal [r lindex list_zipped 4] "2"
        assert_equal [r lindex list_zipped 5] "1"
        
        assert_equal [r type list] "list"
        assert_equal [r llen list] 7
        assert_equal [r lindex list 0] "c"
        assert_equal [r lindex list 1] "b"
        assert_equal [r lindex list 2] "a"
        assert_equal [r lindex list 3] "3"
        assert_equal [r lindex list 4] "2"
        assert_equal [r lindex list 5] "1"
        assert_equal [r lindex list 6] "6000000000"
        
        # Verify hashes
        assert_equal [r type hash_zipped] "hash"
        assert_equal [r hlen hash_zipped] 3
        assert_equal [r hget hash_zipped a] "1"
        assert_equal [r hget hash_zipped b] "2"
        assert_equal [r hget hash_zipped c] "3"
        
        assert_equal [r type hash] "hash"
        assert_equal [r hlen hash] 10
        assert_equal [r hget hash a] "10"
        assert_equal [r hget hash aa] "100"
        assert_equal [r hget hash b] "2"
        assert_equal [r hget hash bb] "20"
        assert_equal [r hget hash bbb] "200"
        assert_equal [r hget hash c] "3"
        assert_equal [r hget hash cc] "30"
        assert_equal [r hget hash ccc] "300"
        assert_equal [r hget hash ddd] "400"
        assert_equal [r hget hash eee] "5000000000"
        
        # Verify total key count
        assert_equal [r dbsize] 14
        
        # Verify RDBDowngradeStats
        set info [r info RDBDowngradeStats]
        assert_match "*rdb_downgrade_keys_attempted:*" $info
        assert_match "*rdb_downgrade_keys_succeeded:*" $info
        assert_match "*rdb_downgrade_bytes_converted:*" $info
    }
}

# Test loading Redis 7.0.15 with-patch RDB
set server_path [tmpdir "server.redis-7.0.15-with-patch"]
exec cp tests/assets/redis-7.0.15-with-patch.rdb $server_path/dump.rdb
start_server [list overrides [list "dir" $server_path "dbfilename" "dump.rdb"]] {
    test "Load Redis 7.0.15 with-patch RDB and verify keys" {
        r select 0
        
        # Verify string keys
        assert_equal [r get string] "Hello World"
        assert_equal [r get number] "10"
        assert_equal [r get a] "1"
        set compressible_value [r get compressible]
        assert_match "a*" $compressible_value
        assert_equal [string length $compressible_value] 130
        
        # Verify sorted sets
        assert_equal [r type zset] "zset"
        assert_equal [r zcard zset] 12
        assert_equal [r zscore zset a] "1"
        assert_equal [r zscore zset aa] "10"
        assert_equal [r zscore zset aaa] "100"
        assert_equal [r zscore zset aaaa] "1000"
        assert_equal [r zscore zset b] "2"
        assert_equal [r zscore zset bb] "20"
        assert_equal [r zscore zset bbb] "200"
        assert_equal [r zscore zset bbbb] "5000000000"
        assert_equal [r zscore zset c] "3"
        assert_equal [r zscore zset cc] "30"
        assert_equal [r zscore zset ccc] "300"
        assert_equal [r zscore zset cccc] "123456789"
        
        assert_equal [r type zset_zipped] "zset"
        assert_equal [r zcard zset_zipped] 3
        assert_equal [r zscore zset_zipped a] "1"
        assert_equal [r zscore zset_zipped b] "2"
        assert_equal [r zscore zset_zipped c] "3"
        
        # Verify sets
        assert_equal [r type set_zipped_1] "set"
        assert_equal [r scard set_zipped_1] 4
        assert [r sismember set_zipped_1 "1"]
        assert [r sismember set_zipped_1 "2"]
        assert [r sismember set_zipped_1 "3"]
        assert [r sismember set_zipped_1 "4"]
        
        assert_equal [r type set_zipped_2] "set"
        assert_equal [r scard set_zipped_2] 6
        assert [r sismember set_zipped_2 "100000"]
        assert [r sismember set_zipped_2 "200000"]
        assert [r sismember set_zipped_2 "300000"]
        assert [r sismember set_zipped_2 "400000"]
        assert [r sismember set_zipped_2 "500000"]
        assert [r sismember set_zipped_2 "600000"]
        
        assert_equal [r type set_zipped_3] "set"
        assert_equal [r scard set_zipped_3] 6
        assert [r sismember set_zipped_3 "1000000000000"]
        assert [r sismember set_zipped_3 "2000000000000"]
        assert [r sismember set_zipped_3 "3000000000000"]
        assert [r sismember set_zipped_3 "4000000000000"]
        assert [r sismember set_zipped_3 "5000000000000"]
        assert [r sismember set_zipped_3 "6000000000000"]
        
        assert_equal [r type set] "set"
        assert_equal [r scard set] 4
        assert [r sismember set "6000000000"]
        assert [r sismember set "a"]
        assert [r sismember set "b"]
        assert [r sismember set "c"]
        
        # Verify lists (LPUSH order is reversed)
        assert_equal [r type list_zipped] "list"
        assert_equal [r llen list_zipped] 6
        assert_equal [r lindex list_zipped 0] "c"
        assert_equal [r lindex list_zipped 1] "b"
        assert_equal [r lindex list_zipped 2] "a"
        assert_equal [r lindex list_zipped 3] "3"
        assert_equal [r lindex list_zipped 4] "2"
        assert_equal [r lindex list_zipped 5] "1"
        
        assert_equal [r type list] "list"
        assert_equal [r llen list] 7
        assert_equal [r lindex list 0] "c"
        assert_equal [r lindex list 1] "b"
        assert_equal [r lindex list 2] "a"
        assert_equal [r lindex list 3] "3"
        assert_equal [r lindex list 4] "2"
        assert_equal [r lindex list 5] "1"
        assert_equal [r lindex list 6] "6000000000"
        
        # Verify hashes
        assert_equal [r type hash_zipped] "hash"
        assert_equal [r hlen hash_zipped] 3
        assert_equal [r hget hash_zipped a] "1"
        assert_equal [r hget hash_zipped b] "2"
        assert_equal [r hget hash_zipped c] "3"
        
        assert_equal [r type hash] "hash"
        assert_equal [r hlen hash] 10
        assert_equal [r hget hash a] "10"
        assert_equal [r hget hash aa] "100"
        assert_equal [r hget hash b] "2"
        assert_equal [r hget hash bb] "20"
        assert_equal [r hget hash bbb] "200"
        assert_equal [r hget hash c] "3"
        assert_equal [r hget hash cc] "30"
        assert_equal [r hget hash ccc] "300"
        assert_equal [r hget hash ddd] "400"
        assert_equal [r hget hash eee] "5000000000"
        
        # Verify total key count
        assert_equal [r dbsize] 14
        
        # Verify RDBDowngradeStats (patch version should show downgrade activity)
        set info [r info RDBDowngradeStats]
        assert_match "*rdb_downgrade_keys_attempted:*" $info
        assert_match "*rdb_downgrade_keys_succeeded:*" $info
        assert_match "*rdb_downgrade_bytes_converted:*" $info
    }
}

# Test loading Valkey 8.0.4 no-patch RDB
set server_path [tmpdir "server.valkey-8.0.4-no-patch"]
exec cp tests/assets/valkey-8.0.4-no-patch.rdb $server_path/dump.rdb
start_server [list overrides [list "dir" $server_path "dbfilename" "dump.rdb"]] {
    test "Load Valkey 8.0.4 no-patch RDB and verify keys" {
        r select 0
        
        # Verify string keys
        assert_equal [r get string] "Hello World"
        assert_equal [r get number] "10"
        assert_equal [r get a] "1"
        set compressible_value [r get compressible]
        assert_match "a*" $compressible_value
        assert_equal [string length $compressible_value] 130
        
        # Verify sorted sets
        assert_equal [r type zset] "zset"
        assert_equal [r zcard zset] 12
        assert_equal [r zscore zset a] "1"
        assert_equal [r zscore zset aa] "10"
        assert_equal [r zscore zset aaa] "100"
        assert_equal [r zscore zset aaaa] "1000"
        assert_equal [r zscore zset b] "2"
        assert_equal [r zscore zset bb] "20"
        assert_equal [r zscore zset bbb] "200"
        assert_equal [r zscore zset bbbb] "5000000000"
        assert_equal [r zscore zset c] "3"
        assert_equal [r zscore zset cc] "30"
        assert_equal [r zscore zset ccc] "300"
        assert_equal [r zscore zset cccc] "123456789"
        
        assert_equal [r type zset_zipped] "zset"
        assert_equal [r zcard zset_zipped] 3
        assert_equal [r zscore zset_zipped a] "1"
        assert_equal [r zscore zset_zipped b] "2"
        assert_equal [r zscore zset_zipped c] "3"
        
        # Verify sets
        assert_equal [r type set_zipped_1] "set"
        assert_equal [r scard set_zipped_1] 4
        assert [r sismember set_zipped_1 "1"]
        assert [r sismember set_zipped_1 "2"]
        assert [r sismember set_zipped_1 "3"]
        assert [r sismember set_zipped_1 "4"]
        
        assert_equal [r type set_zipped_2] "set"
        assert_equal [r scard set_zipped_2] 6
        assert [r sismember set_zipped_2 "100000"]
        assert [r sismember set_zipped_2 "200000"]
        assert [r sismember set_zipped_2 "300000"]
        assert [r sismember set_zipped_2 "400000"]
        assert [r sismember set_zipped_2 "500000"]
        assert [r sismember set_zipped_2 "600000"]
        
        assert_equal [r type set_zipped_3] "set"
        assert_equal [r scard set_zipped_3] 6
        assert [r sismember set_zipped_3 "1000000000000"]
        assert [r sismember set_zipped_3 "2000000000000"]
        assert [r sismember set_zipped_3 "3000000000000"]
        assert [r sismember set_zipped_3 "4000000000000"]
        assert [r sismember set_zipped_3 "5000000000000"]
        assert [r sismember set_zipped_3 "6000000000000"]
        
        assert_equal [r type set] "set"
        assert_equal [r scard set] 4
        assert [r sismember set "6000000000"]
        assert [r sismember set "a"]
        assert [r sismember set "b"]
        assert [r sismember set "c"]
        
        # Verify lists (LPUSH order is reversed)
        assert_equal [r type list_zipped] "list"
        assert_equal [r llen list_zipped] 6
        assert_equal [r lindex list_zipped 0] "c"
        assert_equal [r lindex list_zipped 1] "b"
        assert_equal [r lindex list_zipped 2] "a"
        assert_equal [r lindex list_zipped 3] "3"
        assert_equal [r lindex list_zipped 4] "2"
        assert_equal [r lindex list_zipped 5] "1"
        
        assert_equal [r type list] "list"
        assert_equal [r llen list] 7
        assert_equal [r lindex list 0] "c"
        assert_equal [r lindex list 1] "b"
        assert_equal [r lindex list 2] "a"
        assert_equal [r lindex list 3] "3"
        assert_equal [r lindex list 4] "2"
        assert_equal [r lindex list 5] "1"
        assert_equal [r lindex list 6] "6000000000"
        
        # Verify hashes
        assert_equal [r type hash_zipped] "hash"
        assert_equal [r hlen hash_zipped] 3
        assert_equal [r hget hash_zipped a] "1"
        assert_equal [r hget hash_zipped b] "2"
        assert_equal [r hget hash_zipped c] "3"
        
        assert_equal [r type hash] "hash"
        assert_equal [r hlen hash] 10
        assert_equal [r hget hash a] "10"
        assert_equal [r hget hash aa] "100"
        assert_equal [r hget hash b] "2"
        assert_equal [r hget hash bb] "20"
        assert_equal [r hget hash bbb] "200"
        assert_equal [r hget hash c] "3"
        assert_equal [r hget hash cc] "30"
        assert_equal [r hget hash ccc] "300"
        assert_equal [r hget hash ddd] "400"
        assert_equal [r hget hash eee] "5000000000"
        
        # Verify total key count
        assert_equal [r dbsize] 14
        
        # Verify RDBDowngradeStats (Valkey RDB should trigger downgrade)
        set info [r info RDBDowngradeStats]
        assert_match "*rdb_downgrade_keys_attempted:*" $info
        assert_match "*rdb_downgrade_keys_succeeded:*" $info
        assert_match "*rdb_downgrade_bytes_converted:*" $info
    }
}

# Test loading Valkey 9.0.0 no-patch RDB
set server_path [tmpdir "server.valkey-9.0.0-no-patch"]
exec cp tests/assets/valkey-9.0.0-no-patch.rdb $server_path/dump.rdb
start_server [list overrides [list "dir" $server_path "dbfilename" "dump.rdb"]] {
    test "Load Valkey 9.0.0 no-patch RDB and verify keys" {
        r select 0
        
        # Verify string keys
        assert_equal [r get string] "Hello World"
        assert_equal [r get number] "10"
        assert_equal [r get a] "1"
        set compressible_value [r get compressible]
        assert_match "a*" $compressible_value
        assert_equal [string length $compressible_value] 130
        
        # Verify sorted sets
        assert_equal [r type zset] "zset"
        assert_equal [r zcard zset] 12
        assert_equal [r zscore zset a] "1"
        assert_equal [r zscore zset aa] "10"
        assert_equal [r zscore zset aaa] "100"
        assert_equal [r zscore zset aaaa] "1000"
        assert_equal [r zscore zset b] "2"
        assert_equal [r zscore zset bb] "20"
        assert_equal [r zscore zset bbb] "200"
        assert_equal [r zscore zset bbbb] "5000000000"
        assert_equal [r zscore zset c] "3"
        assert_equal [r zscore zset cc] "30"
        assert_equal [r zscore zset ccc] "300"
        assert_equal [r zscore zset cccc] "123456789"
        
        assert_equal [r type zset_zipped] "zset"
        assert_equal [r zcard zset_zipped] 3
        assert_equal [r zscore zset_zipped a] "1"
        assert_equal [r zscore zset_zipped b] "2"
        assert_equal [r zscore zset_zipped c] "3"
        
        # Verify sets
        assert_equal [r type set_zipped_1] "set"
        assert_equal [r scard set_zipped_1] 4
        assert [r sismember set_zipped_1 "1"]
        assert [r sismember set_zipped_1 "2"]
        assert [r sismember set_zipped_1 "3"]
        assert [r sismember set_zipped_1 "4"]
        
        assert_equal [r type set_zipped_2] "set"
        assert_equal [r scard set_zipped_2] 6
        assert [r sismember set_zipped_2 "100000"]
        assert [r sismember set_zipped_2 "200000"]
        assert [r sismember set_zipped_2 "300000"]
        assert [r sismember set_zipped_2 "400000"]
        assert [r sismember set_zipped_2 "500000"]
        assert [r sismember set_zipped_2 "600000"]
        
        assert_equal [r type set_zipped_3] "set"
        assert_equal [r scard set_zipped_3] 6
        assert [r sismember set_zipped_3 "1000000000000"]
        assert [r sismember set_zipped_3 "2000000000000"]
        assert [r sismember set_zipped_3 "3000000000000"]
        assert [r sismember set_zipped_3 "4000000000000"]
        assert [r sismember set_zipped_3 "5000000000000"]
        assert [r sismember set_zipped_3 "6000000000000"]
        
        assert_equal [r type set] "set"
        assert_equal [r scard set] 4
        assert [r sismember set "6000000000"]
        assert [r sismember set "a"]
        assert [r sismember set "b"]
        assert [r sismember set "c"]
        
        # Verify lists (LPUSH order is reversed)
        assert_equal [r type list_zipped] "list"
        assert_equal [r llen list_zipped] 6
        assert_equal [r lindex list_zipped 0] "c"
        assert_equal [r lindex list_zipped 1] "b"
        assert_equal [r lindex list_zipped 2] "a"
        assert_equal [r lindex list_zipped 3] "3"
        assert_equal [r lindex list_zipped 4] "2"
        assert_equal [r lindex list_zipped 5] "1"
        
        assert_equal [r type list] "list"
        assert_equal [r llen list] 7
        assert_equal [r lindex list 0] "c"
        assert_equal [r lindex list 1] "b"
        assert_equal [r lindex list 2] "a"
        assert_equal [r lindex list 3] "3"
        assert_equal [r lindex list 4] "2"
        assert_equal [r lindex list 5] "1"
        assert_equal [r lindex list 6] "6000000000"
        
        # Verify hashes
        assert_equal [r type hash_zipped] "hash"
        assert_equal [r hlen hash_zipped] 3
        assert_equal [r hget hash_zipped a] "1"
        assert_equal [r hget hash_zipped b] "2"
        assert_equal [r hget hash_zipped c] "3"
        
        assert_equal [r type hash] "hash"
        assert_equal [r hlen hash] 10
        assert_equal [r hget hash a] "10"
        assert_equal [r hget hash aa] "100"
        assert_equal [r hget hash b] "2"
        assert_equal [r hget hash bb] "20"
        assert_equal [r hget hash bbb] "200"
        assert_equal [r hget hash c] "3"
        assert_equal [r hget hash cc] "30"
        assert_equal [r hget hash ccc] "300"
        assert_equal [r hget hash ddd] "400"
        assert_equal [r hget hash eee] "5000000000"
        
        # Verify total key count
        assert_equal [r dbsize] 14
        
        # Verify RDBDowngradeStats (Valkey RDB should trigger downgrade)
        set info [r info RDBDowngradeStats]
        assert_match "*rdb_downgrade_keys_attempted:*" $info
        assert_match "*rdb_downgrade_keys_succeeded:*" $info
        assert_match "*rdb_downgrade_bytes_converted:*" $info
    }
}

} ;# tags
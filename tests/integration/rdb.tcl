tags {"rdb external:skip"} {

# Helper function to start a server and kill it, just to check the error
# logged.
set defaults {}
proc start_server_and_kill_it {overrides code} {
    upvar defaults defaults srv srv server_path server_path
    set config [concat $defaults $overrides]
    set srv [start_server [list overrides $config keep_persistence true]]
    uplevel 1 $code
    kill_server $srv
}

set server_path [tmpdir "server.rdb-encoding-test"]

# Copy RDB with different encodings in server path
exec cp tests/assets/encodings.rdb $server_path
exec cp tests/assets/encodings-rdb12.rdb $server_path
exec cp tests/assets/encodings-rdb75-unknown-types.rdb $server_path
exec cp tests/assets/encodings-rdb987.rdb $server_path
exec cp tests/assets/encodings-rdb987-unknown-types.rdb $server_path
exec cp tests/assets/list-quicklist.rdb $server_path

start_server [list overrides [list "dir" $server_path "dbfilename" "list-quicklist.rdb" save ""]] {
    test "test old version rdb file" {
        r select 0
        assert_equal [r get x] 7
        assert_encoding listpack list
        r lpop list
    } {7}
}

set csv_dump {"0","compressible","string","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
"0","hash","hash","a","1","aa","10","aaa","100","b","2","bb","20","bbb","200","c","3","cc","30","ccc","300","ddd","400","eee","5000000000",
"0","hash_zipped","hash","a","1","b","2","c","3",
"0","list","list","1","2","3","a","b","c","100000","6000000000","1","2","3","a","b","c","100000","6000000000","1","2","3","a","b","c","100000","6000000000",
"0","list_zipped","list","1","2","3","a","b","c","100000","6000000000",
"0","number","string","10"
"0","set","set","1","100000","2","3","6000000000","a","b","c",
"0","set_zipped_1","set","1","2","3","4",
"0","set_zipped_2","set","100000","200000","300000","400000",
"0","set_zipped_3","set","1000000000","2000000000","3000000000","4000000000","5000000000","6000000000",
"0","string","string","Hello World"
"0","zset","zset","a","1","b","2","c","3","aa","10","bb","20","cc","30","aaa","100","bbb","200","ccc","300","aaaa","1000","cccc","123456789","bbbb","5000000000",
"0","zset_zipped","zset","a","1","b","2","c","3",
}

start_server [list overrides [list "dir" $server_path "dbfilename" "encodings.rdb"]] {
  test "RDB encoding loading test" {
    r select 0
    csvdump r
  } $csv_dump
}

start_server_and_kill_it [list "dir" $server_path "dbfilename" "encodings-rdb987.rdb"] {
    test "RDB future version loading, strict version check" {
        wait_for_condition 50 100 {
            [string match {*Fatal error loading*} \
                 [exec tail -1 < [dict get $srv stdout]]]
        } else {
            fail "Server started even though RDB version is unsupported"
        }
    }
}

start_server [list overrides [list "dir" $server_path \
                                  "dbfilename" "encodings-rdb987.rdb" \
                                  "rdb-version-check" "relaxed"]] {
    test "RDB future version loading, relaxed version check" {
        r select 0
        csvdump r
    } $csv_dump
}

start_server_and_kill_it [list dir $server_path \
                              dbfilename "encodings-rdb987-unknown-types.rdb" \
                              rdb-version-check relaxed] {
    test "RDB future version loading with unknown types, relaxed version check" {
        wait_for_condition 50 100 {
            [string match {*Unknown type or opcode when loading DB. Unrecoverable error, aborting now.*} \
                 [exec tail -2 < [dict get $srv stdout]]]
        } else {
            fail "Server started even though RDB contains unknown types"
        }
    }
}

start_server [list overrides [list dir $server_path \
                                  dbfilename "encodings-rdb12.rdb" \
                                  rdb-version-check relaxed]] {
    test "RDB foreign version loading, relaxed version check" {
        r select 0
        assert_equal foo [r keys *]
        assert_equal bar [r get foo]
    }
}

start_server_and_kill_it [list dir $server_path \
                              dbfilename "encodings-rdb75-unknown-types.rdb" \
                              rdb-version-check relaxed] {
    test "RDB foreign version loading with unknown types, relaxed version check" {
        wait_for_condition 50 100 {
            [string match {*Can't handle foreign type or opcode 150 in RDB with version 75*} \
                 [exec tail -2 < [dict get $srv stdout]]]
        } else {
            fail "Server started even though RDB contains unknown types"
        }
    }
}

set server_path [tmpdir "server.rdb-startup-test"]

start_server [list overrides [list "dir" $server_path] keep_persistence true] {
    test {Server started empty with non-existing RDB file} {
        debug_digest
    } {0000000000000000000000000000000000000000}
    # Save an RDB file, needed for the next test.
    r save
}

start_server [list overrides [list "dir" $server_path] keep_persistence true] {
    test {Server started empty with empty RDB file} {
        debug_digest
    } {0000000000000000000000000000000000000000}
}

start_server [list overrides [list "dir" $server_path] keep_persistence true] {
    test {Test RDB stream encoding} {
        for {set j 0} {$j < 1000} {incr j} {
            if {rand() < 0.9} {
                r xadd stream * foo abc
            } else {
                r xadd stream * bar $j
            }
        }
        r xgroup create stream mygroup 0
        set records [r xreadgroup GROUP mygroup Alice COUNT 2 STREAMS stream >]
        r xdel stream [lindex [lindex [lindex [lindex $records 0] 1] 1] 0]
        r xack stream mygroup [lindex [lindex [lindex [lindex $records 0] 1] 0] 0]
        set digest [debug_digest]
        r debug reload
        set newdigest [debug_digest]
        assert {$digest eq $newdigest}
    }
    # delete the stream, maybe valgrind will find something
    r del stream
}

set dump_path [file join $server_path dump.rdb]

# Prepare custom umask test scenario
if {[catch {package require Tclx}]} {
    if {$::verbose} {
        puts "Skipping umask test. Package Tclx not installed."
    }
} else {
    # We have umask from the Tclx package.
    set old_umask [umask]
    set old_perm [expr {666 - $old_umask}]
    assert_equal [file attributes $dump_path -permissions] 00$old_perm

    if {$old_umask == 22} {
        set new_umask 2
    } else {
        set new_umask 22
    }
    set new_perm [expr {666 - $new_umask}]

    umask $new_umask
    start_server [list overrides [list "dir" $server_path] keep_persistence true] {
        test {Test nondefault umask applied} {
            r save
            # Use numeric comparison for compatibility with Tcl 8 and 9.
            assert_range [file attributes $dump_path -permissions] 00$new_perm 00$new_perm
        }
    }
    umask $old_umask
}

# Make the RDB file unreadable
file attributes $dump_path -permissions 0222

# Detect root account (it is able to read the file even with 002 perm)
set isroot 0
catch {
    open $dump_path
    set isroot 1
}

# Now make sure the server aborted with an error
if {!$isroot} {
    start_server_and_kill_it [list "dir" $server_path] {
        test {Server should not start if RDB file can't be open} {
            wait_for_condition 50 100 {
                [string match {*Fatal error loading*} \
                    [exec tail -1 < [dict get $srv stdout]]]
            } else {
                fail "Server started even if RDB was unreadable!"
            }
        }
    }
}

# Fix permissions of the RDB file.
file attributes $dump_path -permissions 0666

# Corrupt its CRC64 checksum.
set filesize [file size $dump_path]
set fd [open $dump_path r+]
fconfigure $fd -translation binary
seek $fd -8 end
puts -nonewline $fd "foobar00"; # Corrupt the checksum
close $fd

# Now make sure the server aborted with an error
start_server_and_kill_it [list "dir" $server_path] {
    test {Server should not start if RDB is corrupted} {
        wait_for_condition 50 100 {
            [string match {*CRC error*} \
                [exec tail -10 < [dict get $srv stdout]]]
        } else {
            fail "Server started even if RDB was corrupted!"
        }
    }
}

start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    foreach bgsave_type {"fork" "forkless"} {
        test "Test FLUSHALL aborts bgsave $bgsave_type" {
            # 5000 keys with 1ms sleep per key should take 5 second
            r config set rdb-key-save-delay 1000
            populate 5000
            assert_lessthan 999 [s rdb_changes_since_last_save]
            r config set bgsave-default-method $bgsave_type
            r bgsave
            assert_equal [s rdb_bgsave_in_progress] 1
            
            # Verify we're testing the right save type while it's running
            set expected_type [expr {$bgsave_type eq "forkless" ? "forkless" : "fork"}]
            assert_equal [s rdb_current_bgsave_type] $expected_type

            # Use this opportunity to also test the "bad arg" reply.
            assert_error {ERR*} {r flushall bad_arg}
            assert_equal [r ping] "PONG"
            
            r flushall
            # wait a second max (bgsave should take 5)
            wait_for_condition 50 100 {
                [s rdb_bgsave_in_progress] == 0
            } else {
                fail "bgsave not aborted"
            }
            # verify that bgsave failed, by checking that the change counter is still high
            assert_lessthan 999 [s rdb_changes_since_last_save]
            # make sure the server is still writable
            r set x xx
        }
    }

    foreach bgsave_type {"fork" "forkless"} {
        test "bgsave $bgsave_type resets the change counter" {
            r config set rdb-key-save-delay 0
            r config set bgsave-default-method $bgsave_type
            r bgsave
            wait_for_condition 50 100 {
                [s rdb_bgsave_in_progress] == 0
            } else {
                fail "bgsave not done"
            }
            assert_equal [s rdb_changes_since_last_save] 0
            
            # Verify we tested the right save type
            set expected_type [expr {$bgsave_type eq "forkless" ? "forkless" : "fork"}]
            assert_equal [s rdb_last_bgsave_type] $expected_type
        }
    }

    foreach bgsave_type {"fork" "forkless"} {
        test "bgsave $bgsave_type metrics are correct after success" {
            set saves_before [s rdb_saves]
            populate 100 "" 16
            r config set bgsave-default-method $bgsave_type
            r bgsave
            waitForBgsave r
            assert {[s rdb_saves] == $saves_before + 1}
            assert {[s rdb_last_bgsave_time_sec] >= 0 && [s rdb_last_bgsave_time_sec] < 3600}
            assert_equal [s rdb_last_bgsave_status] "ok"
            assert_equal [s rdb_last_bgsave_type] $bgsave_type
            assert {[s rdb_bgsave_in_progress] == 0}
            assert {[s current_fork_perc] == 0}
            assert {[s current_save_keys_processed] == 0}
            assert {[s current_save_keys_total] == 0}
        }
    }

    foreach bgsave_type {"fork" "forkless"} {
        test "bgsave $bgsave_type metrics are correct after failure" {
            set saves_before [s rdb_saves]
            populate 1000 "" 16
            r config set bgsave-default-method $bgsave_type
            r config set rdb-key-save-delay 10000000
            if {$bgsave_type eq "forkless"} {
                # Inject a failure to make the save fail: a directory whose name
                # collides with the RDB file makes the final rename fail. We
                # can't just kill -9 like fork-based bgsave since there is no
                # child process.
                set rdb_path [file join [lindex [r config get dir] 1] [lindex [r config get dbfilename] 1]]
                file delete -force $rdb_path
                file mkdir $rdb_path
            }
            r bgsave
            wait_for_condition 50 100 {
                [s rdb_bgsave_in_progress] == 1
            } else {
                fail "$bgsave_type bgsave didn't start"
            }
            if {$bgsave_type eq "fork"} {
                set pid [get_child_pid 0]
                catch {exec kill -9 $pid}
            }
            r config set rdb-key-save-delay 0
            waitForBgsave r
            assert {[s rdb_last_bgsave_time_sec] >= 0 && [s rdb_last_bgsave_time_sec] < 3600}
            assert_equal [s rdb_last_bgsave_status] "err"
            assert_equal [s rdb_last_bgsave_type] $bgsave_type
            assert {[s rdb_bgsave_in_progress] == 0}
            assert {[s current_fork_perc] == 0}
            assert {[s current_save_keys_processed] == 0}
            assert {[s current_save_keys_total] == 0}
            r config set rdb-key-save-delay 0
            if {$bgsave_type eq "forkless"} {
                # Remove the directory so later saves in this server can succeed.
                file delete [file join [lindex [r config get dir] 1] [lindex [r config get dbfilename] 1]]
            }
        }
    }

    foreach bgsave_type {"fork" "forkless"} {
        test "bgsave cancel aborts $bgsave_type save" {
            # Generating RDB will take some 100 seconds
            r config set rdb-key-save-delay 1000000
            populate 100 "" 16

            r config set bgsave-default-method $bgsave_type
            r bgsave
            wait_for_condition 50 100 {
                [s rdb_bgsave_in_progress] == 1
            } else {
                fail "bgsave did not start in time"
            }
            
            # Verify we're testing the right save type
            set expected_type [expr {$bgsave_type eq "forkless" ? "forkless" : "fork"}]
            assert_equal [s rdb_current_bgsave_type] $expected_type
            
            if {$bgsave_type ne "forkless"} {
                set fork_child_pid [get_child_pid 0]
            }
            
            assert {[r bgsave cancel] eq {Background saving cancelled}}
            
            if {$bgsave_type ne "forkless"} {
                set temp_rdb [file join [lindex [r config get dir] 1] temp-${fork_child_pid}.rdb]
                # Temp rdb must be deleted
                wait_for_condition 50 100 {
                    ![file exists $temp_rdb]
                } else {
                    fail "bgsave temp file was not deleted after cancel"
                }
            }

             # Make sure no save is running and that bgsave return an error
             wait_for_condition 50 100 {
                [s rdb_bgsave_in_progress] == 0
            } else {
                fail "bgsave is currently running"
            }
            assert_error "ERR Background saving is currently not in progress or scheduled" {r bgsave cancel}
        }
    }
}

start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    test "forkless bgsave contains expired keys from when save started" {
        
        # Set two keys that expire together
        r set k1 v1
        r set k2 v2
        set curr_time [clock seconds]
        r expireat k1 [expr {$curr_time + 2}]
        r expireat k2 [expr {$curr_time + 2}]
        
        # Start slow forkless save
        r config set rdb-key-save-delay 10000000
        r config set bgsave-default-method forkless
        r bgsave
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "forkless bgsave did not start"
        }
        
        # Let both keys expire
        after 3000
        
        # Serialize k1 in the foreground by touching it
        r set k1 v11
        
        # Complete forkless save so k2 will be serialized in background
        r config set rdb-key-save-delay 0
        waitForBgsave r
        
        # Check both keys are in the RDB
        set rdb_path [file join [lindex [r config get dir] 1] [lindex [r config get dbfilename] 1]]
        set fd [open $rdb_path rb]
        set rdb_content [read $fd]
        close $fd
        assert {[string first "k1" $rdb_content] != -1}
        assert {[string first "k2" $rdb_content] != -1}
    } {} {needs:debug}
}

start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    test "FLUSHDB during single-db forkless bgsave aborts the save without failing it" {
        
        # Populate database with complex dataset
        createComplexDataset r 1000
        
        # Get initial key count
        set initial_keys [r dbsize]
        assert {$initial_keys > 0}
        
        # Start forkless save with very slow save (high delay per key)
        r config set rdb-key-save-delay 100000
        r config set bgsave-default-method forkless
        r bgsave
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "forkless bgsave did not start"
        }
        
        r flushdb
        assert_equal [r dbsize] 0
        
        # Speed up and wait for save to abort
        # Note: Cancellation needs to be processed by background thread
        r config set rdb-key-save-delay 0
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 0
        } else {
            fail "forkless bgsave did not abort"
        }
        
        # A flush aborts the save the way FLUSHALL aborts a fork save: it is not
        # a failure, so writes stay allowed.
        assert {[s rdb_last_bgsave_status] ne "err"}
        r set k v
        assert_equal [r get k] v
    } {} {needs:debug}
}

start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    test "FLUSHDB during multi-db forkless bgsave aborts the save without failing it" {
        
        # Populate multiple databases
        for {set i 0} {$i < 1000} {incr i} {
            r set key$i val$i
        }
        r select 1
        for {set i 0} {$i < 1000} {incr i} {
            r set key$i val$i
        }
        r select 0
        
        # Start slow forkless save
        r config set rdb-key-save-delay 100000
        r config set bgsave-default-method forkless
        r bgsave
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "forkless bgsave did not start"
        }
        
        # Give forkless save time to start iterating
        after 100
        
        # FLUSHDB on db 1 while save is running - this should terminate the save
        r select 1
        r flushdb
        assert_equal [r dbsize] 0
        r select 0
        
        # Resume save speed and wait for save to abort
        r config set rdb-key-save-delay 0
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 0
        } else {
            fail "forkless bgsave did not abort"
        }
        
        # A flush aborts the save; it is not a failure, so writes stay allowed.
        assert {[s rdb_last_bgsave_status] ne "err"}
        r set k v
        assert_equal [r get k] v
    } {} {needs:debug}
}

start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    test "multiple databases modifications during forkless bgsave" {
        
        # Populate 5 databases with all data types
        for {set db 0} {$db < 5} {incr db} {
            r select $db
            createComplexDatasetForVerification r 20 "db${db}_"
        }
        r select 0
        
        # Start slow forkless save
        r config set rdb-key-save-delay 10000
        r config set bgsave-default-method forkless
        r bgsave
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "forkless bgsave did not start"
        }
        
        # Modify keys in all databases while save is running
        for {set db 0} {$db < 5} {incr db} {
            r select $db
            for {set i 0} {$i < 20} {incr i} {
                r append db${db}_before_$i "value_after_$i"
                r incr db${db}_int_$i
                r set db${db}_after_$i "VALUE_AFTER_$i"
                r lpush db${db}_lst_$i "LL2" "LL1"
                r rpush db${db}_lst_$i "RR1" "RR2"
                r sadd db${db}_set_$i "BB1" "BB2"
                r zadd db${db}_zset_$i 5 "Z2"
                r hset db${db}_hash_$i "H1" "c"
                r pfadd db${db}_hll_$i "PF2"
                r geoadd db${db}_geo_$i -122.1592 47.5976 "bellevue"
                r xadd db${db}_stream_$i "*" "D1" "V2"
                r xreadgroup GROUP db${db}_group_$i consumer_after_$i COUNT 1 STREAMS db${db}_stream_$i >
                r bitfield db${db}_bits_$i SET u4 0 0 INCRBY u4 0 1
                r geosearchstore db${db}_geo_set_$i db${db}_geo_$i FROMLONLAT -122.191729 47.685821 BYRADIUS 10 mi
                r geosearchstore db${db}_geo_set_dist_$i db${db}_geo_$i FROMLONLAT -122.191729 47.685821 BYRADIUS 10 mi ASC COUNT 10 STOREDIST
            }
        }
        r select 0
        
        # Verify modifications happened in live database
        assert {[s rdb_changes_since_last_save] > 0}
        for {set db 0} {$db < 5} {incr db} {
            r select $db
            for {set i 0} {$i < 20} {incr i} {
                assert_equal [r get db${db}_before_$i] "value_before_${i}value_after_$i"
            }
        }
        r select 0
        
        # Speed up and complete save
        r config set rdb-key-save-delay 0
        waitForBgsave r
        
        # Verify save completed successfully
        assert_equal [s rdb_last_bgsave_status] ok
        
        # Reload from RDB and verify ORIGINAL values are preserved
        # (consistent snapshot should capture state at start of save)
        catch {r debug reload nosave}
        for {set db 0} {$db < 5} {incr db} {
            r select $db
            for {set i 0} {$i < 20} {incr i} {
                # Strings: original value, not appended
                assert_equal [r get db${db}_before_$i] "value_before_$i"
                # Ints: original value, not incremented
                assert_equal [r get db${db}_int_$i] [expr {42 + $i}]
                # New keys should not exist
                assert_equal [r exists db${db}_after_$i] 0
                # Lists: original 4 elements, not 8
                assert_equal [r llen db${db}_lst_$i] 4
                assert_equal [r lrange db${db}_lst_$i 0 -1] [list "L1" "L2" "R1" "R2"]
                # Sets: original 2 members
                assert_equal [r scard db${db}_set_$i] 2
                assert_equal [lsort [r smembers db${db}_set_$i]] [list "B1" "B2"]
                # Sorted sets: original score
                assert_equal [r zscore db${db}_zset_$i "Z2"] 2
                # Hashes: original value
                assert_equal [r hget db${db}_hash_$i "H1"] "a"
                # HLL: original count
                assert_equal [r pfcount db${db}_hll_$i] 1
                # Geo: original 1 member
                assert_equal [r zcard db${db}_geo_$i] 1
                assert_equal [r zcard db${db}_geo_set_$i] 1
            }
        }
    } {} {needs:debug}
}

start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    test "modify new keys during forkless bgsave" {
        
        # Populate database with all data types
        createComplexDatasetForVerification r 20
        set original_keys [r dbsize]
        
        # Start forkless save with very slow save (high delay per key)
        r config set rdb-key-save-delay 10000
        r config set bgsave-default-method forkless
        r bgsave
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "forkless bgsave did not start"
        }
        
        # Create new keys of all data types while save is running
        for {set i 0} {$i < 100} {incr i} {
            r set after_$i "value_after_$i"
            r set after_i_$i 42
            r lpush after_lst_$i "L2" "L1"
            r rpush after_lst_$i "R1" "R2"
            r sadd after_set_$i "B1"
            r sadd after_iset_$i 12 34
            r zadd after_zset_$i 1 "Z1"
            r hset after_hash_$i "H1" "a"
            r pfadd after_hll_$i "PF1"
            r set after_bits_$i "\x0f"
            r bitfield after_bits_$i SET u4 0 0 INCRBY u4 0 1
            r geoadd after_geo_$i -122.345 47.775 "costco"
            r geosearchstore after_geo_set_$i after_geo_$i FROMLONLAT -122.191729 47.685821 BYRADIUS 10 mi
            r geosearchstore after_geo_set_dist_$i after_geo_$i FROMLONLAT -122.191729 47.685821 BYRADIUS 10 mi ASC COUNT 10 STOREDIST
            r xadd after_stream_$i "*" "D1" "V2"
            r xgroup create after_stream_$i after_group_$i 0
            r hsetex after_hashttl_$i EX 10000 FIELDS 1 HTTL1 a
        }
        
        # Verify new keys were created (14 key types per iteration × 100 iterations)
        set expected_keys [expr {$original_keys + 100 * 14}]
        assert_equal [r dbsize] $expected_keys
        assert_equal [s rdb_bgsave_in_progress] 1
        
        # Speed up and complete save
        r config set rdb-key-save-delay 0
        waitForBgsave r
        
        # Verify save completed successfully
        assert_equal [s rdb_last_bgsave_status] ok
        
        # Reload and verify ONLY original keys exist (new keys should NOT be in snapshot)
        catch {r debug reload nosave}
        assert_equal [r dbsize] $original_keys
        
        # Verify all original data types preserved
        for {set i 0} {$i < 20} {incr i} {
            assert_equal [r get before_$i] "value_before_$i"
            assert_equal [r get int_$i] [expr {42 + $i}]
            assert_equal [r llen lst_$i] 4
            assert_equal [r lrange lst_$i 0 -1] [list "L1" "L2" "R1" "R2"]
            assert_equal [r scard set_$i] 2
            assert_equal [r zscore zset_$i "Z1"] 1
            assert_equal [r hget hash_$i "H1"] "a"
            assert_equal [r pfcount hll_$i] 1
            assert_equal [r zcard geo_$i] 1
            assert_equal [r zcard geo_set_$i] 1
        }
        
        # Verify new keys do NOT exist in snapshot
        for {set i 0} {$i < 100} {incr i} {
            assert_equal [r exists after_$i] 0
            assert_equal [r exists after_lst_$i] 0
            assert_equal [r exists after_set_$i] 0
            assert_equal [r exists after_zset_$i] 0
            assert_equal [r exists after_hash_$i] 0
            assert_equal [r exists after_hll_$i] 0
            assert_equal [r exists after_geo_$i] 0
            assert_equal [r exists after_geo_set_$i] 0
            assert_equal [r exists after_geo_set_dist_$i] 0
            assert_equal [r exists after_stream_$i] 0
            assert_equal [r exists after_hashttl_$i] 0
        }
    } {} {needs:debug}
}

start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    test "SWAPDB during forkless bgsave" {
        
        # Populate 5 databases with all data types
        for {set db 0} {$db < 5} {incr db} {
            r select $db
            createComplexDatasetForVerification r 20 "db${db}_"
        }
        r select 0
        
        # Start slow forkless save
        r config set rdb-key-save-delay 10000
        r config set bgsave-default-method forkless
        r bgsave
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "forkless bgsave did not start"
        }
        
        # Keep swapping databases while save is running
        set perm [list 0 1 2 3 4]
        set swaps 0
        while {[s rdb_bgsave_in_progress] == 1 && $swaps < 200} {
            incr swaps
            # Shuffle permutation
            for {set i 4} {$i > 0} {incr i -1} {
                set j [expr {int(rand() * ($i + 1))}]
                set temp [lindex $perm $i]
                lset perm $i [lindex $perm $j]
                lset perm $j $temp
            }
            # Swap each database with its permuted target
            for {set db 0} {$db < 5} {incr db} {
                r swapdb $db [lindex $perm $db]
            }
        }
        
        # Speed up save and wait for completion
        r config set rdb-key-save-delay 0
        waitForBgsave r
        assert {$swaps > 100}
        
        # Verify save completed successfully
        assert_equal [s rdb_last_bgsave_status] ok
        
        # Reload from RDB and verify keys are in ORIGINAL databases
        # (SWAPDB is ignored for consistent snapshots)
        r select 0
        catch {r debug reload nosave}
        for {set db 0} {$db < 5} {incr db} {
            r select $db
            for {set i 0} {$i < 20} {incr i} {
                assert_equal [r get db${db}_before_$i] "value_before_$i"
                assert_equal [r get db${db}_int_$i] [expr {42 + $i}]
                assert_equal [r lrange db${db}_lst_$i 0 -1] [list "L1" "L2" "R1" "R2"]
                assert_equal [lsort [r smembers db${db}_set_$i]] [list "B1" "B2"]
                assert_equal [r zscore db${db}_zset_$i "Z1"] 1
                assert_equal [r hget db${db}_hash_$i "H1"] "a"
                assert_equal [r pfcount db${db}_hll_$i] 1
                assert_equal [r zcard db${db}_geo_$i] 1
                assert_equal [r zcard db${db}_geo_set_$i] 1
            }
        }
    } {} {needs:debug}
}

start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    test "delete all keys after SWAPDB during forkless bgsave" {
        
        # Populate 5 databases with all data types
        for {set db 0} {$db < 5} {incr db} {
            r select $db
            createComplexDatasetForVerification r 20 "db${db}_"
        }
        r select 0
        
        # Start slow forkless save
        r config set rdb-key-save-delay 10000
        r config set bgsave-default-method forkless
        r bgsave
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "forkless bgsave did not start"
        }
        
        # Swap databases with fixed permutation [2, 3, 4, 0, 1]
        set perm [list 2 3 4 0 1]
        for {set db 0} {$db < 5} {incr db} {
            r swapdb $db [lindex $perm $db]
        }
        
        # Delete all keys in all databases
        for {set db 4} {$db >= 0} {incr db -1} {
            r select $db
            set keys [r keys *]
            foreach key $keys {
                r del $key
            }
            assert_equal [r dbsize] 0
        }
        
        # Speed up and complete save
        r config set rdb-key-save-delay 0
        waitForBgsave r
        
        # Reload from RDB and verify ORIGINAL keys still exist
        # Consistent snapshot should preserve state before SWAPDB and deletions
        r select 0
        catch {r debug reload nosave}
        for {set db 0} {$db < 5} {incr db} {
            r select $db
            for {set i 0} {$i < 20} {incr i} {
                assert_equal [r get db${db}_before_$i] "value_before_$i"
                assert_equal [r get db${db}_int_$i] [expr {42 + $i}]
                assert_equal [r lrange db${db}_lst_$i 0 -1] [list "L1" "L2" "R1" "R2"]
                assert_equal [lsort [r smembers db${db}_set_$i]] [list "B1" "B2"]
                assert_equal [r zscore db${db}_zset_$i "Z1"] 1
                assert_equal [r hget db${db}_hash_$i "H1"] "a"
                assert_equal [r pfcount db${db}_hll_$i] 1
                assert_equal [r zcard db${db}_geo_$i] 1
                assert_equal [r zcard db${db}_geo_set_$i] 1
            }
        }
    } {} {needs:debug}
}

start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    test "deleting keys during forkless bgsave" {
        
        # Populate database with all data types
        createComplexDatasetForVerification r 20
        
        # Start forkless save with very slow save (high delay per key)
        r config set rdb-key-save-delay 10000
        r config set bgsave-default-method forkless
        r bgsave
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "forkless bgsave did not start"
        }
        
        # Delete all keys in the database
        set keys [r keys *]
        foreach key $keys {
            r del $key
        }
        
        # Verify all keys deleted and save still in progress
        assert_equal [r dbsize] 0
        assert_equal [s rdb_bgsave_in_progress] 1
        
        # Speed up and complete save
        r config set rdb-key-save-delay 0
        waitForBgsave r
        
        # Reload from RDB and verify ORIGINAL keys still exist
        catch {r debug reload nosave}
        for {set i 0} {$i < 20} {incr i} {
            assert_equal [r get before_$i] "value_before_$i"
            assert_equal [r get int_$i] [expr {42 + $i}]
            assert_equal [r lrange lst_$i 0 -1] [list "L1" "L2" "R1" "R2"]
            assert_equal [lsort [r smembers set_$i]] [list "B1" "B2"]
            assert_equal [r zscore zset_$i "Z1"] 1
            assert_equal [r hget hash_$i "H1"] "a"
            assert_equal [r pfcount hll_$i] 1
            assert_equal [r zcard geo_$i] 1
            assert_equal [r zcard geo_set_$i] 1
        }
    } {} {needs:debug}
}

start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    test "blocking commands during forkless bgsave" {
        
        # Create initial dataset with 100 keys
        createComplexDatasetForVerification r 100

        # Start blocking commands on nonexistent keys BEFORE save starts
        set rd1 [valkey_deferring_client]
        set rd2 [valkey_deferring_client]
        set rd3 [valkey_deferring_client]
        set rd4 [valkey_deferring_client]
        set rd5 [valkey_deferring_client]
        set rd6 [valkey_deferring_client]
        set rd7 [valkey_deferring_client]
        
        # Consume an item from a nonexistent key
        $rd1 blpop new1 0
        
        # Set up a cascade of brpoplpush's on nonexistent keys
        $rd2 brpoplpush new2 new3 0
        $rd3 brpoplpush new3 new4 0
        
        # Nonexistent keys
        $rd4 brpoplpush new5 new6 0
        
        # Cascade of brpoplpush's onto an existing key
        $rd5 brpoplpush new88 new7 0
        $rd6 brpoplpush new7 lst_2 0
        
        # Destination exists
        $rd7 brpoplpush new8 lst_70 0
        
        # Start save with slow speed
        r config set rdb-key-save-delay 100000
        r config set bgsave-default-method forkless
        r bgsave
        
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "bgsave didn't start"
        }
        
        # Start more blocking commands during save
        set rd8 [valkey_deferring_client]
        set rd9 [valkey_deferring_client]
        set rd10 [valkey_deferring_client]
        set rd11 [valkey_deferring_client]
        set rd12 [valkey_deferring_client]
        
        # Existing keys with new destinations, setting off some of the waiters
        $rd8 brpoplpush lst_33 new1 0
        $rd9 brpoplpush lst_27 new2 0
        
        # Duplicate another brpoplpush above
        $rd10 brpoplpush new5 new6 0
        
        # New key but existing destination
        $rd11 brpoplpush new9 lst_3 0
        
        # Consume an item from a nonexistent key
        $rd12 brpop new100 0
        
        # Set off more waiters
        r rpush new5 foobar
        r rpush new88 foobar
        
        assert_equal [s rdb_bgsave_in_progress] 1
        
        # Resume save at normal speed
        r config set rdb-key-save-delay 0
        waitForBgsave r
        
        # Wait for blocking commands to complete and read responses
        after 1000
        $rd8 read
        $rd9 read
        $rd1 read
        $rd2 read
        $rd3 read
        $rd5 read
        $rd6 read
        
        # Don't read from rd4, rd7, rd10, rd11, rd12 - they remain blocked or timeout
        
        # Verify the blocking commands executed correctly
        assert_equal [r llen new1] 0
        assert_equal [r llen new2] 0
        assert_equal [r llen new3] 0
        assert_equal [lindex [r lrange new4 -1 -1] 0] "R2"
        assert_equal [r llen new5] 0
        assert_equal [lindex [r lrange new6 -1 -1] 0] "foobar"
        assert_equal [r llen new88] 0
        assert_equal [r llen new7] 0
        assert_equal [lindex [r lrange lst_2 0 0] 0] "foobar"
        assert_equal [r llen new8] 0
        assert_equal [r llen lst_70] 4
        assert_equal [r llen lst_33] 3
        assert_equal [r llen lst_27] 3
        assert_equal [r llen lst_3] 4
        
        # Close deferred clients (those that didn't complete will be force-closed)
        $rd1 close
        $rd2 close
        $rd3 close
        $rd4 close
        $rd5 close
        $rd6 close
        $rd7 close
        $rd8 close
        $rd9 close
        $rd10 close
        $rd11 close
        $rd12 close

        # Verify snapshot contains original keys (blocking commands should not affect snapshot)
        catch {r debug reload nosave}
        
        # All original data types should be preserved in snapshot
        for {set i 0} {$i < 100} {incr i} {
            assert_equal [r get before_$i] "value_before_$i"
            assert_equal [r get int_$i] [expr {42 + $i}]
            assert_equal [r lrange lst_$i 0 -1] [list "L1" "L2" "R1" "R2"]
            assert_equal [lsort [r smembers set_$i]] [list "B1" "B2"]
            assert_equal [r zscore zset_$i "Z1"] 1
            assert_equal [r hget hash_$i "H1"] "a"
            assert_equal [r pfcount hll_$i] 1
            assert_equal [r zcard geo_$i] 1
            assert_equal [r zcard geo_set_$i] 1
        }
        
        # New keys created during save should NOT be in snapshot
        assert_equal [r exists new1] 0
        assert_equal [r exists new2] 0
        assert_equal [r exists new3] 0
        assert_equal [r exists new4] 0
        assert_equal [r exists new5] 0
        assert_equal [r exists new6] 0
        assert_equal [r exists new7] 0
        assert_equal [r exists new8] 0
        assert_equal [r exists new88] 0
        assert_equal [r exists new9] 0
        assert_equal [r exists new100] 0
    } {} {needs:debug}
}

start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    test "TTL expiration during forkless bgsave" {
        
        # Create initial dataset with 100 keys
        set num_keys 100
        createComplexDatasetForVerification r $num_keys
        
        # Set TTLs on all keys - key i expires in (i/10 + 1) seconds
        set start_time [clock milliseconds]
        for {set i 0} {$i < $num_keys} {incr i} {
            set ttl [expr {$i/10 + 1}]
            foreach prefix {before int lst set zset hash hll bits geo geo_set stream iset} {
                r expire ${prefix}_${i} $ttl
            }
        }
        
        # Start save and wait for completion
        r config set bgsave-default-method forkless
        r bgsave
        waitForBgsave r
        
        # Reload from RDB
        catch {r debug reload nosave}
        
        # Check keycount is reasonable
        set keycount [r dbsize]
        assert {$keycount <= $num_keys * 12}
        
        # Verify keys based on elapsed time
        set verified [list]
        for {set i 0} {$i < $num_keys} {incr i} {
            lappend verified $i
        }
        
        while {[llength $verified] > 0} {
            set elapsed_time [expr {([clock milliseconds] - $start_time) / 1000.0}]
            
            foreach i $verified {
                # If not yet expired, verify all data types exist
                if {$elapsed_time < [expr {$i/10.0}]} {
                    assert_equal [r exists before_${i}] 1
                    assert_equal [r exists int_${i}] 1
                    assert_equal [r exists lst_${i}] 1
                    assert_equal [r exists set_${i}] 1
                    assert_equal [r exists zset_${i}] 1
                    assert_equal [r exists hash_${i}] 1
                    assert_equal [r exists hll_${i}] 1
                    assert_equal [r exists bits_${i}] 1
                    assert_equal [r exists geo_${i}] 1
                    assert_equal [r exists geo_set_${i}] 1
                    assert_equal [r exists stream_${i}] 1
                    assert_equal [r exists iset_${i}] 1
                }
                
                # If expired for more than 2 seconds, verify all data types are gone
                if {$elapsed_time > [expr {$i/10.0 + 2}]} {
                    assert_equal [r exists before_${i}] 0
                    assert_equal [r exists int_${i}] 0
                    assert_equal [r exists lst_${i}] 0
                    assert_equal [r exists set_${i}] 0
                    assert_equal [r exists zset_${i}] 0
                    assert_equal [r exists hash_${i}] 0
                    assert_equal [r exists hll_${i}] 0
                    assert_equal [r exists bits_${i}] 0
                    assert_equal [r exists geo_${i}] 0
                    assert_equal [r exists geo_set_${i}] 0
                    assert_equal [r exists stream_${i}] 0
                    assert_equal [r exists iset_${i}] 0
                    set verified [lsearch -all -inline -not -exact $verified $i]
                }
            }
            
            after 100
        }
    } {} {needs:debug}
}

start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    test "evictions during forkless bgsave" {
        
        # Create initial dataset
        createComplexDatasetForVerification r 1000
        
        # Start save with stopped speed
        r config set rdb-key-save-delay 10000
        r config set bgsave-default-method forkless
        r bgsave
        
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "bgsave didn't start"
        }
        
        # Trigger evictions by setting maxmemory below current usage
        set current_memory [s used_memory]
        set target_memory [expr {$current_memory * 3 / 4}]
        r config set maxmemory $target_memory
        r config set maxmemory-policy allkeys-lru
        
        # Generate evictions by adding new data
        r set foo bar
        
        # Verify evictions occurred
        set evicted_keys [s evicted_keys]
        assert {$evicted_keys > 0}
        assert_equal [s rdb_bgsave_in_progress] 1
        
        # Resume save at normal speed
        r config set rdb-key-save-delay 0
        waitForBgsave r
        
        # Verify snapshot contains original keys
        catch {r debug reload nosave}
        for {set i 0} {$i < 1000} {incr i} {
            assert_equal [r get before_$i] "value_before_$i"
            assert_equal [r get int_$i] [expr {42 + $i}]
            assert_equal [r lrange lst_$i 0 -1] [list "L1" "L2" "R1" "R2"]
            assert_equal [lsort [r smembers set_$i]] [list "B1" "B2"]
            assert_equal [r zscore zset_$i "Z1"] 1
            assert_equal [r hget hash_$i "H1"] "a"
            assert_equal [r pfcount hll_$i] 1
            assert_equal [r zcard geo_$i] 1
            assert_equal [r zcard geo_set_$i] 1
        }
    } {} {needs:debug}
}

start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    test "comprehensive modifications on all data types during forkless bgsave" {
        
        # Create initial dataset with 1000 keys
        createComplexDatasetForVerification r 1000
        
        # Start save with slow speed to keep it running during modifications
        r config set rdb-key-save-delay 1000
        r config set bgsave-default-method forkless
        r bgsave        
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "bgsave didn't start"
        }
        
        # Overwrite keys during save - all data types.
        set rd [valkey_deferring_client]
        set outstanding 0
        set saw_save_in_progress 0
        for {set i 0} {$i < 1000} {incr i} {
            $rd append before_$i "value_after_$i"
            $rd incr int_$i
            $rd set after_$i "VALUE_AFTER_$i"
            $rd lpush lst_$i LL2 LL1
            $rd rpush lst_$i RR1 RR2
            $rd sadd set_$i BB1 BB2
            $rd zadd zset_$i 5 Z2
            $rd hset hash_$i H1 c
            $rd pfadd hll_$i PF2
            $rd bitfield bits_$i SET u4 0 0 INCRBY u4 0 1
            $rd geoadd geo_$i -122.1592 47.5976 bellevue
            $rd geosearchstore geo_set_$i geo_$i FROMLONLAT -122.191729 47.685821 BYRADIUS 10 mi
            $rd geosearchstore geo_set_dist_$i geo_$i FROMLONLAT -122.191729 47.685821 BYRADIUS 10 mi ASC COUNT 10 STOREDIST
            $rd xadd stream_$i "*" D1 V2
            $rd xreadgroup GROUP group_$i consumer_after_$i COUNT 1 STREAMS stream_$i >
            $rd hsetex hashttl_$i EX 10000 FIELDS 1 HTTL1 a
            # There is a chance that our client is blocked but we don't know it, because
            # as a deferring client we never read replies.  If we are blocked we would
            # keep sending commands forever, which accumulate on the server side and can
            # overflow the buffers.  So stop periodically and consume replies - that is
            # the mechanism that waits until we are unblocked.
            incr outstanding 16
            if {$outstanding >= 320} {
                for {set j 0} {$j < $outstanding} {incr j} { $rd read }
                set outstanding 0
                if {[s rdb_bgsave_in_progress] == 1} { set saw_save_in_progress 1 }
            }
        }
        
        # Verify changes happened while the save was running
        assert {[s rdb_changes_since_last_save] > 0}
        assert_equal $saw_save_in_progress 1
        
        # Speed up save and wait for completion
        r config set rdb-key-save-delay 0
        waitForBgsave r
        $rd close
        
        # Verify snapshot contains original keys
        catch {r debug reload nosave}
        for {set i 0} {$i < 1000} {incr i} {
            assert_equal [r get before_$i] "value_before_$i"
            assert_equal [r get int_$i] [expr {42 + $i}]
            assert_equal [r lrange lst_$i 0 -1] [list "L1" "L2" "R1" "R2"]
            assert_equal [lsort [r smembers set_$i]] [list "B1" "B2"]
            assert_equal [r zscore zset_$i "Z1"] 1
            assert_equal [r hget hash_$i "H1"] "a"
            assert_equal [r pfcount hll_$i] 1
            assert_equal [r zcard geo_$i] 1
            assert_equal [r zcard geo_set_$i] 1
        }
    } {} {needs:debug}
}

start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    test "store key deletion by georadius during forkless bgsave" {
        
        # Create initial dataset with geo data
        createComplexDatasetForVerification r 1000
        
        # Create additional zsets for georadius STORE operations
        r zadd georad_zset_delete_test 1 Z1 2 Z2
        r zadd georadmem_zset_test 1 Z1 2 Z2 3 Z3
        
        # Start save with stopped speed
        r config set rdb-key-save-delay 10000
        r config set bgsave-default-method forkless
        r bgsave
        
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "bgsave didn't start"
        }
        
        # Use GEORADIUS with STORE - deletes georad_zset_delete_test key
        r georadius geo_1 -122.191729 47.685821 5 mi STORE georad_zset_delete_test
        
        # Use GEORADIUSBYMEMBER with STORE - does not delete georadmem_zset_test as it returns 1 member
        r georadiusbymember geo_1 seattle 5 mi STORE georadmem_zset_test
        
        # Verify changes were made
        assert {[s rdb_changes_since_last_save] > 0}
        assert_equal [s rdb_bgsave_in_progress] 1
        
        # Resume save at normal speed
        r config set rdb-key-save-delay 0
        waitForBgsave r
        
        # Verify snapshot contains original keys
        catch {r debug reload nosave}
        
        # Original geo_1 key should be preserved
        assert_equal [r zcard geo_1] 1
        
        # Original zsets should be preserved (not deleted by STORE operations)
        assert_equal [r zcard georad_zset_delete_test] 2
        assert_equal [r zcard georadmem_zset_test] 3
    } {} {needs:debug}
}

start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    test "transactions during forkless bgsave" {
        
        # Populate 5 databases
        for {set db 0} {$db < 5} {incr db} {
            r select $db
            createComplexDatasetForVerification r 100
        }
        r select 0
        
        # Prepare transactions before save starts
        set rd0 [valkey_deferring_client]
        set rd1 [valkey_deferring_client]
        
        $rd0 select 0
        $rd0 multi
        $rd0 set int_1 bad
        $rd0 incrby int_2 2
        
        $rd1 select 1
        $rd1 multi
        $rd1 set int_1 bad
        $rd1 set int_2 bad1
        $rd1 lpush lst_3 bad1
        $rd1 sadd set_3 bad1
        $rd1 set newkey bad1
        
        # Start save with slow speed
        r config set rdb-key-save-delay 10000
        r config set bgsave-default-method forkless
        r bgsave
        
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "bgsave didn't start"
        }
        
        # Start more transactions during save
        set rd4 [valkey_deferring_client]
        set rd3 [valkey_deferring_client]
        
        $rd4 select 4
        $rd4 multi
        $rd4 hset hash_49 bad1 a
        $rd4 hset hash_10 H1 b
        $rd4 zadd zset_3 2 bad1
        $rd4 set newkey bad1
        $rd4 set another_newkey bad55
        $rd4 xadd newstream * D1 V2
        
        $rd3 select 3
        $rd3 multi
        $rd3 set aftersave bad1
        $rd3 xadd another_newstream * D1 V3
        
        assert_equal [s rdb_bgsave_in_progress] 1
        
        # Execute first 3 transactions
        $rd0 exec
        $rd1 exec
        $rd4 exec
        
        # Read all responses: select, multi, queued commands, exec
        # rd0: select(OK) multi(OK) set(QUEUED) incrby(QUEUED) exec(result)
        for {set i 0} {$i < 5} {incr i} { $rd0 read }
        # rd1: select(OK) multi(OK) set(QUEUED) set(QUEUED) lpush(QUEUED) sadd(QUEUED) set(QUEUED) exec(result)
        for {set i 0} {$i < 8} {incr i} { $rd1 read }
        # rd4: select(OK) multi(OK) hset(QUEUED) hset(QUEUED) zadd(QUEUED) set(QUEUED) set(QUEUED) xadd(QUEUED) exec(result)
        for {set i 0} {$i < 9} {incr i} { $rd4 read }
        
        # Verify transactions executed
        r select 0
        assert_equal [r get int_1] "bad"
        r select 1
        assert_equal [r get int_2] "bad1"
        r select 4
        assert_equal [r get newkey] "bad1"
        r select 3
        assert_equal [r exists aftersave] 0
        r select 4
        assert_equal [r xlen newstream] 1
        r select 3
        assert_equal [r xlen another_newstream] 0
        
        assert_equal [s rdb_bgsave_in_progress] 1
        
        # Resume save at normal speed
        r config set rdb-key-save-delay 0
        waitForBgsave r
        
        # Execute last transaction after save completes
        r select 3
        assert_equal [r exists aftersave] 0
        $rd3 exec
        # consume rd3 replies: select(OK) multi(OK) set(QUEUED) xadd(QUEUED) exec(result)
        for {set i 0} {$i < 5} {incr i} { $rd3 read }
        assert_equal [r get aftersave] "bad1"
        assert_equal [r xlen another_newstream] 1
        
        # Close deferred clients
        $rd0 close
        $rd1 close
        $rd3 close
        $rd4 close
        
        # Verify snapshot contains original keys
        catch {r debug reload nosave}
        
        # Original keys should be preserved in all databases
        r select 0
        assert_equal [r get before_0] "value_before_0"
        assert_equal [r get int_1] "43"
        r select 1
        assert_equal [r get int_2] "44"
        assert_equal [r llen lst_3] 4
        r select 4
        assert_equal [r exists newkey] 0
        assert_equal [r exists another_newkey] 0
        assert_equal [r exists newstream] 0
        r select 3
        assert_equal [r exists aftersave] 0
        assert_equal [r exists another_newstream] 0
    } {} {needs:debug}
}

start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    foreach first_type {fork forkless} {
        foreach second_type {fork forkless} {
            test "$first_type bgsave blocks $second_type bgsave" {
                r config set rdb-key-save-delay 1000000
                populate 100 "" 16

                r config set bgsave-default-method $first_type
                r bgsave
                wait_for_condition 50 100 {
                    [s rdb_bgsave_in_progress] == 1
                } else {
                    fail "$first_type bgsave did not start"
                }
                assert_equal [s rdb_current_bgsave_type] $first_type

                r config set bgsave-default-method $second_type
                assert_error "ERR Background save already in progress" {r bgsave}

                r bgsave cancel
                r config set rdb-key-save-delay 0
                waitForBgsave r
            }
        }
    }
}

test {client freed during loading} {
    start_server [list overrides [list key-load-delay 50 loading-process-events-interval-bytes 1024 rdbcompression no save "900 1"]] {
        # create a big rdb that will take long to load. it is important
        # for keys to be big since the server processes events only once in 2mb.
        # 100mb of rdb, 100k keys will load in more than 5 seconds
        r debug populate 100000 key 1000

        restart_server 0 false false

        # make sure it's still loading
        assert_equal [s loading] 1

        # connect and disconnect 5 clients
        set clients {}
        for {set j 0} {$j < 5} {incr j} {
            lappend clients [valkey_deferring_client]
        }
        foreach rd $clients {
            $rd debug log bla
        }
        foreach rd $clients {
            $rd read
        }
        foreach rd $clients {
            $rd close
        }

        # make sure the server freed the clients
        wait_for_condition 100 100 {
            [s connected_clients] < 3
        } else {
            fail "clients didn't disconnect"
        }

        # make sure it's still loading
        assert_equal [s loading] 1

        # no need to keep waiting for loading to complete
        exec kill [srv 0 pid]
    }
}

start_server {} {
    test {Test RDB load info} {
        r debug populate 1000
        r save
        assert {[r lastsave] <= [lindex [r time] 0]}
        restart_server 0 true false
        wait_done_loading r
        assert {[s rdb_last_load_keys_expired] == 0}
        assert {[s rdb_last_load_keys_loaded] == 1000}

        r debug set-active-expire 0
        for {set j 0} {$j < 1024} {incr j} {
            r select [expr $j%16]
            r set $j somevalue px 10
        }
        after 20

        r save
        restart_server 0 true false
        wait_done_loading r
        assert {[s rdb_last_load_keys_expired] == 1024}
        assert {[s rdb_last_load_keys_loaded] == 1000}
    }
}

# Our COW metrics (Private_Dirty) work only on Linux
set system_name [string tolower [exec uname -s]]
set page_size [exec getconf PAGESIZE]
if {$system_name eq {linux} && $page_size == 4096} {

start_server {overrides {save ""}} {
    test {Test child sending info} {
        # make sure that rdb_last_cow_size and current_cow_size are zero (the test using new server),
        # so that the comparisons during the test will be valid
        assert {[s current_cow_size] == 0}
        assert {[s current_save_keys_processed] == 0}
        assert {[s current_save_keys_total] == 0}

        assert {[s rdb_last_cow_size] == 0}

        # using a 200us delay, the bgsave is empirically taking about 10 seconds.
        # we need it to take more than some 5 seconds, since the server only report COW once a second.
        r config set rdb-key-save-delay 200
        r config set loglevel debug

        # populate the db with 10k keys of 512B each (since we want to measure the COW size by
        # changing some keys and read the reported COW size, we are using small key size to prevent from
        # the "dismiss mechanism" free memory and reduce the COW size)
        set rd [valkey_deferring_client 0]
        $rd client reply off
        set size 500 ;# aim for the 512 bin (sds overhead)
        set cmd_count 10000
        set AAA [string repeat A $size]
        for {set k 0} {$k < $cmd_count} {incr k} {
            $rd set key$k $AAA
        }
        $rd client reply on
        assert_equal OK [$rd read]
        $rd close

        # start background rdb save
        r bgsave

        set current_save_keys_total [s current_save_keys_total]
        if {$::verbose} {
            puts "Keys before bgsave start: $current_save_keys_total"
        }

        # on each iteration, we will write some key to the server to trigger copy-on-write, and
        # wait to see that it reflected in INFO.
        set iteration 1
        set key_idx 0
        while 1 {
            # take samples before writing new data to the server
            set cow_size [s current_cow_size]
            if {$::verbose} {
                puts "COW info before copy-on-write: $cow_size"
            }

            set keys_processed [s current_save_keys_processed]
            if {$::verbose} {
                puts "current_save_keys_processed info : $keys_processed"
            }

            # trigger copy-on-write
            set modified_keys 16
            set BBB [string repeat B $size]
            for {set k 0} {$k < $modified_keys} {incr k} {
                r setrange key$key_idx 0 $BBB
                incr key_idx 1
            }

            # changing 16 keys (512B each) will create at least 8192 COW (2 pages), but we don't want the test
            # to be too strict, so we check for a change of at least 4096 bytes
            set exp_cow [expr $cow_size + 4096]
            # wait to see that current_cow_size value updated (as long as the child is in progress)
            wait_for_condition 80 100 {
                [s rdb_bgsave_in_progress] == 0 ||
                [s current_cow_size] >= $exp_cow &&
                [s current_save_keys_processed] > $keys_processed &&
                [s current_fork_perc] > 0
            } else {
                if {$::verbose} {
                    puts "COW info on fail: [s current_cow_size]"
                    puts [exec tail -n 100 < [srv 0 stdout]]
                }
                fail "COW info wasn't reported"
            }

            # assert that $keys_processed is not greater than total keys.
            assert_morethan_equal $current_save_keys_total $keys_processed

            # for no accurate, stop after 2 iterations
            if {!$::accurate && $iteration == 2} {
                break
            }

            # stop iterating if the bgsave completed
            if { [s rdb_bgsave_in_progress] == 0 } {
                break
            }

            incr iteration 1
        }

        # make sure we saw report of current_cow_size
        if {$iteration < 2 && $::verbose} {
            puts [exec tail -n 100 < [srv 0 stdout]]
        }
        assert_morethan_equal $iteration 2

        # if bgsave completed, check that rdb_last_cow_size (fork exit report)
        # is at least 90% of last rdb_active_cow_size.
        if { [s rdb_bgsave_in_progress] == 0 } {
            set final_cow [s rdb_last_cow_size]
            set cow_size [expr $cow_size * 0.9]
            if {$final_cow < $cow_size && $::verbose} {
                puts [exec tail -n 100 < [srv 0 stdout]]
            }
            assert_morethan_equal $final_cow $cow_size
        }
    }
}
} ;# system_name

exec cp -f tests/assets/scriptbackup.rdb $server_path
start_server [list overrides [list "dir" $server_path "dbfilename" "scriptbackup.rdb" "appendonly" "no"]] {
    # the script is: "return redis.call('set', 'foo', 'bar')""
    # its sha1   is: a0c38691e9fffe4563723c32ba77a34398e090e6
    test {script won't load anymore if it's in rdb} {
        assert_equal [r script exists a0c38691e9fffe4563723c32ba77a34398e090e6] 0
    }
}

start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    foreach bgsave_type {"fork" "forkless"} {
        test "failed bgsave $bgsave_type prevents writes" {
            # Make sure the server saves an RDB on shutdown
            r config set save "900 1"

            r config set rdb-key-save-delay 10000000
            populate 1000
            r set x x
            r config set bgsave-default-method $bgsave_type
            if {$bgsave_type eq "forkless"} {
                # Inject a failure to make the save fail: a directory whose name
                # collides with the RDB file makes the final rename fail. We
                # can't just kill -9 like fork-based bgsave since there is no
                # child process.
                set rdb_path [file join [lindex [r config get dir] 1] [lindex [r config get dbfilename] 1]]
                file delete -force $rdb_path
                file mkdir $rdb_path
            }
            r bgsave
            wait_for_condition 50 100 {
                [s rdb_bgsave_in_progress] == 1
            } else {
                fail "$bgsave_type bgsave didn't start"
            }
            if {$bgsave_type ne "forkless"} {
                set pid1 [get_child_pid 0]
                catch {exec kill -9 $pid1}
            }
            r config set rdb-key-save-delay 0
            waitForBgsave r

            # make sure a read command succeeds
            assert_equal [r get x] x

            # make sure a write command fails
            assert_error {MISCONF *} {r set x y}

            # repeat with script
            assert_error {MISCONF *} {r eval {
                return redis.call('set','x',1)
                } 1 x
            }
            assert_equal {x} [r eval {
                return redis.call('get','x')
                } 1 x
            ]

            # again with script using shebang
            assert_error {MISCONF *} {r eval {#!lua
                return redis.call('set','x',1)
                } 1 x
            }
            assert_equal {x} [r eval {#!lua flags=no-writes
                return redis.call('get','x')
                } 1 x
            ]

            r config set rdb-key-save-delay 0
            if {$bgsave_type eq "forkless"} {
                # Remove the blocking directory so the recovery save can succeed.
                file delete [file join [lindex [r config get dir] 1] [lindex [r config get dbfilename] 1]]
            }
            r config set bgsave-default-method $bgsave_type
            r bgsave
            waitForBgsave r

            # server is writable again
            r set x y
        } {OK}
    }
}

start_server {} {
    test {RDB Load from incompatible version preserves data} {
        # Set test keys
        r set testkey1 "value1"
        r set testkey2 "value2" 

        # Use RDB with version 987. 
        # This emulates a full sync from a server with a future version
        set server_dir [lindex [r config get dir] 1]
        set rdb_filename [lindex [r config get dbfilename] 1]
        set rdb_path "$server_dir/$rdb_filename"
        exec cp tests/assets/encodings-rdb987.rdb $rdb_path

        # Reload will trigger the rdbLoad code path with the RDBFLAGS_EMPTY_DATA flag
        catch {r debug reload nosave}
        
        # Check that version error appears in logs
        verify_log_message 0 "*Can't handle RDB format version*" 0

        # Verify we don't enter the flushing code path
        verify_no_log_message 0 "*RDB signature and version check passed*" 0

        # Verify our original data is not flushed
        assert_equal [r get testkey1] "value1"
        assert_equal [r get testkey2] "value2"
    }
}


start_server {overrides {forkless-infrastructure-enabled yes save ""}} {
    test {bgsave-default-method can be set to forkless with forkless-infrastructure-enabled} {
        r config set bgsave-default-method forkless
        assert_equal [lindex [r config get bgsave-default-method] 1] "forkless"
    }
}

start_server {overrides {forkless-infrastructure-enabled yes bgsave-default-method forkless}} {
    test {BGSAVE uses forkless when bgsave-default-method is forkless} {
        r set key value
        set result [r bgsave]
        assert_match "*Background saving started*" $result
        waitForBgsave r
        assert_equal [s rdb_last_bgsave_type] "forkless"
    }
}

start_server {overrides {bgsave-default-method fork}} {
    test {BGSAVE uses fork when bgsave-default-method is fork} {
        r set key value
        set result [r bgsave]
        assert_match "*Background saving started*" $result
        waitForBgsave r
        assert_equal [s rdb_last_bgsave_type] "fork"
    }
}

start_server {overrides {save ""}} {
    test {bgsave-default-method forkless is rejected without forkless-infrastructure-enabled} {
        # forkless-infrastructure-enabled defaults to no here.
        assert_error "*forkless-infrastructure-enabled yes*" {
            r config set bgsave-default-method forkless
        }
        # The value is unchanged and remains fork.
        assert_equal [lindex [r config get bgsave-default-method] 1] "fork"
    }
}

test {Server refuses to start with bgsave-default-method forkless and no forkless-infrastructure-enabled} {
    catch {exec $::VALKEY_SERVER_BIN --bgsave-default-method forkless} err
    assert_match {*forkless-infrastructure-enabled yes*} $err
}

test {Server starts with bgsave-default-method before forkless-infrastructure-enabled yes} {
    start_server {overrides {bgsave-default-method forkless forkless-infrastructure-enabled yes save ""}} {
        assert_equal [lindex [r config get bgsave-default-method] 1] "forkless"
    }
}

} ;# tags

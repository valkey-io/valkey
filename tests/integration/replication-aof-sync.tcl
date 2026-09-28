source tests/support/aofmanifest.tcl

proc log_file_matches {log pattern} {
    set fp [open $log r]
    set content [read $fp]
    close $fp
    string match $pattern $content
}

# Test that reuses the RDB file from full sync as the AOF base file
# when aof-use-rdb-preamble is enabled and disk-based replication is used.

proc get_aof_manifest_path {r} {
    set dir [lindex [$r config get dir] 1]
    set appenddirname [lindex [$r config get appenddirname] 1]
    set appendfilename [lindex [$r config get appendfilename] 1]
    return [file join $dir $appenddirname $appendfilename$::manifest_suffix]
}

proc assert_aof_base_compression {r mode} {
    set manifest_path [get_aof_manifest_path $r]
    set base_name [get_cur_base_aof_name $manifest_path]
    assert {$base_name ne ""}
    set base_path [file join [file dirname $manifest_path] $base_name]

    if {$mode eq "no"} {
        assert_equal "VALKEY" [read_binary_file_prefix $base_path 6]
    } else {
        binary scan [read_binary_file_prefix $base_path 7] cu* envelope
        set codec [dict get {lz4 1 zstd 2} $mode]
        # V C S / envelope version / codec / reserved / RDB stream kind.
        assert_equal [list 86 67 83 1 $codec 0 1] $envelope
    }
}

tags {"repl external:skip"} {

    # Test 1: Disk-based full sync with aof-use-rdb-preamble yes should
    # reuse the RDB file as AOF base file
    test "Reuse RDB from disk-based full sync as AOF base file" {
        start_server {overrides {appendonly yes aof-use-rdb-preamble yes repl-diskless-sync no save ""}} {
            set primary [srv 0 client]
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]

            for {set i 0} {$i < 100} {incr i} {
                $primary set "key:$i" "value:$i"
            }
            waitForBgrewriteaof $primary

            start_server {overrides {appendonly yes aof-use-rdb-preamble yes repl-diskless-sync no save ""}} {
                set replica [srv 0 client]
                set replica_log [srv 0 stdout]

                # Start replication
                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica

                # Check AOF state is ON (not WAIT_REWRITE)
                wait_for_condition 50 100 {
                    [string match "*aof_rewrite_in_progress:0*" [$replica info persistence]]
                } else {
                    fail "AOF rewrite still in progress"
                }

                # Verify the log message about reusing RDB
                wait_for_condition 50 100 {
                    [log_file_matches $replica_log "*Reused RDB file from primary sync as AOF base file*"]
                } else {
                    fail "Expected log message about reusing RDB file not found"
                }

                # Verify AOF base file exists and is RDB format
                set manifest_path [get_aof_manifest_path $replica]
                set base_name [get_cur_base_aof_name $manifest_path]
                assert {$base_name ne ""}
                assert {[string match "*.rdb" $base_name]}

                # Verify data integrity
                assert_equal 100 [$replica dbsize]
                for {set i 0} {$i < 100} {incr i} {
                    assert_equal "value:$i" [$replica get "key:$i"]
                }
            }
        }
    }

    # Test 2: After sync, new writes should go to AOF incr file
    test "New writes after RDB-reuse sync go to AOF incr file" {
        start_server {overrides {appendonly yes aof-use-rdb-preamble yes repl-diskless-sync no save ""}} {
            set primary [srv 0 client]
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]

            for {set i 0} {$i < 50} {incr i} {
                $primary set "key:$i" "value:$i"
            }
            waitForBgrewriteaof $primary

            start_server {overrides {appendonly yes aof-use-rdb-preamble yes repl-diskless-sync no save ""}} {
                set replica [srv 0 client]
                set replica_log [srv 0 stdout]

                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica

                wait_for_condition 50 100 {
                    [log_file_matches $replica_log "*Reused RDB file from primary sync as AOF base file*"]
                } else {
                    fail "Expected log message not found"
                }

                # Write new data to primary after sync
                for {set i 50} {$i < 100} {incr i} {
                    $primary set "key:$i" "value:$i"
                }

                # Wait for replication to propagate
                wait_for_ofs_sync $primary $replica

                # Verify incr AOF file exists
                set manifest_path [get_aof_manifest_path $replica]
                set incr_name [get_last_incr_aof_name $manifest_path]
                assert {$incr_name ne ""}

                # Verify all data is present
                assert_equal 100 [$replica dbsize]
                for {set i 0} {$i < 100} {incr i} {
                    assert_equal "value:$i" [$replica get "key:$i"]
                }
            }
        }
    }

    # Test 3: Replica restart should load AOF correctly after RDB-reuse sync
    test "Replica restart loads AOF correctly after RDB-reuse sync" {
        start_server {overrides {appendonly yes aof-use-rdb-preamble yes repl-diskless-sync no save ""}} {
            set primary [srv 0 client]
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]

            for {set i 0} {$i < 50} {incr i} {
                $primary set "key:$i" "value:$i"
            }
            waitForBgrewriteaof $primary

            start_server {overrides {appendonly yes aof-use-rdb-preamble yes repl-diskless-sync no save ""}} {
                set replica [srv 0 client]
                set replica_log [srv 0 stdout]

                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica

                wait_for_condition 50 100 {
                    [log_file_matches $replica_log "*Reused RDB file from primary sync as AOF base file*"]
                } else {
                    fail "Expected log message not found"
                }

                # Write more data
                for {set i 50} {$i < 80} {incr i} {
                    $primary set "key:$i" "value:$i"
                }
                wait_for_ofs_sync $primary $replica

                # Stop replica and disconnect from primary
                $replica replicaof no one

                # Restart replica (this tests AOF loading)
                restart_server 0 true false
                set replica [srv 0 client]
                wait_done_loading $replica

                # Verify data integrity after restart
                assert_equal 80 [$replica dbsize]
                for {set i 0} {$i < 80} {incr i} {
                    assert_equal "value:$i" [$replica get "key:$i"]
                }
            }
        }
    }

    # The primary selects the disk-based sync codec, while the replica selects
    # the on-disk format that is reused as its AOF base.
    foreach case {
        {lz4 lz4}
        {lz4 no}
        {lz4 zstd}
        {zstd zstd}
    } {
        lassign $case primary_mode replica_mode
        test "Disk-based [string toupper $primary_mode] sync is reused using replica rdbcompression $replica_mode" {
            start_server {overrides {repl-diskless-sync no save ""}} {
                set primary [srv 0 client]
                set primary_host [srv 0 host]
                set primary_port [srv 0 port]

                if {($primary_mode eq "zstd" || $replica_mode eq "zstd") &&
                    ![config_value_supported $primary rdbcompression zstd]} {
                    skip "zstd is not supported by this build"
                }
                $primary config set rdbcompression $primary_mode
                $primary config set repl-compression $primary_mode

                set key_prefix "rcomp-$primary_mode-$replica_mode"
                for {set i 0} {$i < 40} {incr i} {
                    $primary set "$key_prefix-key:$i" "value:$i"
                }
                set primary_loglines [count_log_lines 0]

                start_server {overrides {appendonly yes aof-use-rdb-preamble yes repl-diskless-sync no save ""}} {
                    set replica [srv 0 client]
                    set replica_log [srv 0 stdout]
                    $replica config set rdbcompression $replica_mode
                    $replica config set repl-compression $primary_mode

                    $replica replicaof $primary_host $primary_port
                    wait_for_sync $replica
                    wait_for_log_messages -1 \
                        [list "*Starting BGSAVE for SYNC with target: disk*compression: $primary_mode*"] \
                        $primary_loglines 50 100

                    # The locally encoded sync RDB is reused without another rewrite.
                    wait_for_condition 50 100 {
                        [log_file_matches $replica_log "*Reused RDB file from primary sync as AOF base file*"]
                    } else {
                        fail "Expected sync RDB to be reused as the AOF base"
                    }
                    assert_equal 0 [count_message_lines $replica_log "Background append only file rewriting started"]
                    assert_aof_base_compression $replica $replica_mode

                    # Data is correct after loading the locally encoded sync RDB.
                    assert_equal 40 [$replica dbsize]
                    for {set i 0} {$i < 40} {incr i} {
                        assert_equal "value:$i" [$replica get "$key_prefix-key:$i"]
                    }

                    # Add an incremental command, then verify both AOF files reload.
                    $primary set "$key_prefix-after-sync" incremental
                    wait_for_ofs_sync $primary $replica
                    $replica replicaof no one
                    restart_server 0 true false
                    set replica [srv 0 client]
                    wait_done_loading $replica

                    assert_equal 41 [$replica dbsize]
                    for {set i 0} {$i < 40} {incr i} {
                        assert_equal "value:$i" [$replica get "$key_prefix-key:$i"]
                    }
                    assert_equal incremental [$replica get "$key_prefix-after-sync"]
                }
            }
        }
    }

    # Test 4: aof-use-rdb-preamble no should fall back to bgrewriteaof
    test "Disk-based sync with aof-use-rdb-preamble no uses bgrewriteaof" {
        start_server {overrides {appendonly yes aof-use-rdb-preamble no repl-diskless-sync no save ""}} {
            set primary [srv 0 client]
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]

            for {set i 0} {$i < 50} {incr i} {
                $primary set "key:$i" "value:$i"
            }

            start_server {overrides {appendonly yes aof-use-rdb-preamble no repl-diskless-sync no save ""}} {
                set replica [srv 0 client]
                set replica_log [srv 0 stdout]

                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica

                # Should NOT see the RDB reuse log message
                after 1000
                assert {![log_file_matches $replica_log "*Reused RDB file from primary sync as AOF base file*"]}

                # Structural assertion: AOF base exists (produced by bgrewriteaof)
                # and uses .aof suffix (not .rdb) since rdb-preamble is off.
                waitForBgrewriteaof $replica
                set manifest_path [get_aof_manifest_path $replica]
                set base_name [get_cur_base_aof_name $manifest_path]
                assert {$base_name ne ""}
                assert {[string match "*.aof" $base_name]}

                assert_equal 50 [$replica dbsize]
                for {set i 0} {$i < 50} {incr i} {
                    assert_equal "value:$i" [$replica get "key:$i"]
                }
            }
        }
    }

    # Test 5: Diskless sync with aof-use-rdb-preamble yes should fall back
    # to bgrewriteaof (no RDB file on disk to reuse)
    test "Diskless sync with aof-use-rdb-preamble yes uses bgrewriteaof fallback" {
        start_server {overrides {appendonly yes aof-use-rdb-preamble yes repl-diskless-sync yes repl-diskless-sync-delay 0 save ""}} {
            set primary [srv 0 client]
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]

            for {set i 0} {$i < 50} {incr i} {
                $primary set "key:$i" "value:$i"
            }

            start_server {overrides {appendonly yes aof-use-rdb-preamble yes repl-diskless-load flush-before-load save ""}} {
                set replica [srv 0 client]
                set replica_log [srv 0 stdout]

                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica

                after 1000
                assert {![log_file_matches $replica_log "*Reused RDB file from primary sync as AOF base file*"]}

                # Structural assertion: bgrewriteaof should produce an AOF base
                # file with .rdb suffix (since rdb-preamble is yes).
                waitForBgrewriteaof $replica
                set manifest_path [get_aof_manifest_path $replica]
                set base_name [get_cur_base_aof_name $manifest_path]
                assert {$base_name ne ""}
                assert {[string match "*.rdb" $base_name]}

                assert_equal 50 [$replica dbsize]
                for {set i 0} {$i < 50} {incr i} {
                    assert_equal "value:$i" [$replica get "key:$i"]
                }
            }
        }
    }

    # Test 6: Diskless sync with a stale local RDB must NOT reuse it.
    # This verifies that disk_based_sync=0 prevents the optimization even
    # when a leftover dump.rdb exists on the replica.
    test "Diskless sync with stale local RDB does not reuse it as AOF base" {
        start_server {overrides {appendonly yes aof-use-rdb-preamble yes repl-diskless-sync yes repl-diskless-sync-delay 0 save ""}} {
            set primary [srv 0 client]
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]

            for {set i 0} {$i < 50} {incr i} {
                $primary set "key:$i" "value:$i"
            }

            start_server {overrides {appendonly yes aof-use-rdb-preamble yes repl-diskless-load flush-before-load save ""}} {
                set replica [srv 0 client]
                set replica_log [srv 0 stdout]

                # Create stale data and persist it as a local RDB so that
                # dump.rdb exists when the diskless sync completes.
                $replica set stale_key stale_value
                $replica bgsave
                waitForBgsave $replica

                # Now do a diskless full sync
                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica

                # The stale RDB must NOT have been reused
                after 1000
                assert {![log_file_matches $replica_log "*Reused RDB file from primary sync as AOF base file*"]}

                # Structural assertion: bgrewriteaof produced the base file
                waitForBgrewriteaof $replica
                set manifest_path [get_aof_manifest_path $replica]
                set base_name [get_cur_base_aof_name $manifest_path]
                assert {$base_name ne ""}

                # Data must come from the primary, not the stale RDB
                assert_equal 50 [$replica dbsize]
                for {set i 0} {$i < 50} {incr i} {
                    assert_equal "value:$i" [$replica get "key:$i"]
                }
                assert_equal "" [$replica get stale_key]

                # Restart replica and verify data again. This catches the regression
                # where a stale dump.rdb was incorrectly reused as AOF base: at
                # runtime memory is correct (from socket), but after restart the
                # wrong AOF base would load stale data.
                $replica replicaof no one
                restart_server 0 true false
                set replica [srv 0 client]
                wait_done_loading $replica

                assert_equal 50 [$replica dbsize]
                for {set i 0} {$i < 50} {incr i} {
                    assert_equal "value:$i" [$replica get "key:$i"]
                }
                assert_equal "" [$replica get stale_key]
            }
        }
    }
}

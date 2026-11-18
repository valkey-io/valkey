source tests/support/aofmanifest.tcl

# Waits for the AOF rewrite that follows a full sync, so that the AOF is on.
proc wait_for_aof_on {r} {
    wait_for_condition 50 100 {
        [set info [$r info persistence]] ne "" &&
        [getInfoProperty $info aof_rewrite_in_progress] == 0 &&
        [getInfoProperty $info aof_rewrite_scheduled] == 0
    } else {
        fail "AOF rewrite didn't finish"
    }
}

# Restarts the server at the given level after killing it, like a crash, so that
# nothing is written at shutdown. With appendfsync everysec, a background fsync
# can postpone writing aof_buf, so wait until the buffer has been written first.
proc restart_killed_server {level} {
    wait_for_condition 50 100 {
        [s $level aof_buffer_length] == 0
    } else {
        fail "AOF buffer wasn't written"
    }
    catch {exec kill -9 [srv $level pid]}
    restart_server $level true false
}

# Restarts the replica at level 0, after killing it if 'crash' is set, and checks
# that it restored the replication ID and offset it had from its AOF. The caller's
# 'replica' gets the new client first, so a failed check doesn't break later tests.
# A restored offset that's too low still allows a partial resync, but the primary
# then sends some writes again, so the tests write INCR rather than SET.
proc restart_replica {crash} {
    upvar 1 replica replica
    set restored "*Restored replication ID [status $replica master_replid], offset [status $replica master_repl_offset] and DB *"
    set loglines [count_log_lines 0]
    if {$crash} {
        restart_killed_server 0
    } else {
        restart_server 0 true false
    }
    set replica [srv 0 client]
    verify_log_message 0 $restored $loglines
}

proc aof_manifest_path {r} {
    set dir [lindex [$r config get dir] 1]
    set appenddirname [lindex [$r config get appenddirname] 1]
    set appendfilename [lindex [$r config get appendfilename] 1]
    return [file join $dir $appenddirname $appendfilename$::manifest_suffix]
}

# An AOF block with the given header fields.
proc aof_block {fields payload} {
    return "#HDR:v1;len:[string length $payload];$fields\r\n$payload"
}

# Creates an AOF with a base file that sets k, and the given INCR files.
proc create_test_aof {aof_dirpath args} {
    exec rm -rf $aof_dirpath
    create_aof $aof_dirpath "$aof_dirpath/appendonly.aof.1.base.aof" {append_to_aof [formatCommand set k v]}
    set manifest "file appendonly.aof.1.base.aof seq 1 type b\n"
    set seq 0
    foreach data $args {
        incr seq
        create_aof $aof_dirpath "$aof_dirpath/appendonly.aof.$seq.incr.aof" {append_to_aof $data}
        append manifest "file appendonly.aof.$seq.incr.aof seq $seq type i\n"
    }
    create_aof_manifest $aof_dirpath "$aof_dirpath/appendonly.aof.manifest" {append_to_manifest $manifest}
}

tags {"repl aof external:skip"} {
    foreach integrity {no yes} {
        set overrides [list appendonly yes aof-replication-offsets yes aof-integrity-check $integrity]
        start_server [list overrides [concat $overrides {repl-ping-replica-period 3600}]] {
            set primary [srv 0 client]
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]

            start_server [list overrides $overrides] {
                set replica [srv 0 client]
                $replica replicaof $primary_host $primary_port
                # Stay a replica after a restart.
                $replica config rewrite
                wait_for_sync $replica
                wait_for_aof_on $replica

                test "Replica partially resyncs after a restart (integrity $integrity)" {
                    $primary select 0
                    $primary incr a
                    $primary select 5
                    $primary incr b
                    wait_for_ofs_sync $primary $replica
                    $primary config resetstat

                    restart_replica 0
                    wait_for_sync $replica
                    wait_for_ofs_sync $primary $replica
                    assert_equal 0 [status $primary sync_full]
                    assert_equal 1 [status $primary sync_partial_ok]
                    assert_equal [$primary debug digest] [$replica debug digest]
                }

                test "Replica partially resyncs after a crash (integrity $integrity)" {
                    # Without the block written at shutdown, the offset comes from the
                    # block of the last write.
                    $primary incr c
                    wait_for_ofs_sync $primary $replica
                    $primary config resetstat

                    restart_replica 1
                    wait_for_sync $replica
                    wait_for_ofs_sync $primary $replica
                    assert_equal 0 [status $primary sync_full]
                    assert_equal 1 [status $primary sync_partial_ok]
                    assert_equal [$primary debug digest] [$replica debug digest]
                }

                test "Replica restores the DB of the replication stream (integrity $integrity)" {
                    # PUBLISH makes the replication stream select DB 5, while the
                    # replica only writes to DB 0 in its AOF.
                    $primary select 0
                    $primary multi
                    $primary incr d
                    $primary select 5
                    $primary publish ch hello
                    $primary exec
                    wait_for_ofs_sync $primary $replica
                    $primary config resetstat

                    restart_replica 1
                    wait_for_sync $replica
                    assert_equal 0 [status $primary sync_full]
                    assert_equal 1 [status $primary sync_partial_ok]

                    # The replication stream is still in DB 5, so this write has no SELECT.
                    $primary set e 5
                    wait_for_ofs_sync $primary $replica
                    $replica select 5
                    assert_equal 5 [$replica get e]
                    assert_equal [$primary debug digest] [$replica debug digest]
                }

                test "Replica partially resyncs with several INCR files (integrity $integrity)" {
                    # A failed rewrite leaves the INCR file it opened, which gets the
                    # next writes and has to record the replication ID again. With a
                    # key, the delay keeps the rewrite running until it's killed.
                    $primary incr j
                    wait_for_ofs_sync $primary $replica
                    wait_for_aof_on $replica
                    test_defer_fail [list catch [list $replica config set rdb-key-save-delay 0]]
                    $replica config set rdb-key-save-delay 10000000
                    $replica bgrewriteaof
                    catch {exec kill -9 [get_child_pid 0]}
                    waitForBgrewriteaof $replica
                    $replica config set rdb-key-save-delay 0
                    assert_equal 2 [regexp -all {type i} [read_binary_file [aof_manifest_path $replica]]]
                    $primary incr j
                    wait_for_ofs_sync $primary $replica
                    $primary config resetstat

                    restart_replica 1
                    wait_for_sync $replica
                    wait_for_ofs_sync $primary $replica
                    assert_equal 0 [status $primary sync_full]
                    assert_equal 1 [status $primary sync_partial_ok]
                    assert_equal [$primary debug digest] [$replica debug digest]
                }

                test "Replica fully resyncs if its last writes have no offset (integrity $integrity)" {
                    # Not saved in the config file, so it's enabled again after the restart.
                    test_defer_fail [list catch [list $replica config set aof-replication-offsets yes]]
                    $replica config set aof-replication-offsets no
                    $primary set f 6
                    wait_for_ofs_sync $primary $replica
                    $primary config resetstat

                    restart_server 0 true false
                    set replica [srv 0 client]
                    wait_for_sync $replica
                    wait_for_aof_on $replica
                    assert_equal 1 [status $primary sync_full]
                    assert_equal [$primary debug digest] [$replica debug digest]
                }

                if {!$integrity} {
                    test "Replica fully resyncs if the AOF ends with commands without a header" {
                        $primary set g 7
                        wait_for_ofs_sync $primary $replica
                        set aof [get_last_incr_aof_path $replica]
                        catch {$replica shutdown}

                        # Like writes appended by an older version.
                        append_binary_file $aof "*3\r\n\$3\r\nSET\r\n\$1\r\nx\r\n\$1\r\ny\r\n"
                        $primary config resetstat
                        restart_server 0 true false
                        set replica [srv 0 client]
                        wait_for_sync $replica
                        wait_for_aof_on $replica
                        assert_equal 1 [status $primary sync_full]
                        assert_equal [$primary debug digest] [$replica debug digest]
                    }
                }

                test "Primary lets its replicas partially resync after a restart (integrity $integrity)" {
                    $primary set h 8
                    # The offset grows without writes to the AOF.
                    $primary publish ch hello
                    wait_for_ofs_sync $primary $replica
                    set replid [status $primary master_replid]

                    restart_server -1 true false
                    set primary [srv -1 client]
                    wait_for_sync $replica
                    assert_equal $replid [status $primary master_replid2]
                    assert_equal 0 [status $primary sync_full]
                    assert_equal 1 [status $primary sync_partial_ok]
                    assert_equal [$primary debug digest] [$replica debug digest]
                }

                test "Primary lets its replicas partially resync after a crash (integrity $integrity)" {
                    # Without the block written at shutdown, the offset comes from the
                    # block of the last write.
                    $primary incr counter
                    wait_for_ofs_sync $primary $replica
                    set replid [status $primary master_replid]
                    set offset [status $primary master_repl_offset]

                    restart_killed_server -1
                    set primary [srv -1 client]
                    wait_for_sync $replica
                    wait_for_ofs_sync $primary $replica
                    assert_equal $replid [status $primary master_replid2]
                    assert_equal [expr {$offset + 1}] [status $primary second_repl_offset]
                    assert_equal 0 [status $primary sync_full]
                    assert_equal 1 [status $primary sync_partial_ok]
                    assert_equal [$primary debug digest] [$replica debug digest]
                }

                test "The first command of a new INCR file selects its DB (integrity $integrity)" {
                    # A write, a PUBLISH from another DB and BGREWRITEAOF in the same event
                    # loop iteration: the rewrite flushes the write, and selects the DB of
                    # the replication stream at the end of the INCR file it closes, before
                    # the new one starts. The next write, in that DB, has to select it again.
                    # The closed INCR file is kept to check its end.
                    test_config_set $primary aof-disable-auto-gc yes
                    set closed [get_last_incr_aof_path $primary]
                    set rd [valkey_deferring_client -1]
                    test_defer [list catch [list $rd close]]
                    $rd select 3
                    $rd read
                    $rd set k0 v0
                    $rd read
                    $rd write [format_command set k1 v1]
                    $rd write [format_command select 5]
                    $rd write [format_command publish ch hello]
                    $rd write [format_command bgrewriteaof]
                    $rd flush
                    for {set i 0} {$i < 4} {incr i} {
                        $rd read
                    }
                    $rd set k2 v2
                    $rd read
                    $rd close
                    wait_for_aof_on $primary
                    # The INCR file the rewrite closed ends with a block that has both the
                    # write and the SELECT. Had the write been flushed on its own before,
                    # the rewrite would have had nothing to flush, and the new INCR file
                    # would select its DB anyway.
                    set block "[formatCommand set k1 v1][formatCommand SELECT 5]"
                    assert_equal ";\r\n$block" [string range [read_binary_file $closed] end-[expr {[string length $block] + 2}] end]
                    $primary config set aof-disable-auto-gc no
                    $primary debug loadaof
                    $primary select 5
                    assert_equal v2 [$primary get k2]
                }

                test "valkey-check-aof accepts the AOF (integrity $integrity)" {
                    foreach r [list $primary $replica] {
                        catch {exec $::VALKEY_CHECK_AOF_BIN [aof_manifest_path $r]} output
                        assert_match "*All AOF files and manifest are valid*" $output
                    }
                }

                test "Promoted replica records its new replication ID (integrity $integrity)" {
                    $replica replicaof no one
                    $replica config rewrite
                    set replid [status $replica master_replid]
                    $replica set i 9
                    # With appendfsync everysec, the write can come after the reply.
                    set aof [get_last_incr_aof_path $replica]
                    wait_for_condition 50 100 {
                        [string match "*#HDR:v1;len:*;replid:$replid;reploff:*" [read_binary_file $aof]]
                    } else {
                        fail "The AOF doesn't have the new replication ID"
                    }

                    set offset [status $replica master_repl_offset]
                    restart_server 0 true false
                    set replica [srv 0 client]
                    assert_equal $replid [status $replica master_replid2]
                    assert_equal [expr {$offset + 1}] [status $replica second_repl_offset]
                }
            }
        }
    }

    # Dual channel sync buffers writes while the replica loads the RDB file. The
    # RDB file is reused as the AOF base, and the buffered writes go to the new
    # INCR file, so the restarted replica can partially resync.
    start_server {overrides {repl-diskless-sync yes repl-diskless-sync-delay 0 dual-channel-replication-enabled yes rdbcompression no repl-ping-replica-period 3600}} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]
        populate 1000 base 1000

        start_server {overrides {appendonly yes aof-replication-offsets yes repl-diskless-load disabled dual-channel-replication-enabled yes loading-process-events-interval-bytes 1024}} {
            set replica [srv 0 client]

            test "Replica partially resyncs after a dual channel sync" {
                # Keep RDB loading unfinished (30s for 1000 keys) until the
                # writes below are buffered on the replica.
                $replica config set key-load-delay 30000
                test_defer [list catch [list $replica config set key-load-delay 0]]
                $replica replicaof $primary_host $primary_port
                wait_for_condition 200 50 {
                    [status $replica loading] == 1 &&
                    [string match "*slave*,state=bg_transfer*,type=main-channel*" [$primary info replication]]
                } else {
                    fail "Sync didn't start"
                }
                # The replica buffers these writes while it loads the RDB. Send
                # enough data to allocate at least two 16 KB buffer blocks.
                set buf_before [status $replica replicas_repl_buffer_size]
                set val [string repeat x 1000]
                for {set i 0} {$i < 50} {incr i} {
                    $primary set during$i $val-$i
                }
                wait_for_condition 200 50 {
                    [status $replica loading] == 1 &&
                    [status $replica replicas_repl_buffer_size] >= $buf_before + 32768
                } else {
                    fail "Replica didn't buffer the writes while loading the RDB"
                }
                $replica config set key-load-delay 0
                wait_for_condition 100 100 {
                    [s -1 rdb_bgsave_in_progress] == 0 && [status $replica master_link_status] eq "up"
                } else {
                    fail "Replica didn't sync"
                }
                assert {[status $replica replicas_repl_buffer_peak] >= $buf_before + 32768}
                wait_for_aof_on $replica
                $replica config rewrite
                $primary set after 1
                wait_for_ofs_sync $primary $replica
                $primary config resetstat

                restart_server 0 true false
                set replica [srv 0 client]
                wait_for_sync $replica
                wait_for_ofs_sync $primary $replica
                assert_equal 0 [status $primary sync_full]
                assert_equal 1 [status $primary sync_partial_ok]
                assert_equal [$primary debug digest] [$replica debug digest]
            }
        }
    }

    foreach preamble {yes no} {
        foreach integrity {no yes} {
            set overrides [list appendonly yes aof-replication-offsets yes aof-integrity-check $integrity aof-use-rdb-preamble $preamble]
            start_server [list overrides [concat $overrides {repl-ping-replica-period 3600}]] {
                set primary [srv 0 client]
                set primary_host [srv 0 host]
                set primary_port [srv 0 port]

                start_server [list overrides $overrides] {
                    set replica [srv 0 client]
                    $replica replicaof $primary_host $primary_port
                    $replica config rewrite
                    wait_for_sync $replica
                    wait_for_aof_on $replica

                    test "Replica partially resyncs from the base file (preamble $preamble, integrity $integrity)" {
                        $primary select 3
                        $primary incr a
                        # The replication stream moves to DB 5 without a write, so the base
                        # file has no data in the DB of the replication stream.
                        $primary select 5
                        $primary publish ch hello
                        wait_for_ofs_sync $primary $replica
                        $replica bgrewriteaof
                        wait_for_aof_on $replica
                        if {!$preamble} {
                            assert_match "*#HDR:v1;len:*;replid:*;reploff:*" [read_binary_file [get_base_aof_path $replica]]
                        }
                        catch {exec $::VALKEY_CHECK_AOF_BIN [aof_manifest_path $replica]} output
                        assert_match "*All AOF files and manifest are valid*" $output
                        # Only the base file has the offset.
                        assert_equal 0 [file size [get_last_incr_aof_path $replica]]
                        $primary config resetstat

                        restart_replica 1
                        wait_for_sync $replica
                        assert_equal 0 [status $primary sync_full]
                        assert_equal 1 [status $primary sync_partial_ok]

                        # The DB of the replication stream is restored too: this write has
                        # no SELECT.
                        $primary set b 2
                        wait_for_ofs_sync $primary $replica
                        $replica select 5
                        assert_equal 2 [$replica get b]
                        assert_equal [$primary debug digest] [$replica debug digest]
                    }
                }
            }
        }
    }

    start_server {overrides {repl-ping-replica-period 3600}} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        # With appendfsync always, each write gets its own block.
        start_server {overrides {appendonly yes aof-replication-offsets yes aof-integrity-check no appendfsync always}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port
            $replica config rewrite
            wait_for_sync $replica
            wait_for_aof_on $replica

            test "The replication ID is recorded once per INCR file" {
                for {set i 0} {$i < 3} {incr i} {
                    $primary incr counter
                    wait_for_ofs_sync $primary $replica
                }
                set aof [read_binary_file [get_last_incr_aof_path $replica]]
                assert_equal 3 [regexp -all {#HDR:} $aof]
                assert_equal 1 [regexp -all {replid:} $aof]
            }

            test "A full sync that reuses the RDB file records the replication ID in the new INCR file" {
                $primary incr counter
                wait_for_ofs_sync $primary $replica
                # Promoted and made a replica again without a write in between, the
                # replica full syncs from the same primary, so it gets back the
                # replication ID that its AOF recorded last.
                set reused [count_log_message 0 "Reused RDB file from primary sync as AOF base file"]
                $replica replicaof no one
                $replica replicaof $primary_host $primary_port
                wait_for_sync $replica
                wait_for_aof_on $replica
                assert_equal [expr {$reused + 1}] [count_log_message 0 "Reused RDB file from primary sync as AOF base file"]
                $primary incr counter
                wait_for_ofs_sync $primary $replica
                $primary config resetstat

                restart_replica 1
                wait_for_sync $replica
                wait_for_ofs_sync $primary $replica
                assert_equal 0 [status $primary sync_full]
                assert_equal 1 [status $primary sync_partial_ok]
                assert_equal [$primary debug digest] [$replica debug digest]
            }
        }

        start_server {overrides {appendonly yes aof-replication-offsets yes aof-integrity-check no}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port
            $replica config rewrite
            wait_for_sync $replica
            wait_for_aof_on $replica

            test "Replica partially resyncs after aof-integrity-check is enabled across a restart" {
                # The INCR file was written without integrity, so its headers aren't
                # verified, but they still record the offset.
                $primary incr counter
                wait_for_ofs_sync $primary $replica
                $primary config resetstat
                set fd [open [srv 0 config_file] a]
                puts $fd "aof-integrity-check yes"
                close $fd

                restart_replica 1
                assert_equal yes [lindex [$replica config get aof-integrity-check] 1]
                wait_for_sync $replica
                wait_for_ofs_sync $primary $replica
                assert_equal 0 [status $primary sync_full]
                assert_equal 1 [status $primary sync_partial_ok]
                assert_equal [$primary debug digest] [$replica debug digest]
            }
        }

        start_server {overrides {appendonly yes aof-replication-offsets yes}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port
            $replica config rewrite
            wait_for_sync $replica
            wait_for_aof_on $replica

            test "Replica fully resyncs if the DB of the replication stream doesn't exist" {
                # The base file records DB 10 as the DB of the replication stream, and
                # the replica restarts with 10 databases.
                $primary select 10
                $primary publish ch hello
                wait_for_ofs_sync $primary $replica
                wait_for_aof_on $replica
                $replica bgrewriteaof
                wait_for_aof_on $replica
                $primary config resetstat
                set fd [open [srv 0 config_file] a]
                puts $fd "databases 10"
                close $fd

                restart_killed_server 0
                set replica [srv 0 client]
                wait_for_sync $replica
                assert_equal 1 [status $primary sync_full]
                assert_equal 0 [status $primary sync_partial_ok]
            }
        }
    }

    # The block written at shutdown only records the replication offset, so failing
    # to write or fsync it doesn't stop the shutdown, even with appendfsync always.
    # The next INCR file is a device: writes to /dev/full fail, and /dev/null can't
    # be fsynced.
    if {$::tcl_platform(os) eq "Linux" && [file exists /dev/full] && [file exists /dev/null]} {
        start_server {overrides {repl-ping-replica-period 3600}} {
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]

            foreach {dev what error} {
                /dev/full write "*Error writing to the AOF file*"
                /dev/null fsync "*Can't fsync the replication offset to the AOF*"
            } {
                start_server {overrides {appendonly yes aof-replication-offsets yes appendfsync always}} {
                    set replica [srv 0 client]
                    $replica replicaof $primary_host $primary_port
                    wait_for_sync $replica
                    wait_for_aof_on $replica

                    test "A failed $what of the block written at shutdown doesn't stop the shutdown" {
                        set manifest [aof_manifest_path $replica]
                        set seq 0
                        foreach {- s} [regexp -all -inline {seq (\d+) type i} [read_binary_file $manifest]] {
                            if {$s > $seq} {set seq $s}
                        }
                        set next "[lindex [$replica config get appendfilename] 1].[expr {$seq + 1}].incr.aof"
                        file link -symbolic [file join [file dirname $manifest] $next] $dev
                        $replica bgrewriteaof
                        wait_for_aof_on $replica
                        assert_match "*$next*" [read_binary_file $manifest]

                        # Nothing else is written to the new INCR file before the shutdown.
                        set loglines [count_log_lines 0]
                        catch {$replica shutdown}
                        wait_for_log_messages 0 [list $error] $loglines 50 100
                        wait_for_log_messages 0 {"*ready to exit, bye bye*"} $loglines 50 100
                        verify_no_log_message 0 "*Exiting...*" $loglines
                    }
                }
            }
        }
    }

    # Like an RDB file, a primary without a replication backlog records no offset,
    # since it isn't part of a replication history.
    start_server {overrides {appendonly yes aof-replication-offsets yes}} {
        test "A primary without a replication backlog records no offset" {
            r set a 1
            set aof [get_last_incr_aof_path r]
            wait_for_condition 50 100 {
                [string match "*\r\na\r\n\$1\r\n1\r\n" [read_binary_file $aof]]
            } else {
                fail "The write isn't in the AOF"
            }
            assert_no_match "*reploff*" [read_binary_file $aof]
            restart_server 0 true false
            assert_equal 1 [r get a]
            assert_equal 0000000000000000000000000000000000000000 [s master_replid2]
        }
    }

    # Crafted AOF files, without integrity. A primary keeps the restored replication
    # ID / offset as its secondary ones.
    set defaults {appendonly yes appendfilename appendonly.aof appenddirname appendonlydir aof-replication-offsets yes aof-integrity-check no}
    set server_path [tmpdir server.aof-repl-offsets]
    set aof_dirpath "$server_path/appendonlydir"
    set replid 0123456789abcdef0123456789abcdef01234567
    set set_a [formatCommand set a 1]
    set set_b [formatCommand set b 2]
    set set_c [formatCommand set c 3]
    # The offset is only restored if the AOF ends with a block that records it. The
    # torn last block ends after a whole command, so only its header shows it's torn.
    foreach {desc incr restored} [list \
        "a block that records the offset" \
            "[aof_block "replid:$replid;reploff:100;" $set_a]" 100 \
        "an empty block" \
            "[aof_block "replid:$replid;reploff:100;" $set_a][aof_block "reploff:150;" ""]" 150 \
        "a torn last block" \
            "[aof_block "replid:$replid;reploff:100;" $set_a][string range [aof_block "reploff:200;" $set_b$set_c] 0 end-[string length $set_c]]" 100 \
        "a command without a header" \
            "[aof_block "replid:$replid;reploff:100;" $set_a]$set_b" {} \
        "a malformed header" \
            "[aof_block "replid:$replid;reploff:100;" $set_a][aof_block "reploff:x;" $set_b]" {} \
        "a malformed header before a header without replid" \
            "[aof_block "replid:$replid;reploff:100;" $set_a][aof_block "reploff:x;" $set_b][aof_block "reploff:200;" $set_c]" {} \
        "a header of an unknown version" \
            "[aof_block "replid:$replid;reploff:100;" $set_a]#HDR:v2;len:0;reploff:150;\r\n" 100 \
        "no replication ID in the file" \
            "[aof_block "reploff:100;" $set_a]" {} \
        "a block that ends inside a transaction" \
            "[aof_block "replid:$replid;reploff:100;" [formatCommand multi]$set_a]" {} \
        "an offset of LLONG_MAX" \
            "[aof_block "replid:$replid;reploff:9223372036854775807;" $set_a]" {} \
    ] {
        set verb [expr {$restored eq {} ? "doesn't restore" : "restores"}]
        test "Primary $verb the offset from crafted AOF files: $desc" {
            create_test_aof $aof_dirpath $incr
            start_server [list overrides [concat $defaults [list dir $server_path]] keep_persistence true] {
                if {$restored eq {}} {
                    assert_equal 0000000000000000000000000000000000000000 [s master_replid2]
                } else {
                    assert_equal $replid [s master_replid2]
                    assert_equal [expr {$restored + 1}] [s second_repl_offset]
                }
            }
        }
    }

    # Each INCR file records the replication ID before its first offset.
    test "Primary doesn't restore the offset from crafted AOF files: no replication ID in the last INCR file" {
        create_test_aof $aof_dirpath [aof_block "replid:$replid;reploff:100;" $set_a] [aof_block "reploff:150;" $set_b]
        start_server [list overrides [concat $defaults [list dir $server_path]] keep_persistence true] {
            r select 0
            assert_equal 2 [r get b]
            assert_equal 0000000000000000000000000000000000000000 [s master_replid2]
        }
    }

    test "Primary doesn't restore the offset with aof-replication-offsets no" {
        create_test_aof $aof_dirpath [aof_block "replid:$replid;reploff:100;" $set_a]
        start_server [list overrides [concat $defaults [list dir $server_path aof-replication-offsets no]] keep_persistence true] {
            r select 0
            assert_equal 1 [r get a]
            assert_equal 0000000000000000000000000000000000000000 [s master_replid2]
        }
    }

    # A torn last block is truncated as a whole, at its header, by the server and
    # by valkey-check-aof --fix.
    set block [aof_block "replid:$replid;reploff:100;" $set_a]
    set incr_path "$aof_dirpath/appendonly.aof.1.incr.aof"
    foreach {desc torn} [list \
        "only a header" "#HDR:v1;len:[string length $set_b];reploff:200;\r\n" \
        "a whole command" [string range [aof_block "reploff:200;" $set_b$set_c] 0 end-3] \
    ] {
        foreach fix {no yes} {
            test "A torn last block with $desc is truncated at its header (valkey-check-aof --fix: $fix)" {
                create_test_aof $aof_dirpath $block$torn
                if {$fix} {
                    catch {exec $::VALKEY_CHECK_AOF_BIN "$aof_dirpath/appendonly.aof.manifest"} output
                    assert_match "*extends past the end of the file*" $output
                    exec $::VALKEY_CHECK_AOF_BIN --fix "$aof_dirpath/appendonly.aof.manifest" << "y\n"
                    assert_equal [string length $block] [file size $incr_path]
                }
                set overrides [concat $defaults [list dir $server_path aof-load-truncated [expr {$fix ? "no" : "yes"}]]]
                start_server [list overrides $overrides keep_persistence true] {
                    r select 0
                    assert_equal [string length $block] [file size $incr_path]
                    assert_equal 1 [r get a]
                    assert_equal 0 [r exists b]
                    assert_equal $replid [s master_replid2]
                    assert_equal 101 [s second_repl_offset]
                }
            }
        }
    }

    # A verified empty block advances the safe truncation boundary too, so a torn
    # header after it is truncated without failing integrity verification.
    foreach integrity {no yes} {
        test "A torn header after an empty block is truncated (integrity $integrity)" {
            if {$integrity} {
                set base_cmd [formatCommand set k v]
                set p1 "#HDR:v1;len:[string length $set_a];replid:$replid;reploff:100;checksum:"
                set c1 [crc64 [crc64 0 $p1] ";\r\n$set_a"]
                set p2 "#HDR:v1;len:0;reploff:150;checksum:"
                set c2 [crc64 [crc64 $c1 $p2] ";\r\n"]
                set valid "$p1[format %llu $c1];\r\n$set_a$p2[format %llu $c2];\r\n"
                exec rm -rf $aof_dirpath
                create_aof $aof_dirpath "$aof_dirpath/appendonly.aof.1.base.aof" {
                    append_to_aof "$base_cmd#CRC:[format %llu [crc64 0 $base_cmd]]\r\n"
                }
                create_aof $aof_dirpath $incr_path {append_to_aof "$valid#HDR:v1;len:10"}
                set manifest "file appendonly.aof.1.base.aof seq 1 type b integrity 1 checksum 0\nfile appendonly.aof.1.incr.aof seq 1 type i integrity 1\n"
                create_aof_manifest $aof_dirpath "$aof_dirpath/appendonly.aof.manifest" {
                    append_to_manifest "$manifest# manifest-checksum: [format %llu [crc64 0 $manifest]]\n"
                }
            } else {
                set valid "[aof_block "replid:$replid;reploff:100;" $set_a][aof_block "reploff:150;" ""]"
                create_test_aof $aof_dirpath "$valid#HDR:v1;len:10"
            }
            set overrides [concat $defaults [list dir $server_path aof-integrity-check $integrity aof-load-truncated yes]]
            start_server [list overrides $overrides keep_persistence true] {
                r select 0
                assert_equal [string length $valid] [file size $incr_path]
                assert_equal 1 [r get a]
                assert_equal $replid [s master_replid2]
                assert_equal 151 [s second_repl_offset]
            }
        }
    }

    # Also when the last block is corrupt rather than torn: --fix doesn't truncate
    # inside it, so the server loads the result without truncating it.
    test "valkey-check-aof --fix truncates a corrupt last block at its header" {
        set bad_c [string map [list "\$1\r\nc\r\n" "\$X\r\nc\r\n"] $set_c]
        create_test_aof $aof_dirpath $block[aof_block "reploff:200;" $set_b$bad_c]
        catch {exec $::VALKEY_CHECK_AOF_BIN "$aof_dirpath/appendonly.aof.manifest"} output
        assert_match "*is not valid*" $output
        exec $::VALKEY_CHECK_AOF_BIN --fix "$aof_dirpath/appendonly.aof.manifest" << "y\n"
        assert_equal [string length $block] [file size $incr_path]
        start_server [list overrides [concat $defaults [list dir $server_path aof-load-truncated no]] keep_persistence true] {
            r select 0
            assert_equal 1 [r get a]
            assert_equal 0 [r exists b]
            assert_equal $replid [s master_replid2]
            assert_equal 101 [s second_repl_offset]
        }

        # If the last file is a RESP base written with integrity whose CRC covers
        # malformed RESP, --fix updates its #CRC line after truncating.
        exec rm -rf $aof_dirpath
        set base_body "$set_a$bad_c"
        create_aof $aof_dirpath "$aof_dirpath/appendonly.aof.1.base.aof" {
            append_to_aof "$base_body#CRC:[format %llu [crc64 0 $base_body]]\r\n"
        }
        set manifest "file appendonly.aof.1.base.aof seq 1 type b integrity 1\n"
        create_aof_manifest $aof_dirpath "$aof_dirpath/appendonly.aof.manifest" {
            append_to_manifest "$manifest# manifest-checksum: [format %llu [crc64 0 $manifest]]\n"
        }
        exec $::VALKEY_CHECK_AOF_BIN --fix "$aof_dirpath/appendonly.aof.manifest" << "y\n"
        exec $::VALKEY_CHECK_AOF_BIN "$aof_dirpath/appendonly.aof.manifest"
        start_server [list overrides [concat $defaults [list dir $server_path aof-integrity-check yes aof-load-truncated no]] keep_persistence true] {
            r select 0
            assert_equal 1 [r get a]
            assert_equal 0 [r exists c]
        }
    }

    test "valkey-check-aof --truncate-to-timestamp truncates the block that has the timestamp" {
        set ts_block [aof_block "replid:$replid;reploff:100;" "#TS:100\r\n$set_a"]
        create_test_aof $aof_dirpath $ts_block[aof_block "reploff:200;" "#TS:200\r\n$set_b#TS:300\r\n$set_c"]
        exec $::VALKEY_CHECK_AOF_BIN --truncate-to-timestamp 250 "$aof_dirpath/appendonly.aof.manifest"
        assert_equal [string length $ts_block] [file size $incr_path]
        start_server [list overrides [concat $defaults [list dir $server_path aof-load-truncated no]] keep_persistence true] {
            r select 0
            assert_equal 1 [r get a]
            assert_equal 0 [r exists b]
            assert_equal $replid [s master_replid2]
            assert_equal 101 [s second_repl_offset]
        }

        # If the last file is a RESP base written with integrity, truncating it to a
        # timestamp updates its #CRC line so the checker and server still accept it.
        exec rm -rf $aof_dirpath
        set base_body "#TS:100\r\n$set_a#TS:200\r\n$set_b"
        create_aof $aof_dirpath "$aof_dirpath/appendonly.aof.1.base.aof" {
            append_to_aof "$base_body#CRC:[format %llu [crc64 0 $base_body]]\r\n"
        }
        set manifest "file appendonly.aof.1.base.aof seq 1 type b integrity 1\n"
        create_aof_manifest $aof_dirpath "$aof_dirpath/appendonly.aof.manifest" {
            append_to_manifest "$manifest# manifest-checksum: [format %llu [crc64 0 $manifest]]\n"
        }
        exec $::VALKEY_CHECK_AOF_BIN --truncate-to-timestamp 150 "$aof_dirpath/appendonly.aof.manifest"
        exec $::VALKEY_CHECK_AOF_BIN "$aof_dirpath/appendonly.aof.manifest"
        start_server [list overrides [concat $defaults [list dir $server_path aof-integrity-check yes aof-load-truncated no]] keep_persistence true] {
            r select 0
            assert_equal 1 [r get a]
            assert_equal 0 [r exists b]
        }
    }
}

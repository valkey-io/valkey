source tests/support/aofmanifest.tcl

tags {"rdb-compression external:skip needs:debug"} {

proc dump_rdb_path {client} {
    return [file join [lindex [$client config get dir] 1] dump.rdb]
}

proc read_dump_rdb_header_bytes {client} {
    return [read_binary_file_prefix [dump_rdb_path $client] 8]
}

proc assert_lz4_rdb_envelope {client} {
    binary scan [read_binary_file_prefix [dump_rdb_path $client] 7] cu* bytes
    # V C S / envelope version / LZ4 codec / reserved / RDB stream kind.
    assert_equal [list 86 67 83 1 1 0 1] $bytes
}

proc assert_lz4_rdb_checksum_flags {client expected} {
    set vcs_envelope_size 7
    set lz4_frame_magic_size 4
    set frame_flg_offset [expr {$vcs_envelope_size + $lz4_frame_magic_size}]
    binary scan [read_binary_file_prefix [dump_rdb_path $client] [expr {$frame_flg_offset + 1}]] cu* bytes
    # LZ4 frame magic 0x184D2204 is stored in little-endian byte order.
    assert_equal [list 4 34 77 24] [lrange $bytes $vcs_envelope_size [expr {$frame_flg_offset - 1}]]
    set frame_flg [lindex $bytes $frame_flg_offset]
    # LZ4 Frame Format FLG bits 4 and 2 enable block and content checksums.
    set has_block_checksum [expr {($frame_flg & 0x10) != 0}]
    set has_content_checksum [expr {($frame_flg & 0x04) != 0}]
    assert_equal $expected $has_block_checksum
    assert_equal $expected $has_content_checksum
}

proc assert_zstd_rdb_checksum_flag {client expected} {
    # The zstd frame header begins after the seven-byte VCS envelope. Its
    # four-byte magic is followed by a descriptor whose bit 2 is the content
    # checksum flag.
    set frame_descriptor_offset [expr {7 + 4}]
    binary scan [read_binary_file_prefix [dump_rdb_path $client] [expr {$frame_descriptor_offset + 1}]] cu* bytes
    set frame_descriptor [lindex $bytes $frame_descriptor_offset]
    set has_content_checksum [expr {($frame_descriptor & 0x04) != 0}]
    assert_equal $expected $has_content_checksum
}

proc rdbcompression_supported {client mode} {
    set old [lindex [$client config get rdbcompression] 1]
    set supported [expr {[catch {$client config set rdbcompression $mode}] == 0}]
    catch {$client config set rdbcompression $old}
    return $supported
}

proc write_rdb_test_dataset {client prefix} {
    $client flushall
    for {set i 0} {$i < 12} {incr i} {
        $client set "${prefix}:str:$i" [string repeat "${prefix}:value:$i " 16]
    }
    $client lpush "${prefix}:list" a b c d e
    $client sadd "${prefix}:set" alpha beta gamma
    $client zadd "${prefix}:zset" 1 one 2 two 3 three
    $client hset "${prefix}:hash" f1 v1 f2 [string repeat "${prefix}:hash " 8]
    $client xadd "${prefix}:stream" * f1 s1 f2 [string repeat "${prefix}:stream " 4]
    $client xadd "${prefix}:stream" * f1 s2 f2 tail
}

proc assert_rdb_test_dataset {client prefix} {
    assert_equal [string repeat "${prefix}:value:0 " 16] [$client get "${prefix}:str:0"]
    assert_equal 17 [$client dbsize]
    assert_equal 5 [$client llen "${prefix}:list"]
    assert_equal 3 [$client scard "${prefix}:set"]
    assert_equal 3 [$client zcard "${prefix}:zset"]
    assert_equal v1 [$client hget "${prefix}:hash" f1]
    assert_equal 2 [$client xlen "${prefix}:stream"]
}

set ::rdbcompression_zstd_supported 0

start_server {overrides {save "" enable-debug-command local}} {
    set ::rdbcompression_zstd_supported [rdbcompression_supported r zstd]

    test {RDB save and load round-trip with LZ4 compression} {
        r config set rdbcompression lz4
        r flushall
        createComplexDataset r 1000

        # DUMP/RESTORE uses its existing value encoding rather than wrapping
        # individual values in the whole-RDB compression envelope.
        set dump_value [string repeat "dump-restore-value " 100]
        r set dump-restore:key $dump_value
        set serialized [r dump dump-restore:key]
        assert_not_equal "VCS" [string range $serialized 0 2]
        r del dump-restore:key
        assert_equal "OK" [r restore dump-restore:key 0 $serialized]
        assert_equal $dump_value [r get dump-restore:key]

        set digest [debug_digest]

        assert_equal "OK" [r save]
        assert_lz4_rdb_envelope r
        assert_lz4_rdb_checksum_flags r 1
        set loglines [count_log_lines 0]
        assert_equal "OK" [r debug reload nosave]
        verify_log_message 0 "*Logical RDB CRC64 skipped for streaming-compressed input*" $loglines
        r config rewrite
        restart_server 0 true false

        assert_equal "lz4" [lindex [r config get rdbcompression] 1]
        set newdigest [debug_digest]
        assert {$digest eq $newdigest}
    }

    test {RDB save and load round-trip with ZSTD compression} {
        if {![rdbcompression_supported r zstd]} {
            skip "zstd is not supported by this build"
        }

        set prefix "zstd-round-trip"
        r config set rdbcompression zstd
        write_rdb_test_dataset r $prefix
        set digest [debug_digest]

        assert_equal "OK" [r save]
        binary scan [read_binary_file_prefix [dump_rdb_path r] 7] cu* bytes
        assert_equal [list 86 67 83 1 2 0 1] $bytes
        assert_zstd_rdb_checksum_flag r 1
        r config rewrite
        restart_server 0 true false

        assert_equal "zstd" [lindex [r config get rdbcompression] 1]
        assert_equal $digest [debug_digest]
        assert_rdb_test_dataset r $prefix
    }

    test {Empty LZ4-compressed RDB saves and loads correctly} {
        r config set rdbcompression lz4
        r flushall

        assert_equal 0 [r dbsize]
        assert_equal "OK" [r save]
        r config rewrite
        assert_lz4_rdb_envelope r

        restart_server 0 true false

        assert_equal "lz4" [lindex [r config get rdbcompression] 1]
        assert_equal 0 [r dbsize]
    }

    test {RDB files load across compression configuration modes} {
        foreach {save_mode load_mode expected_header} {
            yes yes VALKEY
            no no VALKEY
            lz4 lzf VCS
            lzf lz4 VALKEY
        } {
            set detail "RDB saved with $save_mode and loaded with $load_mode"
            r config set rdbcompression $save_mode
            r flushall
            createComplexDataset r 1000
            set digest [debug_digest]

            assert_equal "OK" [r save]
            set actual_header [string range [read_dump_rdb_header_bytes r] 0 [expr {[string length $expected_header] - 1}]]
            assert_equal $expected_header $actual_header $detail
            r config set rdbcompression $load_mode
            r config rewrite
            restart_server 0 true false

            assert_equal $load_mode [lindex [r config get rdbcompression] 1] $detail
            assert_equal $digest [debug_digest] $detail
        }
    }

    test {Changing compression config during active BGSAVE does not affect the in-flight save} {
        r config set rdbcompression lz4
        r config set rdb-key-save-delay 10000
        with_cleanup {
            r flushall
            for {set i 0} {$i < 128} {incr i} {
                r set "bgsave-race:$i" [string repeat "payload:$i " 128]
            }

            assert_match {*Background saving started*} [r bgsave]
            wait_for_condition 200 10 {
                [s rdb_bgsave_in_progress] eq 1
            } else {
                fail "BGSAVE did not start in time"
            }

            # The child must keep the compression setting inherited at fork.
            r config set rdbcompression yes

            wait_for_condition 500 10 {
                [s rdb_bgsave_in_progress] eq 0
            } else {
                fail "BGSAVE did not finish in time"
            }
            r config set rdb-key-save-delay 0

            assert_equal "yes" [lindex [r config get rdbcompression] 1]
            assert_lz4_rdb_envelope r

            assert_equal "OK" [r save]
            assert_equal "VALKEY" [string range [read_dump_rdb_header_bytes r] 0 5]
        } {
            catch {r config set rdb-key-save-delay 0}
            catch {r config set rdbcompression yes}
        }
    }

    test {Switching between ZSTD and every other RDB compression mode preserves data} {
        if {![rdbcompression_supported r zstd]} {
            skip "zstd is not supported by this build"
        }

        set transitions {
            {zstd lz4}
            {lz4 zstd}
            {zstd yes}
            {yes zstd}
            {zstd no}
            {no zstd}
        }

        foreach transition $transitions {
            lassign $transition source_mode target_mode
            set prefix "$source_mode-to-$target_mode"
            r config set rdbcompression $source_mode
            assert_equal $source_mode [lindex [r config get rdbcompression] 1]
            r flushall
            write_rdb_test_dataset r $prefix
            set digest [debug_digest]

            assert_equal "OK" [r save]
            if {$source_mode eq "zstd"} {
                binary scan [read_binary_file_prefix [dump_rdb_path r] 7] cu* bytes
                assert_equal [list 86 67 83 1 2 0 1] $bytes
            } elseif {$source_mode eq "lz4"} {
                assert_lz4_rdb_envelope r
            } else {
                assert_equal "VALKEY" [string range [read_dump_rdb_header_bytes r] 0 5]
            }

            r config set rdbcompression $target_mode
            r config rewrite
            restart_server 0 true false

            assert_equal $target_mode [lindex [r config get rdbcompression] 1]
            assert_equal $digest [debug_digest]
            assert_rdb_test_dataset r $prefix
        }
    }

    test {Invalid compression config is rejected} {
        set previous [lindex [r config get rdbcompression] 1]
        set expected "no, yes, lzf, lz4"
        if {[rdbcompression_supported r zstd]} {
            append expected ", zstd"
        }
        assert_error "*argument(s) must be one of the following: $expected*" {
            r config set rdbcompression snappy
        }
        assert_equal $previous [lindex [r config get rdbcompression] 1]
    }

    test {ZSTD compression config is rejected when unsupported} {
        if {$::rdbcompression_zstd_supported} {
            skip "zstd is supported by this build"
        }
        assert_error "*argument(s) must be one of the following: no, yes, lzf, lz4*" {
            r config set rdbcompression zstd
        }
    }

    test {Truncated LZ4 frame is rejected on load} {
        r config set rdbcompression lz4
        r flushall
        set noisy_payload [randstring 4096 4096 alpha]
        for {set i 0} {$i < 32} {incr i} {
            r set "partial:$i" "${noisy_payload}:$i"
        }

        assert_equal "OK" [r save]
        set rdbfile [dump_rdb_path r]
        assert_lz4_rdb_envelope r
        set original [read_binary_file $rdbfile]

        with_cleanup {
            write_binary_file $rdbfile [string range $original 0 [expr {[string length $original] / 2}]]

            set failed [catch {r debug reload nosave} err]
            assert_equal 1 $failed
            assert_match "*Error trying to load the RDB*" $err

            r debug set-skip-checksum-validation 1
            set failed [catch {r debug reload nosave} err]
            assert_equal 1 $failed
            assert_match "*Error trying to load the RDB*" $err
        } {
            catch {r debug set-skip-checksum-validation 0}
            write_binary_file $rdbfile $original
        }
    }

    test {LZ4 compressed RDB detects a content checksum mismatch} {
        r config set rdbcompression lz4
        assert_equal "yes" [lindex [r config get rdbchecksum] 1]
        r flushall
        for {set i 0} {$i < 100} {incr i} {
            r set "footer:$i" [string repeat "payload$i " 100]
        }

        r save
        set rdbfile [dump_rdb_path r]
        set original [read_binary_file $rdbfile]

        with_cleanup {
            set checksum_offset [expr {[string length $original] - 1}]
            binary scan [string index $original $checksum_offset] cu checksum_byte
            set mutated [string replace $original $checksum_offset $checksum_offset \
                [binary format c [expr {$checksum_byte ^ 1}]]]
            write_binary_file $rdbfile $mutated

            set failed [catch {r debug reload nosave} err]
            assert_equal 1 $failed
            assert_match "*Error trying to load the RDB*" $err

            set loglines [count_log_lines 0]
            r debug set-skip-checksum-validation 1
            assert_equal "OK" [r debug reload nosave]
            verify_log_message 0 "*Logical RDB CRC64 skipped for streaming-compressed input*" $loglines
        } {
            catch {r debug set-skip-checksum-validation 0}
            write_binary_file $rdbfile $original
        }
    }

    test {ZSTD compressed RDB detects a content checksum mismatch and allows bypass} {
        if {![rdbcompression_supported r zstd]} {
            skip "zstd is not supported by this build"
        }

        r config set rdbcompression zstd
        assert_equal "yes" [lindex [r config get rdbchecksum] 1]
        r flushall
        for {set i 0} {$i < 100} {incr i} {
            r set "zstd-footer:$i" [string repeat "payload$i " 100]
        }

        r save
        assert_zstd_rdb_checksum_flag r 1
        set rdbfile [dump_rdb_path r]
        set fd [open $rdbfile r+]
        fconfigure $fd -translation binary
        seek $fd -1 end
        binary scan [read $fd 1] cu checksum_byte
        seek $fd -1 end
        puts -nonewline $fd [binary format c [expr {$checksum_byte ^ 1}]]
        close $fd

        set failed [catch {r debug reload nosave} err]
        assert_equal 1 $failed
        assert_match "*Error trying to load the RDB*" $err

        r debug set-skip-checksum-validation 1
        assert_equal "OK" [r debug reload nosave]
        r debug set-skip-checksum-validation 0
        assert_equal [string repeat "payload10 " 100] [r get zstd-footer:10]
    }

    test {RDB loader rejects incompatible VCS envelope fields without changing data} {
        r config set rdbcompression lz4
        r flushall
        r set incompatible-envelope:key [string repeat "payload " 100]

        assert_equal "OK" [r save]
        set digest [debug_digest]
        set rdbfile [dump_rdb_path r]
        set original [read_binary_file $rdbfile]

        with_cleanup {
            foreach case {
                {version 3 2}
                {codec 4 127}
                {reserved-byte 5 1}
                {stream-kind 6 127}
            } {
                lassign $case field offset value
                set mutated [string replace $original $offset $offset [binary format c $value]]
                write_binary_file $rdbfile $mutated
                set loglines [count_log_lines 0]

                set failed [catch {r debug reload nosave} err]
                assert_equal 1 $failed "VCS $field should be rejected"
                assert_match "*Error trying to load the RDB*" $err
                verify_log_message 0 "*Invalid or unsupported RDB stream envelope*" $loglines
                assert_equal $digest [debug_digest]
            }
        } {
            write_binary_file $rdbfile $original
        }
    }

    test {RDB loader ignores trailing data after an LZ4 frame like a plain RDB} {
        r config set rdbcompression lz4
        r flushall
        r set trailing-data:key value
        assert_equal "OK" [r save]

        set rdbfile [dump_rdb_path r]
        set original [read_binary_file $rdbfile]

        with_cleanup {
            write_binary_file $rdbfile "${original}trailing-data"
            assert_equal "OK" [r debug reload nosave]
            assert_equal value [r get trailing-data:key]
        } {
            write_binary_file $rdbfile $original
        }
    }

    test {LZ4 compressed RDB detects corruption in its compressed stream} {
        r config set rdbcompression lz4
        r flushall
        for {set i 0} {$i < 100} {incr i} {
            r set "corrupt:$i" [string repeat "testdata$i " 100]
        }

        assert_equal "OK" [r save]

        set rdbfile [dump_rdb_path r]
        set original [read_binary_file $rdbfile]
        set pos [expr {[string length $original] / 2}]
        binary scan [string index $original $pos] cu value
        set mutated [string replace $original $pos $pos [binary format c [expr {$value ^ 1}]]]

        with_cleanup {
            write_binary_file $rdbfile $mutated
            set failed [catch {r debug reload nosave} err]
            assert_equal 1 $failed
            assert_match "*Error trying to load the RDB*" $err
        } {
            write_binary_file $rdbfile $original
        }
    }

}

start_server {config "minimal.conf" args {"--rdbcompression lz4"}} {
    test {Startup accepts valid LZ4 compression config} {
        assert_equal "lz4" [lindex [r config get rdbcompression] 1]
    }
}

if {$::rdbcompression_zstd_supported} {
    start_server {config "minimal.conf" args {"--rdbcompression zstd"}} {
        test {Startup accepts valid ZSTD compression config} {
            assert_equal "zstd" [lindex [r config get rdbcompression] 1]
        }
    }
}

start_server {overrides {save "" enable-debug-command local rdbchecksum no}} {
    test {rdbchecksum controls LZ4 frame checksums} {
        r config set rdbcompression lz4
        r flushall
        for {set i 0} {$i < 50} {incr i} {
            r set "nocksum:$i" [string repeat "data$i " 100]
        }

        r save
        assert_lz4_rdb_envelope r
        assert_lz4_rdb_checksum_flags r 0
        set rdbfile [dump_rdb_path r]
        set digest [debug_digest]
        set loglines [count_log_lines 0]
        assert_equal "OK" [r debug reload nosave]
        verify_log_message 0 "*Logical RDB CRC64 skipped for streaming-compressed input*" $loglines

        restart_server 0 true false
        set newdigest [debug_digest]
        assert {$digest eq $newdigest}
        assert_equal [string repeat "data10 " 100] [r get nocksum:10]
    }

    test {rdbchecksum controls ZSTD frame checksums} {
        if {![rdbcompression_supported r zstd]} {
            skip "zstd is not supported by this build"
        }

        r config set rdbcompression zstd
        r flushall
        for {set i 0} {$i < 50} {incr i} {
            r set "zstd-nocksum:$i" [string repeat "data$i " 100]
        }

        assert_equal "OK" [r save]
        assert_zstd_rdb_checksum_flag r 0
        set digest [debug_digest]
        assert_equal "OK" [r debug reload nosave]

        restart_server 0 true false
        assert_equal $digest [debug_digest]
        assert_equal [string repeat "data10 " 100] [r get zstd-nocksum:10]
    }
}

foreach mode {lz4 zstd} {
    if {$mode eq "zstd" && !$::rdbcompression_zstd_supported} continue

    start_server [list overrides [list save "" appendonly yes aof-use-rdb-preamble yes rdbcompression $mode]] {
        test "AOF rewrite RDB preamble remains plain with $mode stream snapshots" {
            r set "aof-$mode:key" [string repeat "aof-$mode-value " 100]
            set digest [debug_digest]

            r bgrewriteaof
            waitForBgrewriteaof r

            set base_aof [get_base_aof_path r]
            assert {[file exists $base_aof]}
            assert_equal "VALKEY" [string range [read_binary_file_prefix $base_aof 7] 0 5]

            restart_server 0 true false
            assert_equal $digest [debug_digest]
            assert_equal [string repeat "aof-$mode-value " 100] [r get "aof-$mode:key"]
        }
    }
}

start_server {tags {"rdb-compression repl external:skip"} overrides {save ""}} {
    set replica [srv 0 client]

    start_server {overrides {save "" enable-debug-command local}} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        test {Full sync remains compatible with every streaming compression codec} {
            foreach mode {lz4 zstd} {
                if {$mode eq "zstd" && !$::rdbcompression_zstd_supported} continue

                $primary config set rdbcompression $mode
                $primary config set rdb-del-sync-files no
                $primary flushall
                for {set i 0} {$i < 300} {incr i} {
                    $primary set "repl:$i" [string repeat "payload$i " 40]
                }

                $primary config set repl-diskless-sync-delay 0
                $replica config set repl-diskless-load swapdb

                foreach diskless {no yes} {
                    $replica replicaof no one
                    $replica flushall
                    $primary config set repl-diskless-sync $diskless
                    # Prevent partial resynchronization from bypassing the RDB path
                    # on the second iteration.
                    $primary debug change-repl-id

                    $replica replicaof $primary_host $primary_port
                    wait_for_sync $replica
                    wait_done_loading $replica
                    assert_equal [$primary debug digest] [$replica debug digest]
                    assert_equal [string repeat "payload42 " 40] [$replica get repl:42]

                    if {$diskless eq "no"} {
                        assert {[file exists [dump_rdb_path $primary]]}
                        assert_equal "VALKEY" [string range [read_dump_rdb_header_bytes $primary] 0 5]
                    }
                }

                $primary set repl:post-sync "after-sync-$mode"
                wait_for_condition 50 100 {
                    [$replica get repl:post-sync] eq "after-sync-$mode"
                } else {
                    fail "Replica did not receive post-sync write with $mode"
                }
            }
        }

        $replica replicaof no one
    }
}

}

tags {"rdb-compression external:skip needs:debug needs:other-server compatible-redis"} {
    start_server {start-other-server 1 config "minimal.conf" overrides {save ""}} {
        set other_server [srv 0 client]
        $other_server config set rdbcompression yes
        $other_server flushall
        createComplexDataset $other_server 1000
        set compatibility_value [string repeat "other-server-lzf " 32]
        $other_server set compatibility:key $compatibility_value
        set expected_dbsize [$other_server dbsize]
        assert_equal "OK" [$other_server save]
        set other_rdb [file join [lindex [$other_server config get dir] 1] dump.rdb]

        start_server {config "minimal.conf" overrides {save "" enable-debug-command local}} {
            test {Current server loads an LZF RDB created by another server version} {
                set current_rdb [file join [lindex [r config get dir] 1] dump.rdb]
                file copy -force $other_rdb $current_rdb
                assert_equal "OK" [r debug reload nosave]
                assert_equal $expected_dbsize [r dbsize]
                assert_equal $compatibility_value [r get compatibility:key]
            }
        }
    }
}

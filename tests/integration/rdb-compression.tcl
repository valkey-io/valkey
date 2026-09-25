source tests/support/aofmanifest.tcl

tags {"rdb-compression external:skip needs:debug"} {

proc dump_rdb_path {client} {
    return [file join [lindex [$client config get dir] 1] dump.rdb]
}

proc read_dump_rdb_header_bytes {client} {
    return [read_binary_file_prefix [dump_rdb_path $client] 8]
}

proc assert_rdb_file_envelope {path mode} {
    binary scan [read_binary_file_prefix $path 7] cu* bytes
    set codec [dict get {lz4 1 zstd 2} $mode]
    # V C S / envelope version / codec / reserved / RDB stream kind.
    assert_equal [list 86 67 83 1 $codec 0 1] $bytes
}

proc assert_rdb_envelope {client mode} {
    assert_rdb_file_envelope [dump_rdb_path $client] $mode
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
    # The Zstd frame descriptor follows the seven-byte VCS envelope and
    # four-byte frame magic; bit 2 declares a content checksum.
    set frame_descriptor_offset 11
    binary scan [read_binary_file_prefix [dump_rdb_path $client] [expr {$frame_descriptor_offset + 1}]] cu* bytes
    assert_equal [list 86 67 83 1 2 0 1] [lrange $bytes 0 6]
    assert_equal [list 40 181 47 253] [lrange $bytes 7 10]
    set frame_descriptor [lindex $bytes $frame_descriptor_offset]
    assert_equal $expected [expr {($frame_descriptor & 0x04) != 0}]
}

set ::rdbcompression_zstd_supported 0
set ::rdbcompression_modes {lz4}

start_server {overrides {save "" enable-debug-command local}} {
    set ::rdbcompression_zstd_supported [config_value_supported r rdbcompression zstd]
    if {$::rdbcompression_zstd_supported} {
        lappend ::rdbcompression_modes zstd
    }

    test {DUMP and RESTORE remain independent of whole-RDB compression} {
        r config set rdbcompression lz4
        set dump_value [string repeat "dump-restore-value " 100]
        r set dump-restore:key $dump_value
        set serialized [r dump dump-restore:key]
        assert_not_equal "VCS" [string range $serialized 0 2]
        r del dump-restore:key
        assert_equal "OK" [r restore dump-restore:key 0 $serialized]
        assert_equal $dump_value [r get dump-restore:key]
    }

    foreach mode $::rdbcompression_modes {
        test "RDB save and load round-trip with [string toupper $mode] compression" {
            r config set rdbcompression $mode
            r flushall
            createComplexDataset r 1000

            set digest [debug_digest]
            assert_equal "OK" [r save]
            assert_rdb_envelope r $mode
            if {$mode eq "lz4"} {
                assert_lz4_rdb_checksum_flags r 1
            }
            set loglines [count_log_lines 0]
            assert_equal "OK" [r debug reload nosave]
            verify_log_message 0 "*Logical RDB CRC64 skipped for streaming-compressed input*" $loglines
            r config rewrite
            restart_server 0 true false

            assert_equal $mode [lindex [r config get rdbcompression] 1]
            assert_equal $digest [debug_digest]
        }

        test "Empty [string toupper $mode]-compressed RDB saves and loads correctly" {
            r config set rdbcompression $mode
            r flushall

            assert_equal 0 [r dbsize]
            assert_equal "OK" [r save]
            r config rewrite
            assert_rdb_envelope r $mode

            restart_server 0 true false

            assert_equal $mode [lindex [r config get rdbcompression] 1]
            assert_equal 0 [r dbsize]
        }
    }

    test {RDB files load across compression configuration modes} {
        set compression_cases {
            yes yes VALKEY
            no no VALKEY
            lz4 lzf VCS
            lzf lz4 VALKEY
        }
        if {$::rdbcompression_zstd_supported} {
            lappend compression_cases \
                zstd lz4 VCS \
                lz4 zstd VCS \
                no zstd VALKEY
        }
        foreach {save_mode load_mode expected_header} $compression_cases {
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

    foreach mode $::rdbcompression_modes {
        test "Changing compression config during active $mode BGSAVE does not affect the in-flight save" {
            r config set rdbcompression $mode
            r config set rdb-key-save-delay 10000
            with_cleanup {
                r flushall
                for {set i 0} {$i < 128} {incr i} {
                    r set "bgsave-race:$mode:$i" [string repeat "payload:$i " 128]
                }

                assert_match {*Background saving started*} [r bgsave]
                wait_for_condition 200 10 {
                    [s rdb_bgsave_in_progress] eq 1
                } else {
                    fail "$mode BGSAVE did not start in time"
                }

                # The child must keep the compression setting inherited at fork.
                r config set rdbcompression yes

                wait_for_condition 500 10 {
                    [s rdb_bgsave_in_progress] eq 0
                } else {
                    fail "$mode BGSAVE did not finish in time"
                }
                r config set rdb-key-save-delay 0

                assert_equal "yes" [lindex [r config get rdbcompression] 1]
                assert_rdb_envelope r $mode

                assert_equal "OK" [r save]
                assert_equal "VALKEY" [string range [read_dump_rdb_header_bytes r] 0 5]
            } {
                catch {r config set rdb-key-save-delay 0}
                catch {r config set rdbcompression yes}
            }
        }
    }

    test {Invalid compression config is rejected} {
        set previous [lindex [r config get rdbcompression] 1]
        assert_error {*argument(s) must be one of the following: no, yes, lzf, lz4, zstd*} {
            r config set rdbcompression snappy
        }
        assert_equal $previous [lindex [r config get rdbcompression] 1]
    }

    test {ZSTD compression config is rejected when unsupported} {
        if {$::rdbcompression_zstd_supported} {
            skip "zstd is supported by this build"
        }
        assert_error {*Zstandard compression is not available in this build*} {
            r config set rdbcompression zstd
        }
    }

    foreach mode $::rdbcompression_modes {
        test "Truncated $mode frame is rejected on load even when checksum validation is bypassed" {
            r config set rdbcompression $mode
            r flushall
            set noisy_payload [randstring 4096 4096 alpha]
            for {set i 0} {$i < 32} {incr i} {
                r set "partial:$mode:$i" "${noisy_payload}:$i"
            }

            assert_equal "OK" [r save]
            set rdbfile [dump_rdb_path r]
            assert_rdb_envelope r $mode
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
    }

    foreach mode $::rdbcompression_modes {
        test "[string toupper $mode] compressed RDB detects a content checksum mismatch and allows bypass" {
            r config set rdbcompression $mode
            assert_equal "yes" [lindex [r config get rdbchecksum] 1]
            r flushall
            for {set i 0} {$i < 100} {incr i} {
                r set "$mode-footer:$i" [string repeat "payload$i " 100]
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
                assert_equal [string repeat "payload10 " 100] [r get "$mode-footer:10"]
            } {
                catch {r debug set-skip-checksum-validation 0}
                write_binary_file $rdbfile $original
            }
        }
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

    foreach mode $::rdbcompression_modes {
        test "RDB loader ignores trailing data after a $mode frame like a plain RDB" {
            r config set rdbcompression $mode
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
    }

    foreach mode $::rdbcompression_modes {
        test "$mode compressed RDB detects corruption in its compressed stream" {
            r config set rdbcompression $mode
            r flushall
            for {set i 0} {$i < 100} {incr i} {
                r set "corrupt:$mode:$i" [string repeat "testdata$i " 100]
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

}

start_server {overrides {save "" enable-debug-command local rdbchecksum no}} {
    foreach mode $::rdbcompression_modes {
        test "rdbchecksum controls [string toupper $mode] frame checksums" {
            r config set rdbcompression $mode
            r flushall
            for {set i 0} {$i < 50} {incr i} {
                r set "$mode-nocksum:$i" [string repeat "data$i " 100]
            }

            r save
            assert_rdb_envelope r $mode
            if {$mode eq "zstd"} {
                assert_zstd_rdb_checksum_flag r 0
            } else {
                assert_lz4_rdb_checksum_flags r 0
            }
            set digest [debug_digest]
            set loglines [count_log_lines 0]
            assert_equal "OK" [r debug reload nosave]
            verify_log_message 0 "*Logical RDB CRC64 skipped for streaming-compressed input*" $loglines

            restart_server 0 true false
            assert_equal $digest [debug_digest]
            assert_equal [string repeat "data10 " 100] [r get "$mode-nocksum:10"]
        }
    }
}

start_server {overrides {save "" appendonly yes aof-use-rdb-preamble yes}} {
    foreach mode $::rdbcompression_modes {
        test "AOF rewrite compresses and reloads its RDB base with [string toupper $mode]" {
            r config set rdbcompression $mode
            r flushall
            r set "aof-$mode:key" [string repeat "aof-$mode-value " 100]

            r bgrewriteaof
            waitForBgrewriteaof r

            set base_aof [get_base_aof_path r]
            assert {[file exists $base_aof]}
            assert_rdb_file_envelope $base_aof $mode

            set dir [lindex [r config get dir] 1]
            set appenddirname [lindex [r config get appenddirname] 1]
            set appendfilename [lindex [r config get appendfilename] 1]
            set manifest [file join $dir $appenddirname $appendfilename$::manifest_suffix]
            assert_match "*All AOF files and manifest are valid*" [exec $::VALKEY_CHECK_AOF_BIN $manifest]

            # The decoder must stop at the compressed frame boundary so an
            # old-style AOF can continue with a RESP tail in the same file.
            set old_style_aof [file join $dir "compressed-preamble-$mode.aof"]
            with_cleanup {
                set old_style_data [read_binary_file $base_aof]
                append old_style_data [formatCommand set "aof-$mode:old-style-tail" tail]
                write_binary_file $old_style_aof $old_style_data
                assert_match "*RDB preamble is OK, proceeding with AOF tail*is valid*" \
                    [exec $::VALKEY_CHECK_AOF_BIN $old_style_aof]
            } {
                file delete -force $old_style_aof
            }

            # Keep data in the incremental AOF too, so restart covers both files.
            r set "aof-$mode:incremental" tail
            set digest [debug_digest]

            restart_server 0 true false
            assert_equal $digest [debug_digest]
            assert_equal [string repeat "aof-$mode-value " 100] [r get "aof-$mode:key"]
            assert_equal tail [r get "aof-$mode:incremental"]
        }
    }
}

start_server {tags {"rdb-compression repl external:skip"} overrides {save ""}} {
    set replica [srv 0 client]

    start_server {overrides {save "" enable-debug-command local}} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        test {Full sync remains compatible when rdbcompression is lz4} {
            $primary config set rdbcompression lz4
            $primary config set rdb-del-sync-files no
            $primary flushall
            for {set i 0} {$i < 300} {incr i} {
                $primary set "repl:$i" [string repeat "payload$i " 40]
            }

            $primary config set repl-diskless-sync-delay 0
            $replica config set repl-diskless-load swapdb
            # Keep the replica non-capable so this covers the cohort downgrade.
            $replica config set rdbcompression no

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

            $primary set repl:post-sync "after-sync"
            wait_for_condition 50 100 {
                [$replica get repl:post-sync] eq "after-sync"
            } else {
                fail "Replica did not receive post-sync write"
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

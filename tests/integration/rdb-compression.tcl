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

start_server {overrides {save "" enable-debug-command local}} {
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

    test {Invalid compression config is rejected} {
        set previous [lindex [r config get rdbcompression] 1]
        assert_error "*argument(s) must be one of the following: no, yes, lzf, lz4*" {
            r config set rdbcompression snappy
        }
        assert_equal $previous [lindex [r config get rdbcompression] 1]
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
}

start_server {overrides {save "" appendonly yes aof-use-rdb-preamble yes rdbcompression lz4}} {
    test {AOF rewrite RDB preamble remains plain with LZ4 stream snapshots} {
        r set aof-lz4:key [string repeat "aof-lz4-value " 100]
        set digest [debug_digest]

        r bgrewriteaof
        waitForBgrewriteaof r

        set base_aof [get_base_aof_path r]
        assert {[file exists $base_aof]}
        assert_equal "VALKEY" [string range [read_binary_file_prefix $base_aof 7] 0 5]

        restart_server 0 true false
        assert_equal $digest [debug_digest]
        assert_equal [string repeat "aof-lz4-value " 100] [r get aof-lz4:key]
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

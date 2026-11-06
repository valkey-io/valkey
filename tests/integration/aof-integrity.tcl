source tests/support/aofmanifest.tcl
tags {"aof-integrity external:skip"} {
    
    test "AOF headers are generated when aof-integrity-check is enabled" {
        set sp1 [tmpdir server.aof-integrity-1]
        start_server [list overrides [list dir $sp1 appendonly yes appendfsync always aof-integrity-check yes aof-use-rdb-preamble yes] keep_persistence true] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            set ai1 [file join [dict get [srv config] dir] "appendonlydir" "appendonly.aof.1.incr.aof"]
            $rd set foo bar
            $rd set baz qux
            
            wait_for_condition 50 100 {
                [file exists $ai1] && [file size $ai1] > 0
            } else {
                fail "AOF file not created"
            }
            
            set fp [open $ai1 r]
            fconfigure $fp -translation binary
            set content [read $fp]
            close $fp
            
            assert_match {*#HDR:v1;*checksum:*} $content
            set sp1_actual [dict get [srv config] dir]
        }

        # Restart and verify data restoration
        start_server [list overrides [list dir $sp1_actual appendonly yes aof-integrity-check yes]] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            assert_equal "bar" [$rd get foo]
            assert_equal "qux" [$rd get baz]
        }
    }

    test "Server fails to start if checksum mismatch is detected" {
        set sp3 [tmpdir server.aof-integrity-3]
        start_server [list overrides [list dir $sp3 appendonly yes appendfsync always aof-integrity-check yes aof-use-rdb-preamble yes] keep_persistence true] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            set ::ai3_path [file join [dict get [srv config] dir] "appendonlydir" "appendonly.aof.1.incr.aof"]
            set ::sp3_actual [dict get [srv config] dir]
            $rd set a 1
            wait_for_condition 50 100 {
                [file size $::ai3_path] > 0
            } else {
                fail "AOF file not created"
            }
        }
        
        # Flip a bit in the data to break the checksum
        set fp [open $::ai3_path r]
        fconfigure $fp -translation binary
        set content [read $fp]
        close $fp
        
        set new_content [string map { "\r\na\r\n" "\r\nb\r\n" } $content]
        
        set fp [open $::ai3_path w]
        fconfigure $fp -translation binary
        puts -nonewline $fp $new_content
        close $fp
        
        start_server [list overrides [list dir $::sp3_actual appendonly yes aof-integrity-check yes aof-use-rdb-preamble yes] wait_ready false] {
            wait_for_log_messages 0 {"*AOF checksum mismatch*"} 0 10 1000
            wait_for_condition 50 100 {
                [is_alive [srv pid]] == 0
            } else {
                fail "Server is still running despite AOF checksum mismatch"
            }
        }
        }
    test "AOF integrity: strict header enforcement fails on missing header" {
        set sp [tmpdir server.aof-integrity-strict]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes aof-use-rdb-preamble yes] keep_persistence true] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            set ai_path [file join [dict get [srv config] dir] "appendonlydir" "appendonly.aof.1.incr.aof"]
            set sp_actual [dict get [srv config] dir]
            $rd set a 1
            wait_for_condition 50 100 {
                [file exists $ai_path] && [file size $ai_path] > 0
            } else {
                fail "AOF file not created"
            }
        }

        # Append a command WITHOUT a header
        set fp [open $ai_path a]
        fconfigure $fp -translation binary
        puts -nonewline $fp "*3\r\n\$3\r\nSET\r\n\$1\r\nb\r\n\$1\r\n2\r\n"
        close $fp

        start_server [list overrides [list dir $sp_actual appendonly yes aof-integrity-check yes] wait_ready false] {
            wait_for_log_messages 0 {"*lacks an integrity header*"} 0 10 1000
            wait_for_condition 50 100 {
                [is_alive [srv pid]] == 0
            } else {
                fail "Server is still running despite missing integrity header"
            }
        }
    }

    test "AOF preamble integrity check works even if rdbchecksum is off" {
        set sp_pre [tmpdir server.aof-integrity-preamble]
        start_server [list overrides [list dir $sp_pre appendonly yes appendfsync always aof-integrity-check yes aof-use-rdb-preamble yes rdbchecksum no] keep_persistence true] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            set ::sp_pre_actual [dict get [srv config] dir]
            $rd set x 123
            $rd bgrewriteaof
            waitForBgrewriteaof $rd
            set ::base_path [get_base_aof_path $rd]
        }

        set fp [open $::base_path r]
        fconfigure $fp -translation binary
        set content [read $fp]
        close $fp

        # Flip a bit in the middle of the RDB preamble
        set content "[string range $content 0 10]X[string range $content 12 end]"

        set fp [open $::base_path w]
        fconfigure $fp -translation binary
        puts -nonewline $fp $content
        close $fp

        start_server [list overrides [list dir $::sp_pre_actual appendonly yes aof-integrity-check yes aof-use-rdb-preamble yes rdbchecksum no] wait_ready false] {
            wait_for_log_messages 0 {"*RDB CRC error*"} 0 10 1000
            wait_for_condition 50 100 {
                [is_alive [srv pid]] == 0
            } else {
                fail "Server is still running despite RDB CRC error in AOF preamble"
            }
        }
    }

    test "AOF integrity: large command incremental CRC verification" {
        set sp [tmpdir server.aof-integrity-large]
        # Blocks up to 1 MB are checked in memory, larger ones are read again from the file.
        set mid_val [string repeat "B" [expr 1000 * 1000]]
        set large_val [string repeat "A" [expr 1024 * 1024]]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes aof-use-rdb-preamble yes] keep_persistence true] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            $rd set mid_key $mid_val
            $rd set large_key $large_val
            $rd set small_key small
            set sp_actual [dict get [srv config] dir]
        }
        
        start_server [list overrides [list dir $sp_actual appendonly yes aof-integrity-check yes]] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            assert_equal $mid_val [$rd get mid_key]
            assert_equal $large_val [$rd get large_key]
            assert_equal small [$rd get small_key]
        }
    }

    test "AOF integrity: toggling at runtime applies to the files of a scheduled rewrite" {
        set sp [tmpdir server.aof-integrity-toggle]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            set manifest [file join [lindex [r config get dir] 1] appendonlydir appendonly.aof.manifest]
            r set a 1
            r config set aof-integrity-check no
            wait_for_condition 100 100 {
                [s aof_rewrite_scheduled] == 0 && [s aof_rewrite_in_progress] == 0
            } else {
                fail "AOF rewrite didn't run"
            }
            r set b 2
            assert_no_match {*#HDR:*} [read_binary_file [get_last_incr_aof_path r]]
            assert_no_match {*integrity*} [read_binary_file $manifest]

            r config set aof-integrity-check yes
            wait_for_condition 100 100 {
                [s aof_rewrite_scheduled] == 0 && [s aof_rewrite_in_progress] == 0
            } else {
                fail "AOF rewrite didn't run"
            }
            r set c 3
            assert_match {*#HDR:v1;*} [read_binary_file [get_last_incr_aof_path r]]
            assert_match {*type i integrity 1*# manifest-checksum: *} [read_binary_file $manifest]
        }

        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal 1 [r get a]
            assert_equal 2 [r get b]
            assert_equal 3 [r get c]
        }
    }

    test "AOF integrity: toggling twice before a flush doesn't mix modes in a block" {
        set sp [tmpdir server.aof-integrity-toggle-twice]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            set rd [valkey_deferring_client]
            $rd set a 1
            $rd config set aof-integrity-check no
            $rd set b 2
            $rd config set aof-integrity-check yes
            $rd set c 3
            for {set i 0} {$i < 5} {incr i} {$rd read}
            $rd close
            wait_for_condition 100 100 {
                [s aof_rewrite_scheduled] == 0 && [s aof_rewrite_in_progress] == 0
            } else {
                fail "AOF rewrite didn't run"
            }
        }

        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal 1 [r get a]
            assert_equal 2 [r get b]
            assert_equal 3 [r get c]
        }
    }

    test "aof-integrity-check can be enabled when rdb-preamble is off" {
        set sp8 [tmpdir server.aof-integrity-8]
        start_server [list overrides [list dir $sp8 appendonly yes aof-integrity-check yes aof-use-rdb-preamble no]] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            set config_val [lindex [$rd config get aof-integrity-check] 1]
            assert_equal "yes" $config_val
        }
    }

    test "AOF integrity: checksum persistence through AOF rewrite" {
        set sp [tmpdir server.aof-integrity-lsn-persistence]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes aof-use-rdb-preamble yes] keep_persistence true] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            $rd set a 1
            $rd set b 2
            $rd bgrewriteaof
            waitForBgrewriteaof $rd
            
            $rd set c 3
            
            set incr_path [get_last_incr_aof_path $rd]
            set fp [open $incr_path r]
            set content [read $fp]
            close $fp
            
            # The file should contain integrity headers.
            assert_match {*#HDR:v1;*checksum:*} $content
            set sp_actual [dict get [srv config] dir]
        }

        # Restart and verify data restoration
        start_server [list overrides [list dir $sp_actual appendonly yes aof-integrity-check yes]] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            assert_equal "1" [$rd get a]
            assert_equal "2" [$rd get b]
            assert_equal "3" [$rd get c]
        }
    }

    test "valkey-check-aof detects integrity issues and handles multiple INCR files" {
        set sp6 [tmpdir server.aof-integrity-tool-multi]
        start_server [list overrides [list dir $sp6 appendonly yes appendfsync always aof-integrity-check yes aof-use-rdb-preamble yes] keep_persistence true] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            set ::am6_path [file join [dict get [srv config] dir] "appendonlydir" "appendonly.aof.manifest"]
            $rd set a 1
            
            # Trigger rewrite to create a new INCR file
            $rd bgrewriteaof
            waitForBgrewriteaof $rd
            
            $rd set b 2
            set ::ai6_path [get_last_incr_aof_path $rd]
        }
        
        catch {exec $::VALKEY_CHECK_AOF_BIN $::am6_path} output
        assert_match {*All AOF files and manifest are valid*} $output
        
        # Corrupt the NEWEST increment file by appending a manual entry with a wrong checksum.
        set fp [open $::ai6_path a]
        fconfigure $fp -translation binary
        # We use a wrong checksum value here to simulate an integrity failure.
        # Note the ';' before '\r\n' to ensure the header is well-formed but has a bad checksum.
        puts -nonewline $fp "#HDR:v1;len:22;checksum:123456789;\r\n*3\r\n\$3\r\nSET\r\n\$1\r\nc\r\n\$1\r\n3\r\n"
        close $fp
        
        catch {exec $::VALKEY_CHECK_AOF_BIN $::am6_path} output
        assert_match {*AOF checksum mismatch*} $output
        
        # Test case for missing header
        set sp7 [tmpdir server.aof-integrity-tool-missing-hdr]
        start_server [list overrides [list dir $sp7 appendonly yes appendfsync always aof-integrity-check yes aof-use-rdb-preamble yes] keep_persistence true] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            set am_path [file join [dict get [srv config] dir] "appendonlydir" "appendonly.aof.manifest"]
            $rd set a 1
            set incr_path [get_last_incr_aof_path $rd]
        }
        
        # Append a raw RESP command without #HDR
        set fp [open $incr_path a]
        fconfigure $fp -translation binary
        puts -nonewline $fp "*3\r\n\$3\r\nSET\r\n\$1\r\nb\r\n\$1\r\n2\r\n"
        close $fp
        
        catch {exec $::VALKEY_CHECK_AOF_BIN $am_path} output
        assert_match {*lacks an integrity header*} $output
    }

    test "AOF manifest contains checksums when integrity check is enabled" {
        set sp [tmpdir server.aof-integrity-manifest-checksum]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes aof-use-rdb-preamble yes] keep_persistence true] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            $rd set x 1
            $rd bgrewriteaof
            waitForBgrewriteaof $rd
            $rd set y 2
            
            set manifest_path [file join [dict get [srv config] dir] "appendonlydir" "appendonly.aof.manifest"]
            set fp [open $manifest_path r]
            set content [read $fp]
            close $fp
            
            assert {[string match {*# manifest-checksum: *} $content]}
            assert {[string match {*type b integrity 1 checksum *} $content]}
            assert {[string match {*type i integrity 1*} $content]}
        }
    }

    test "AOF manifest does not contain checksums when integrity check is disabled" {
        set sp [tmpdir server.aof-integrity-manifest-no-checksum]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check no aof-use-rdb-preamble yes] keep_persistence true] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            $rd set x 1
            $rd bgrewriteaof
            waitForBgrewriteaof $rd
            $rd set y 2
            
            set manifest_path [file join [dict get [srv config] dir] "appendonlydir" "appendonly.aof.manifest"]
            set fp [open $manifest_path r]
            set content [read $fp]
            close $fp
            
            assert {! [string match {*# manifest-checksum: *} $content]}
            assert {! [string match {*checksum *} $content]}
            assert {! [string match {*integrity*} $content]}
        }
    }

    test "AOF integrity: test helper builds blocks the server accepts" {
        assert_equal 16845390139448941002 [crc64 0 "123456789"]
        set sp [tmpdir server.aof-integrity-helper]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            set incr [get_last_incr_aof_path r]
        }
        set payload "*3\r\n\$3\r\nSET\r\n\$2\r\nk2\r\n\$2\r\nv2\r\n"
        append_binary_file $incr [aof_integrity_block [aof_last_block_checksum $incr] $payload]
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
            assert_equal v2 [r get k2]
        }
    }

    test "AOF integrity: a torn header at the end of the file is truncated with aof-load-truncated" {
        foreach torn [list "#H" "#HDR:v1;" "#HDR:v1;len:45" "#HDR:v1;len:45;checksum:" "#HDR:v1;len:45;checksum:123;\r"] {
            set sp [tmpdir server.aof-integrity-torn-header]
            start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
                r set k1 v1
                set incr [get_last_incr_aof_path r]
            }
            append_binary_file $incr $torn
            start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated yes] keep_persistence true] {
                assert_equal v1 [r get k1]
                verify_log_message 0 "*Truncating the AOF*" 0
                r set k2 v2
            }
            # The torn bytes are gone, so the next append continues a valid chain.
            start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
                assert_equal v1 [r get k1]
                assert_equal v2 [r get k2]
            }
        }
    }

    test "AOF integrity: a torn header at the end of the file fails to load without aof-load-truncated" {
        set sp [tmpdir server.aof-integrity-torn-header-strict]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            set incr [get_last_incr_aof_path r]
        }
        append_binary_file $incr "#HDR:v1;len:45"
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true wait_ready false] {
            wait_for_log_messages 0 {"*Unexpected end of file reading the append only file*"} 0 10 1000
            wait_for_condition 50 100 {
                [is_alive [srv pid]] == 0
            } else {
                fail "Server is still running despite a torn header"
            }
        }
    }

    test "AOF: a torn annotation at the end of the file is truncated with aof-load-truncated" {
        set sp [tmpdir server.aof-torn-annotation]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check no] keep_persistence true] {
            r set k1 v1
            set incr [get_last_incr_aof_path r]
        }
        append_binary_file $incr "#TS:17"
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check no aof-load-truncated yes] keep_persistence true] {
            assert_equal v1 [r get k1]
            r set k2 v2
        }
        # The torn line was removed, so the write after it starts on a new line and the
        # file loads without aof-load-truncated.
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check no aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
            assert_equal v2 [r get k2]
        }
    }

    test "AOF integrity: a torn block at the end of the file is truncated and the chain continues" {
        set sp [tmpdir server.aof-integrity-torn-block]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            r set k2 v2
            set incr [get_last_incr_aof_path r]
        }
        # Cut the end of the last block (SET k2 v2).
        set content [read_binary_file $incr]
        write_binary_file $incr [string range $content 0 end-5]
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated yes] keep_persistence true] {
            assert_equal v1 [r get k1]
            assert_equal {} [r get k2]
            r set k3 v3
        }
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
            assert_equal v3 [r get k3]
        }
    }

    test "AOF integrity: an incomplete transaction inside a verified block is a format error" {
        set sp [tmpdir server.aof-integrity-incomplete-multi]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            set incr [get_last_incr_aof_path r]
        }
        set payload "*1\r\n\$5\r\nMULTI\r\n*3\r\n\$3\r\nSET\r\n\$2\r\nk2\r\n\$2\r\nv2\r\n"
        append_binary_file $incr [aof_integrity_block [aof_last_block_checksum $incr] $payload]
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated yes] keep_persistence true wait_ready false] {
            wait_for_log_messages 0 {"*Incomplete data inside a verified integrity block*"} 0 10 1000
            wait_for_condition 50 100 {
                [is_alive [srv pid]] == 0
            } else {
                fail "Server is still running despite an incomplete transaction"
            }
        }
    }

    test "AOF integrity: a command extending past its block is a format error" {
        # The block's len ends inside the command, so the rest of it isn't covered: inside
        # its first line, or inside an argument.
        foreach len {1 10} {
            set sp [tmpdir server.aof-integrity-command-past-block]
            start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
                r set k1 v1
                set incr [get_last_incr_aof_path r]
            }
            set payload "*3\r\n\$3\r\nSET\r\n\$2\r\nk2\r\n\$2\r\nv2\r\n"
            append_binary_file $incr [aof_integrity_block [aof_last_block_checksum $incr] $payload $len]
            start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes] keep_persistence true wait_ready false] {
                wait_for_log_messages 0 {"*extends past the end of its integrity block*"} 0 10 1000
                wait_for_condition 50 100 {
                    [is_alive [srv pid]] == 0
                } else {
                    fail "Server is still running despite a command outside its block"
                }
            }
        }
    }

    test "AOF integrity: the file is never truncated inside a verified block" {
        set sp [tmpdir server.aof-integrity-truncate-inside-block]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            set incr [get_last_incr_aof_path r]
        }
        # A verified block holding SET k2 and the start of SET k3, with the file ending
        # inside SET k3. SET k2 runs before the end of the file is reached.
        set k2 "*3\r\n\$3\r\nSET\r\n\$2\r\nk2\r\n\$2\r\nv2\r\n"
        set k3 "*3\r\n\$3\r\nSET\r\n\$2\r\nk3\r\n\$2\r\nv3\r\n"
        set payload "$k2[string range $k3 0 19]"
        set len [expr {[string length $k2] + 10}]
        append_binary_file $incr [aof_integrity_block [aof_last_block_checksum $incr] $payload $len]
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated yes] keep_persistence true wait_ready false] {
            wait_for_log_messages 0 {"*Incomplete data inside a verified integrity block*"} 0 10 1000
            wait_for_condition 50 100 {
                [is_alive [srv pid]] == 0
            } else {
                fail "Server is still running after truncating inside a verified block"
            }
        }
    }

    test "AOF integrity: unknown header fields, anywhere in the header, are ignored by the loader and valkey-check-aof" {
        set sp [tmpdir server.aof-integrity-unknown-field]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            set incr [get_last_incr_aof_path r]
            set manifest [file join [lindex [r config get dir] 1] appendonlydir appendonly.aof.manifest]
        }
        set payload "*3\r\n\$3\r\nSET\r\n\$2\r\nk2\r\n\$2\r\nv2\r\n"
        append_binary_file $incr [aof_integrity_block [aof_last_block_checksum $incr] $payload {} "foo:bar;" "baz:qux;"]
        catch {exec $::VALKEY_CHECK_AOF_BIN $manifest} output
        assert_match {*All AOF files and manifest are valid*} $output
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal v2 [r get k2]
        }
    }

    test "AOF integrity: malformed headers are format errors" {
        foreach {hdr err} [list "#HDR:v1;len:abc;checksum:1;\r\n" "*invalid len*" \
                                "#HDR:v2;len:4;checksum:1;\r\n" "*unsupported header version*"] {
            set sp [tmpdir server.aof-integrity-malformed-header]
            start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
                r set k1 v1
                set incr [get_last_incr_aof_path r]
            }
            append_binary_file $incr "$hdr*1\r\n"
            start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated yes] keep_persistence true wait_ready false] {
                wait_for_log_messages 0 [list "*Malformed AOF integrity header*$err"] 0 10 1000
                wait_for_condition 50 100 {
                    [is_alive [srv pid]] == 0
                } else {
                    fail "Server is still running despite a malformed header"
                }
            }
        }
    }

    test "AOF integrity: a block longer than the rest of the file is truncated" {
        set sp [tmpdir server.aof-integrity-len-past-eof]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            set incr [get_last_incr_aof_path r]
        }
        append_binary_file $incr "#HDR:v1;len:1000000;checksum:1;\r\n*3\r\n"
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated yes] keep_persistence true] {
            assert_equal v1 [r get k1]
            verify_log_message 0 "*extends past the end of the file*" 0
            r set k2 v2
        }
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
            assert_equal v2 [r get k2]
        }
    }

    test "valkey-check-aof --fix truncates a torn integrity header" {
        set sp [tmpdir server.aof-integrity-fix-torn]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            set incr [get_last_incr_aof_path r]
            set manifest [file join [lindex [r config get dir] 1] appendonlydir appendonly.aof.manifest]
        }
        set size [file size $incr]
        append_binary_file $incr "#HDR:v1;len:45"
        catch {exec $::VALKEY_CHECK_AOF_BIN $manifest} output
        assert_match {*Torn annotation at the end of the file*} $output
        exec $::VALKEY_CHECK_AOF_BIN --fix $manifest << "y\n"
        assert_equal $size [file size $incr]
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
        }
    }

    test "valkey-check-aof --fix truncates a corrupt integrity block" {
        set sp [tmpdir server.aof-integrity-fix-corrupt]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            set incr [get_last_incr_aof_path r]
            set manifest [file join [lindex [r config get dir] 1] appendonlydir appendonly.aof.manifest]
            set size [file size $incr]
            r set k2 v2
        }
        # Change "v2" to "x2" in the last block.
        set content [read_binary_file $incr]
        write_binary_file $incr [string replace $content end-3 end-3 "x"]
        exec $::VALKEY_CHECK_AOF_BIN --fix $manifest << "y\n"
        assert_equal $size [file size $incr]
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
            assert_equal {} [r get k2]
        }
    }

    test "valkey-check-aof --fix truncates a block whose command extends past it" {
        set sp [tmpdir server.aof-integrity-fix-past-block]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            set incr [get_last_incr_aof_path r]
            set manifest [file join [lindex [r config get dir] 1] appendonlydir appendonly.aof.manifest]
        }
        set size [file size $incr]
        set payload "*3\r\n\$3\r\nSET\r\n\$2\r\nk2\r\n\$2\r\nv2\r\n"
        append_binary_file $incr [aof_integrity_block [aof_last_block_checksum $incr] $payload 10]
        catch {exec $::VALKEY_CHECK_AOF_BIN $manifest} output
        assert_match {*extends past the end of its integrity block*} $output
        exec $::VALKEY_CHECK_AOF_BIN --fix $manifest << "y\n"
        assert_equal $size [file size $incr]
    }

    test "valkey-check-aof --truncate-to-timestamp truncates whole integrity blocks" {
        set sp [tmpdir server.aof-integrity-truncate-ts]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes aof-timestamp-enabled yes] keep_persistence true] {
            r set k1 v1
            after 1100
            r set k2 v2
            after 1100
            r set k3 v3
            set incr [get_last_incr_aof_path r]
            set manifest [file join [lindex [r config get dir] 1] appendonlydir appendonly.aof.manifest]
        }
        # Each block starts with #HDR followed by the #TS of its second.
        set ts2 [lindex [regexp -all -inline {#TS:([0-9]+)\r\n} [read_binary_file $incr]] 3]
        exec $::VALKEY_CHECK_AOF_BIN --truncate-to-timestamp [expr {$ts2 - 1}] $manifest
        catch {exec $::VALKEY_CHECK_AOF_BIN $manifest} output
        assert_match {*All AOF files and manifest are valid*} $output
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
            assert_equal {} [r get k2]
            assert_equal {} [r get k3]
        }
    }

    test "AOF integrity: restarting with integrity disabled continues the last file without it" {
        set sp [tmpdir server.aof-integrity-restart-disabled]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            r set k2 v2
            set incr [get_last_incr_aof_path r]
            set manifest [file join [lindex [r config get dir] 1] appendonlydir appendonly.aof.manifest]
        }
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check no] keep_persistence true] {
            assert_equal v2 [r get k2]
            verify_log_message 0 "*no longer treated as an integrity file*" 0
            # Writes continue in the same file, without headers, and nothing is rewritten.
            assert_equal 0 [s aof_rewrite_scheduled]
            assert_equal $incr [get_last_incr_aof_path r]
            set size [file size $incr]
            r set k3 v3
            assert_no_match "*#HDR:*" [string range [read_binary_file $incr] $size end]
        }
        # The base keeps its marker.
        assert_match "*type b integrity 1*file [file tail $incr] seq 1 type i\n*" [read_binary_file $manifest]
        # Enabling it again starts a new file with integrity, and rewrites the AOF.
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            wait_for_condition 100 100 {
                [s aof_rewrite_scheduled] == 0 && [s aof_rewrite_in_progress] == 0
            } else {
                fail "AOF rewrite didn't run"
            }
            assert_equal ok [s aof_last_bgrewrite_status]
            r set k4 v4
        }
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal v2 [r get k2]
            assert_equal v3 [r get k3]
            assert_equal v4 [r get k4]
        }
    }

    test "AOF integrity: restarting with integrity enabled starts a new file and rewrites the AOF" {
        set sp [tmpdir server.aof-integrity-restart-enabled]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check no] keep_persistence true] {
            r set k1 v1
        }
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            wait_for_condition 100 100 {
                [s aof_rewrite_scheduled] == 0 && [s aof_rewrite_in_progress] == 0
            } else {
                fail "AOF rewrite didn't run"
            }
            r set k2 v2
            assert_match {*#HDR:v1;*} [read_binary_file [get_last_incr_aof_path r]]
        }
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
            assert_equal v2 [r get k2]
        }
    }

    test "AOF integrity: an integrity file needs headers from its first byte" {
        set sp [tmpdir server.aof-integrity-stripped]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            set incr [get_last_incr_aof_path r]
        }
        write_binary_file $incr [regsub -all {#HDR:[^\r]*\r\n} [read_binary_file $incr] {}]
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes] keep_persistence true wait_ready false] {
            wait_for_log_messages 0 {"*lacks an integrity header*"} 0 10 1000
            wait_for_log_messages 0 {"*set aof-integrity-check to no*"} 0 10 1000
            wait_for_condition 50 100 {
                [is_alive [srv pid]] == 0
            } else {
                fail "Server is still running despite missing integrity headers"
            }
        }
        # With integrity disabled, the file is loaded without verification.
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check no] keep_persistence true] {
            assert_equal v1 [r get k1]
        }
    }

    test "AOF integrity: a closed file must end with the checksum recorded for it" {
        set sp [tmpdir server.aof-integrity-closed-checksum]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes rdb-key-save-delay 10000000] keep_persistence true] {
            r set k1 v1
            r set k2 v2
            set incr [get_last_incr_aof_path r]
            set manifest [file join [lindex [r config get dir] 1] appendonlydir appendonly.aof.manifest]
            # A failed rewrite leaves the INCR file closed, with its final checksum recorded.
            r bgrewriteaof
            catch {exec kill -9 [get_child_pid 0]}
            wait_for_condition 50 100 {
                [s aof_rewrite_in_progress] == 0
            } else {
                fail "AOF rewrite didn't stop"
            }
        }
        # Drop the last block (SET k2) of the closed file.
        set content [read_binary_file $incr]
        write_binary_file $incr [string range $content 0 [expr {[string last "#HDR:" $content] - 1}]]
        catch {exec $::VALKEY_CHECK_AOF_BIN $manifest} output
        assert_match {*ends with checksum*but the manifest records*} $output
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes] keep_persistence true wait_ready false] {
            wait_for_log_messages 0 {"*ends with checksum*but the manifest records*"} 0 10 1000
            wait_for_condition 50 100 {
                [is_alive [srv pid]] == 0
            } else {
                fail "Server is still running despite a shortened closed file"
            }
        }
    }

    test "AOF integrity: disabling during a rewrite keeps the AOF loadable" {
        set sp [tmpdir server.aof-integrity-toggle-during-rewrite]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes rdb-key-save-delay 500000] keep_persistence true] {
            r set k1 v1
            r bgrewriteaof
            r config set aof-integrity-check no
            r set k2 v2
            wait_for_condition 100 100 {
                [s aof_rewrite_scheduled] == 0 && [s aof_rewrite_in_progress] == 0
            } else {
                fail "AOF rewrites didn't run"
            }
            r set k3 v3
        }
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
            assert_equal v2 [r get k2]
            assert_equal v3 [r get k3]
        }
    }

    test "AOF integrity: the manifest checksum is verified, also by valkey-check-aof" {
        set sp [tmpdir server.aof-integrity-manifest-corrupt]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            set manifest [file join [lindex [r config get dir] 1] appendonlydir appendonly.aof.manifest]
        }
        # A trailing space doesn't change how the line is parsed, only its checksum.
        set content [read_binary_file $manifest]
        write_binary_file $manifest [regsub {\n} $content " \n"]
        catch {exec $::VALKEY_CHECK_AOF_BIN $manifest} output
        assert_match {*checksum mismatch*valkey-check-aof --fix*} $output
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes] keep_persistence true wait_ready false] {
            wait_for_log_messages 0 {"*AOF manifest checksum mismatch*"} 0 10 1000
            wait_for_condition 50 100 {
                [is_alive [srv pid]] == 0
            } else {
                fail "Server is still running despite a corrupt manifest"
            }
        }
        # A manifest edited on purpose can get its checksum updated.
        set output [exec $::VALKEY_CHECK_AOF_BIN --fix $manifest << "y\n"]
        assert_match {*Updated the AOF manifest checksum*All AOF files and manifest are valid*} $output
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes] keep_persistence true] {
            assert_equal v1 [r get k1]
        }
    }

    test "AOF integrity: the manifest checksum line allows whitespace, and nothing may follow it" {
        set sp [tmpdir server.aof-integrity-manifest-line]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            set manifest [file join [lindex [r config get dir] 1] appendonlydir appendonly.aof.manifest]
        }
        set content [read_binary_file $manifest]
        write_binary_file $manifest [string map {"# manifest-checksum: " "#  Manifest-Checksum :  "} $content]
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
        }
        append_binary_file $manifest "file appendonly.aof.9.incr.aof seq 9 type i\n"
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes] keep_persistence true wait_ready false] {
            wait_for_log_messages 0 {"*Unexpected content after the AOF manifest checksum*"} 0 10 1000
            wait_for_condition 50 100 {
                [is_alive [srv pid]] == 0
            } else {
                fail "Server is still running despite content after the manifest checksum"
            }
        }
    }

    test "AOF integrity: a RESP base ends with its CRC, which is verified" {
        set sp [tmpdir server.aof-integrity-resp-base]
        set overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes aof-use-rdb-preamble no]
        start_server [list overrides $overrides keep_persistence true] {
            r set k1 v1
            r bgrewriteaof
            waitForBgrewriteaof r
            set base [get_base_aof_path r]
            set manifest [file join [lindex [r config get dir] 1] appendonlydir appendonly.aof.manifest]
            assert_match "*#CRC:*\r\n" [read_binary_file $base]
            assert_match {*type b integrity 1 checksum *} [read_binary_file $manifest]
        }
        start_server [list overrides [concat $overrides aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
        }
        catch {exec $::VALKEY_CHECK_AOF_BIN $manifest} output
        assert_match {*All AOF files and manifest are valid*} $output

        # A changed byte fails the CRC.
        set orig [read_binary_file $base]
        set idx [string first "v1" $orig]
        write_binary_file $base [string replace $orig $idx $idx "x"]
        catch {exec $::VALKEY_CHECK_AOF_BIN $manifest} output
        assert_match {*checksum mismatch*} $output
        start_server [list overrides $overrides keep_persistence true wait_ready false] {
            wait_for_log_messages 0 {"*AOF base file*checksum mismatch*"} 0 10 1000
            wait_for_condition 50 100 {
                [is_alive [srv pid]] == 0
            } else {
                fail "Server is still running despite a corrupt base"
            }
        }

        # So does a missing CRC.
        write_binary_file $base [regsub {#CRC:[0-9]+\r\n$} $orig {}]
        catch {exec $::VALKEY_CHECK_AOF_BIN $manifest} output
        assert_match {*lacks its checksum*} $output
        start_server [list overrides $overrides keep_persistence true wait_ready false] {
            wait_for_log_messages 0 {"*AOF base file*lacks its checksum*"} 0 10 1000
            wait_for_condition 50 100 {
                [is_alive [srv pid]] == 0
            } else {
                fail "Server is still running despite a base without its CRC"
            }
        }
    }

    test "AOF integrity: an RDB base loads with rdbchecksum disabled" {
        set sp [tmpdir server.aof-integrity-rdb-base-no-rdbchecksum]
        set overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes rdbchecksum no]
        start_server [list overrides $overrides keep_persistence true] {
            r set k1 v1
            r bgrewriteaof
            waitForBgrewriteaof r
        }
        start_server [list overrides [concat $overrides aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
        }

        set sp_lz4 [tmpdir server.aof-integrity-rdb-base-lz4-no-rdbchecksum]
        set overrides_lz4 [concat $overrides [list dir $sp_lz4 rdbcompression lz4]]
        set val [string repeat "lz4-value-" 100]
        start_server [list overrides $overrides_lz4 keep_persistence true] {
            r set k1 $val
            r bgrewriteaof
            waitForBgrewriteaof r
            set ::lz4_base_path [get_base_aof_path r]
        }
        start_server [list overrides [concat $overrides_lz4 aof-load-truncated no] keep_persistence true] {
            assert_equal $val [r get k1]
        }

        # Corrupt the compressed base's codec checksum and verify loading fails.
        set raw [read_binary_file $::lz4_base_path]
        binary scan [string index $raw end] cu last_byte
        set corrupted "[string range $raw 0 end-1][binary format c [expr {$last_byte ^ 1}]]"
        write_binary_file $::lz4_base_path $corrupted
        start_server [list overrides $overrides_lz4 keep_persistence true wait_ready false] {
            wait_for_log_messages 0 {"*Corrupt streaming-compressed RDB input*"} 0 10 1000
            wait_for_condition 50 100 {
                [is_alive [srv pid]] == 0
            } else {
                fail "Server is still running despite a corrupted compressed RDB base"
            }
        }
    }

    foreach mode {disabled swapdb} {
        test "AOF integrity: a replica's AOF stays verifiable after a full sync (repl-diskless-load $mode)" {
            set sp [tmpdir server.aof-integrity-repl-$mode]
            start_server {overrides {save ""}} {
                set primary [srv 0 client]
                set primary_host [srv 0 host]
                set primary_port [srv 0 port]
                $primary set k1 v1
                set overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes \
                                   aof-use-rdb-preamble yes repl-diskless-load $mode]
                start_server [list overrides $overrides keep_persistence true] {
                    set replica [srv 0 client]
                    # Writes before the sync advance the replica's chain, which the sync restarts.
                    $replica set local x
                    $replica replicaof $primary_host $primary_port
                    wait_for_sync $replica
                    waitForBgrewriteaof $replica
                    $primary set k2 v2
                    wait_for_ofs_sync $primary $replica
                    set dir [lindex [$replica config get dir] 1]
                }
                assert_match {*All AOF files and manifest are valid*} \
                    [exec $::VALKEY_CHECK_AOF_BIN [file join $dir appendonlydir appendonly.aof.manifest]]
                start_server [list overrides [list dir $dir appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
                    assert_equal v1 [r get k1]
                    assert_equal v2 [r get k2]
                    assert_equal {} [r get local]
                }
            }
        }
    }

    test "AOF integrity: enabling appendonly at runtime writes integrity files" {
        set sp [tmpdir server.aof-integrity-enable-aof]
        start_server [list overrides [list dir $sp appendonly no appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            r config set appendonly yes
            waitForBgrewriteaof r
            r set k2 v2
            set dir [lindex [r config get dir] 1]
        }
        set manifest [file join $dir appendonlydir appendonly.aof.manifest]
        assert_match {*type b integrity 1*type i integrity 1*} [read_binary_file $manifest]
        assert_match {*All AOF files and manifest are valid*} [exec $::VALKEY_CHECK_AOF_BIN $manifest]
        start_server [list overrides [list dir $dir appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
            assert_equal v2 [r get k2]
        }
    }

    test "AOF integrity: the chain continues across a failed rewrite and a successful one" {
        set sp [tmpdir server.aof-integrity-failed-rewrite]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes rdb-key-save-delay 10000000] keep_persistence true] {
            r set k1 v1
            r bgrewriteaof
            catch {exec kill -9 [get_child_pid 0]}
            waitForBgrewriteaof r
            r set k2 v2
            r config set rdb-key-save-delay 0
            r bgrewriteaof
            waitForBgrewriteaof r
            assert_equal ok [s aof_last_bgrewrite_status]
            r set k3 v3
            set dir [lindex [r config get dir] 1]
        }
        assert_match {*All AOF files and manifest are valid*} \
            [exec $::VALKEY_CHECK_AOF_BIN [file join $dir appendonlydir appendonly.aof.manifest]]
        start_server [list overrides [list dir $dir appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
            assert_equal v2 [r get k2]
            assert_equal v3 [r get k3]
        }
    }

    test "AOF integrity: the last file loaded without verification is no longer an integrity file" {
        set payload "*3\r\n\$3\r\nSET\r\n\$2\r\nk2\r\n\$2\r\nv2\r\n"
        # The file ends with a block cut in the middle of its command, or with a header
        # whose block never reached the disk.
        foreach tail {cut header} {
            set sp [tmpdir server.aof-integrity-unverified-$tail]
            start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
                r set k1 v1
                set incr [get_last_incr_aof_path r]
                set dir [lindex [r config get dir] 1]
            }
            set block [aof_integrity_block [aof_last_block_checksum $incr] $payload]
            if {$tail eq "cut"} {
                append_binary_file $incr [string range $block 0 end-5]
            } else {
                append_binary_file $incr [string range $block 0 [expr {[string first "\r\n" $block] + 1}]]
            }
            start_server [list overrides [list dir $dir appendonly yes appendfsync always aof-integrity-check no aof-load-truncated yes] keep_persistence true] {
                assert_equal v1 [r get k1]
                assert_equal {} [r get k2]
                verify_log_message 0 "*no longer treated as an integrity file*" 0
                r set k3 v3
            }
            set manifest [read_binary_file [file join $dir appendonlydir appendonly.aof.manifest]]
            assert_match "*file [file tail $incr] seq 1 type i\n*" $manifest
            start_server [list overrides [list dir $dir appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
                assert_equal v1 [r get k1]
                assert_equal v3 [r get k3]
            }
        }
    }

    test "AOF integrity: enabling it with only a base, after upgrading an old AOF, rewrites the AOF" {
        set sp [tmpdir server.aof-integrity-upgrade]
        set fp [open [file join $sp appendonly.aof] w]
        fconfigure $fp -translation binary
        puts -nonewline $fp [formatCommand select [expr {$::singledb ? 0 : 9}]][formatCommand set k1 v1]
        close $fp
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            wait_for_condition 100 100 {
                [s aof_rewrite_scheduled] == 0 && [s aof_rewrite_in_progress] == 0
            } else {
                fail "AOF rewrite didn't run"
            }
            assert_equal ok [s aof_last_bgrewrite_status]
            assert_equal v1 [r get k1]
            set dir [lindex [r config get dir] 1]
        }
        assert_match {*type b integrity 1*} [read_binary_file [file join $dir appendonlydir appendonly.aof.manifest]]
        start_server [list overrides [list dir $dir appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
        }
    }

    test "valkey-check-aof --truncate-to-timestamp reports the commands it truncates before the timestamp" {
        set sp [tmpdir server.aof-integrity-truncate-ts-mid-block]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            set incr [get_last_incr_aof_path r]
            set manifest [file join [lindex [r config get dir] 1] appendonlydir appendonly.aof.manifest]
        }
        set size [file size $incr]
        # A block with a command before the timestamp, which is truncated with the block.
        set before [formatCommand set k2 v2]
        set payload "${before}#TS:1700000000\r\n[formatCommand set k3 v3]"
        append_binary_file $incr [aof_integrity_block [aof_last_block_checksum $incr] $payload]
        set output [exec $::VALKEY_CHECK_AOF_BIN --truncate-to-timestamp 1600000000 $manifest]
        assert_match "*the [string length $before] bytes of the block before it were truncated too*" $output
        assert_equal $size [file size $incr]
    }

    test "AOF integrity: DEBUG LOADAOF keeps the chain" {
        set sp [tmpdir server.aof-integrity-debug-loadaof]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            r debug loadaof
            r set k2 v2
            set dir [lindex [r config get dir] 1]
        }
        assert_match {*All AOF files and manifest are valid*} \
            [exec $::VALKEY_CHECK_AOF_BIN [file join $dir appendonlydir appendonly.aof.manifest]]
        start_server [list overrides [list dir $dir appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
            assert_equal v2 [r get k2]
        }
    }

    test "AOF integrity: DEBUG LOADAOF before a runtime disable is applied keeps the chain" {
        set sp [tmpdir server.aof-integrity-debug-loadaof-disable]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            # A BGSAVE holds back the rewrite that applies the change, so the INCR file
            # keeps getting headers after DEBUG LOADAOF.
            r config set rdb-key-save-delay 10000000
            r bgsave
            wait_for_condition 50 100 {
                [s rdb_bgsave_in_progress] == 1
            } else {
                fail "BGSAVE didn't start"
            }
            r config set aof-integrity-check no
            r debug loadaof
            r set k2 v2
            assert_equal 1 [s aof_rewrite_scheduled]
            set dir [lindex [r config get dir] 1]
        }
        start_server [list overrides [list dir $dir appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
            assert_equal v2 [r get k2]
        }
    }

    test "AOF integrity: the chain restarts after a file that lost its integrity marker" {
        set sp [tmpdir server.aof-integrity-chain-restart]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            r set k1 v1
            r config set rdb-key-save-delay 10000000
            r bgsave
            wait_for_condition 50 100 {
                [s rdb_bgsave_in_progress] == 1
            } else {
                fail "BGSAVE didn't start"
            }
            # Loading without verification removes the marker of the INCR file. Enabling
            # integrity again before the rewrite runs doesn't change the writer's setting.
            r config set aof-integrity-check no
            r debug loadaof
            r config set aof-integrity-check yes
            r set k2 v2
            # The scheduled rewrite starts once the BGSAVE is gone, and then fails.
            catch {exec kill -9 [get_child_pid 0]}
            wait_for_condition 100 100 {
                [s aof_rewrite_in_progress] == 1
            } else {
                fail "AOF rewrite didn't start"
            }
            r set k3 v3
            catch {exec kill -9 [get_child_pid 0]}
            wait_for_condition 100 100 {
                [s aof_rewrite_in_progress] == 0
            } else {
                fail "AOF rewrite didn't stop"
            }
            set dir [lindex [r config get dir] 1]
        }
        assert_match {*All AOF files and manifest are valid*} \
            [exec $::VALKEY_CHECK_AOF_BIN [file join $dir appendonlydir appendonly.aof.manifest]]
        start_server [list overrides [list dir $dir appendonly yes aof-integrity-check yes aof-load-truncated no] keep_persistence true] {
            assert_equal v1 [r get k1]
            assert_equal v2 [r get k2]
            assert_equal v3 [r get k3]
        }
    }

    test "AOF integrity: a restart rewrites the AOF while any file lacks integrity" {
        set sp [tmpdir server.aof-integrity-failed-enable]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check no rdb-key-save-delay 10000000] keep_persistence true] {
            r set k1 v1
            # The rewrite that applies the change fails, so the first INCR file stays without integrity.
            r config set aof-integrity-check yes
            wait_for_condition 100 100 {
                [s aof_rewrite_in_progress] == 1
            } else {
                fail "AOF rewrite didn't start"
            }
            catch {exec kill -9 [get_child_pid 0]}
            wait_for_condition 100 100 {
                [s aof_rewrite_in_progress] == 0
            } else {
                fail "AOF rewrite didn't stop"
            }
            r set k2 v2
            set manifest [file join [lindex [r config get dir] 1] appendonlydir appendonly.aof.manifest]
        }
        assert_match "*seq 1 type i\n*seq 2 type i integrity 1*" [read_binary_file $manifest]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes] keep_persistence true] {
            wait_for_condition 100 100 {
                [s aof_rewrite_scheduled] == 0 && [s aof_rewrite_in_progress] == 0
            } else {
                fail "AOF rewrite didn't run"
            }
            assert_equal ok [s aof_last_bgrewrite_status]
            assert_equal v1 [r get k1]
            assert_equal v2 [r get k2]
        }
        foreach line [split [read_binary_file $manifest] "\n"] {
            if {[string match "file *" $line]} {
                assert_match "*integrity 1*" $line
            }
        }
    }

    test "AOF integrity: a verified file that isn't the last one is never truncated" {
        set sp [tmpdir server.aof-integrity-closed-not-truncated]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes rdb-key-save-delay 10000000] keep_persistence true] {
            r set k1 v1
            r set k2 v2
            set incr [get_last_incr_aof_path r]
            # A failed rewrite closes the INCR file.
            r bgrewriteaof
            catch {exec kill -9 [get_child_pid 0]}
            wait_for_condition 50 100 {
                [s aof_rewrite_in_progress] == 0
            } else {
                fail "AOF rewrite didn't stop"
            }
        }
        # A corrupted len makes the last block of the closed file look longer than the file.
        set content [read_binary_file $incr]
        set pos [expr {[string first "len:" $content [string last "#HDR:" $content]] + 4}]
        write_binary_file $incr [string replace $content $pos $pos 9]
        set size [file size $incr]
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes aof-load-truncated yes] keep_persistence true wait_ready false] {
            wait_for_log_messages 0 {"*isn't the last AOF file*"} 0 10 1000
            wait_for_condition 50 100 {
                [is_alive [srv pid]] == 0
            } else {
                fail "Server is still running despite a corrupted closed file"
            }
        }
        assert_equal $size [file size $incr]
    }

    test "AOF integrity: an empty base written with integrity is a format error" {
        set sp [tmpdir server.aof-integrity-empty-base]
        # Enabling integrity at runtime records checksum 0 for the base, so the INCR file
        # would still verify without it.
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check no] keep_persistence true] {
            r set k1 v1
            r config set aof-integrity-check yes
            wait_for_condition 100 100 {
                [s aof_rewrite_scheduled] == 0 && [s aof_rewrite_in_progress] == 0
            } else {
                fail "AOF rewrite didn't run"
            }
            r set k2 v2
            set base [get_base_aof_path r]
        }
        write_binary_file $base ""
        start_server [list overrides [list dir $sp appendonly yes aof-integrity-check yes] keep_persistence true wait_ready false] {
            wait_for_log_messages 0 {"*AOF base file*is empty, but it was written with integrity*"} 0 10 1000
            wait_for_condition 50 100 {
                [is_alive [srv pid]] == 0
            } else {
                fail "Server is still running despite an empty base"
            }
        }
    }

    test "AOF integrity: aof-integrity-check can't be changed while loading" {
        start_server [list overrides [list appendonly yes appendfsync everysec aof-integrity-check yes key-load-delay 500]] {
            populate 5000
            restart_server 0 false false
            wait_for_condition 50 100 {
                [s loading] eq 1
            } else {
                fail "Server didn't start loading"
            }
            assert_error {*LOADING*} {r config set aof-integrity-check no}
            wait_for_condition 100 100 {
                [s loading] eq 0
            } else {
                fail "Loading didn't finish"
            }
            assert_equal yes [lindex [r config get aof-integrity-check] 1]
            assert_equal 5000 [r dbsize]
        }
    }
}

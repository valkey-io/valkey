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
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes aof-use-rdb-preamble yes] keep_persistence true] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            set large_val [string repeat "A" [expr 1024 * 1024]]
            $rd set large_key $large_val
            set sp_actual [dict get [srv config] dir]
        }
        
        start_server [list overrides [list dir $sp_actual appendonly yes aof-integrity-check yes]] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            set large_val [string repeat "A" [expr 1024 * 1024]]
            assert_equal $large_val [$rd get large_key]
        }
    }

    test "AOF integrity: dynamic toggle and disabled marker" {
        set sp [tmpdir server.aof-integrity-toggle]
        start_server [list overrides [list dir $sp appendonly yes appendfsync always aof-integrity-check yes aof-use-rdb-preamble yes] keep_persistence true] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            set ai_path [file join [dict get [srv config] dir] "appendonlydir" "appendonly.aof.1.incr.aof"]
            set sp_actual [dict get [srv config] dir]
            
            $rd set a 1
            $rd config set aof-integrity-check no
            $rd set b 2
            
            wait_for_condition 50 100 {
                [file size $ai_path] > 50
            } else {
                fail "AOF file not updated"
            }
            
            set fp [open $ai_path r]
            set content [read $fp]
            close $fp
            assert_match {*#INTEGRITY_OFF*} $content
            
            $rd config set aof-integrity-check yes
            $rd set c 3
            
            # Wait for the write to hit disk
            wait_for_condition 50 100 {
                [file size $ai_path] > 100
            } else {
                fail "AOF file not updated after re-enabling"
            }
            
            set fp [open $ai_path r]
            set content [read $fp]
            close $fp
            
            # Verify that a new #HDR:v1 appears after #INTEGRITY_OFF
            assert_match {*#INTEGRITY_OFF*#HDR:v1;*set*c*} $content
        }
        
        start_server [list overrides [list dir $sp_actual appendonly yes aof-integrity-check yes]] {
            set rd [valkey [srv host] [srv port] 0 $::tls]
            assert_equal "1" [$rd get a]
            assert_equal "2" [$rd get b]
            assert_equal "3" [$rd get c]
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
            
            # set c should have an integrity header.
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
            assert {[string match {*checksum *} $content]}
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
        }
    }
}

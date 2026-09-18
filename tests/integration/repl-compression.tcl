tags {"repl external:skip"} {

# repl_compressed_bytes= and repl_uncompressed_bytes= from the replica line of
# the primary's INFO replication.
proc replica_line_compression_bytes {primary} {
    set info [$primary info replication]
    assert {[regexp {repl_compressed_bytes=([0-9]+),repl_uncompressed_bytes=([0-9]+)} \
        $info -> compressed_bytes uncompressed_bytes]}
    return [list $compressed_bytes $uncompressed_bytes]
}

proc replica_line_uncompressed_bytes {primary} {
    return [lindex [replica_line_compression_bytes $primary] 1]
}

set ::replcompression_zstd_supported 0

# Start a fake primary that completes a size-framed full sync, then sends the
# supplied steady-state replication bytes and waits for the replica to close.
proc start_fake_primary_with_stream {rdb_payload stream_payload} {
    set rdb_file [tmpfile fake-primary-rdb]
    set stream_file [tmpfile fake-primary-stream]
    write_binary_file $rdb_file $rdb_payload
    write_binary_file $stream_file $stream_payload
    set port [find_available_port $::baseport $::portcount]
    set pid [exec [info nameofexecutable] tests/helpers/fake_primary.tcl \
                 $port $rdb_file [string length $rdb_payload] $stream_file &]
    wait_for_condition 50 50 {
        [ping_server 127.0.0.1 $port]
    } else {
        fail "Failed to start fake primary"
    }
    return [list $pid $port]
}

# Start a fake dual-channel primary that sends a valid RDB on the RDB channel
# and the supplied buffered command-stream bytes on the main channel.
proc start_fake_dual_channel_primary_with_stream {rdb_payload stream_payload} {
    set rdb_file [tmpfile fake-dual-primary-rdb]
    set stream_file [tmpfile fake-dual-primary-stream]
    write_binary_file $rdb_file $rdb_payload
    write_binary_file $stream_file $stream_payload
    set port [find_available_port $::baseport $::portcount]
    set pid [exec [info nameofexecutable] tests/helpers/fake_dual_channel_primary.tcl \
                 $port $rdb_file $stream_file &]
    wait_for_condition 50 50 {
        [ping_server 127.0.0.1 $port]
    } else {
        fail "Failed to start fake dual-channel primary"
    }
    return [list $pid $port]
}

# ============================================================
# Config CRUD — single-server tests, no replication needed
# ============================================================

start_server {overrides {save "" repl-compression no}} {
    set ::replcompression_zstd_supported [config_value_supported r repl-compression zstd]

    test {repl-compression config: default, set, and survives CONFIG REWRITE and restart} {
        assert_equal "no" [lindex [r config get repl-compression] 1]

        r config set repl-compression yes
        assert_equal "yes" [lindex [r config get repl-compression] 1]
        r config set repl-compression lz4
        assert_equal "lz4" [lindex [r config get repl-compression] 1]
        set rewritten_mode lz4
        if {$::replcompression_zstd_supported} {
            r config set repl-compression zstd
            assert_equal "zstd" [lindex [r config get repl-compression] 1]
            set rewritten_mode zstd
        }
        r config rewrite

        restart_server 0 true false

        assert_equal $rewritten_mode [lindex [r config get repl-compression] 1]

        r config set repl-compression no
    }

    test {Invalid repl-compression config is rejected without changing the setting} {
        set previous [lindex [r config get repl-compression] 1]
        assert_error {*argument(s) must be one of the following:*} {
            r config set repl-compression snappy
        }
        assert_equal $previous [lindex [r config get repl-compression] 1]
    }

    test {ZSTD replication compression config is rejected when unsupported} {
        if {$::replcompression_zstd_supported} {
            skip "zstd is supported by this build"
        }
        assert_error {*Zstandard compression is not available in this build*} {
            r config set repl-compression zstd
        }
    }
}

# ============================================================
# Replication handshake behavior — primary + replica tests
# ============================================================

start_server {tags {"repl"} overrides {save ""}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    # Negotiation and the compressed incremental stream are load-mode
    # independent. Keep both explicit LZ4 load modes and prove that "yes"
    # selects the current default algorithm (LZ4).
    set negotiation_cases {
        {lz4 swapdb}
        {lz4 disabled}
        {yes swapdb}
    }
    if {$::replcompression_zstd_supported} {
        lappend negotiation_cases {zstd swapdb}
    }
    foreach negotiation_case $negotiation_cases {
        lassign $negotiation_case compression_mode diskless_load
        test "Replica negotiates $compression_mode compression (repl-diskless-load $diskless_load)" {
            set expected_algo [expr {$compression_mode eq "yes" ? "lz4" : $compression_mode}]
            $primary config set repl-compression $compression_mode
            set _code [catch {
                start_server [list overrides [list save "" repl-compression $compression_mode repl-diskless-load $diskless_load]] {
                    set replica [srv 0 client]
                    $replica replicaof $primary_host $primary_port

                    wait_for_condition 50 100 {
                        [s 0 master_link_status] eq {up}
                    } else {
                        fail "Replication not started"
                    }

                    # The same negotiated capability covers diskless full sync
                    # and the post-sync incremental stream.
                    wait_for_condition 50 100 {
                        [regexp -all "repl_compression=$expected_algo" [$primary info replication]] >= 1
                    } else {
                        fail "Compression not negotiated"
                    }

                    # Exercise the compressed incremental stream.
                    for {set i 0} {$i < 100} {incr i} {
                        $primary set "negotiated:$i" [string repeat "v" 50]
                    }
                    wait_for_condition 50 100 {
                        [$replica get "negotiated:99"] eq [string repeat "v" 50]
                    } else {
                        fail "Replica did not receive compressed incremental stream"
                    }
                    assert_equal [$primary debug digest] [$replica debug digest]

                    $replica replicaof no one
                }
            } _res _opts]
            $primary config set repl-compression no
            return -options $_opts $_res
        }
    }

    set batch_modes {lz4}
    if {$::replcompression_zstd_supported} {
        lappend batch_modes zstd
    }
    foreach mode $batch_modes {
        test "[string toupper $mode] replication handles compressible and incompressible values across write batches" {
            $primary config set repl-compression $mode
            $primary flushall

            start_server [list overrides [list save "" repl-compression $mode repl-diskless-load swapdb]] {
                set replica [srv 0 client]
                $replica replicaof $primary_host $primary_port

                wait_for_condition 50 200 {
                    [s 0 master_link_status] eq {up} &&
                    [regexp "state=online.*repl_compression=$mode,repl_compressed_bytes=\[0-9\]+,repl_uncompressed_bytes=\[0-9\]+" \
                        [$primary info replication]]
                } else {
                    fail "$mode replication compression not established"
                }

                if {$mode eq "zstd"} {
                    # Repeating an individually incompressible value across
                    # small write batches requires the frame history to span
                    # SYNC flushes. Ending one frame per batch stays near the
                    # uncompressed size instead.
                    expr {srand(8675309)}
                    set history_payload ""
                    for {set i 0} {$i < 512} {incr i} {
                        append history_payload [format %c [expr {33 + int(rand() * 90)}]]
                    }
                    lassign [replica_line_compression_bytes $primary] compressed_before uncompressed_before
                    for {set i 0} {$i < 32} {incr i} {
                        set key "zstd:history:$i"
                        $primary set $key $history_payload
                        wait_for_condition 50 100 {
                            [$replica get $key] eq $history_payload
                        } else {
                            fail "ZSTD replication did not deliver small write batch $i"
                        }
                    }
                    lassign [replica_line_compression_bytes $primary] compressed_after uncompressed_after
                    set compressed_delta [expr {$compressed_after - $compressed_before}]
                    set uncompressed_delta [expr {$uncompressed_after - $uncompressed_before}]
                    if {$uncompressed_delta <= 0 || $compressed_delta * 2 >= $uncompressed_delta} {
                        fail "ZSTD did not retain compression history across write batches: $compressed_delta compressed bytes for $uncompressed_delta uncompressed bytes"
                    }
                }

                # This spans several 1 MiB raw batches. For Zstd it also
                # exercises multiple bounded frames on one VCS stream.
                set compressible [string repeat "abcdefghij0123456789" 209715]
                $primary set "$mode:compressible" $compressible

                # A deterministic ratio≈1 payload exercises worst-case
                # compressed output sizing across multiple raw batches.
                expr {srand(424242)}
                set incompressible ""
                while {[string length $incompressible] < 1572864} {
                    set chunk ""
                    for {set i 0} {$i < 4096} {incr i} {
                        append chunk [format %c [expr {int(rand()*256)}]]
                    }
                    append incompressible $chunk
                }
                $primary set "$mode:incompressible" $incompressible

                wait_for_condition 50 200 {
                    [$replica get "$mode:compressible"] eq $compressible &&
                    [$replica get "$mode:incompressible"] eq $incompressible
                } else {
                    fail "$mode replication did not preserve data"
                }
                assert_equal [$primary debug digest] [$replica debug digest]
                $replica replicaof no one
            }
            $primary config set repl-compression no
        }
    }

    test {Primary ZSTD replication writer closes bounded frames without repeating the envelope} {
        if {!$::replcompression_zstd_supported} {
            skip "zstd is not supported by this build"
        }
        if {$::tls} {
            skip "bounded integrity frames apply only to plaintext replication links"
        }

        set capture ""
        set old_repl_compression [lindex [$primary config get repl-compression] 1]
        set old_rdbcompression [lindex [$primary config get rdbcompression] 1]
        set old_diskless_sync [lindex [$primary config get repl-diskless-sync] 1]
        set old_ping_period [lindex [$primary config get repl-ping-replica-period] 1]
        set old_key_save_delay [lindex [$primary config get rdb-key-save-delay] 1]
        with_cleanup {
            $primary config set repl-compression zstd
            $primary config set rdbcompression no
            $primary config set repl-diskless-sync no
            $primary config set repl-ping-replica-period 3600
            $primary flushall
            $primary set zstd:wire:seed seed
            # Keep the disk-backed full sync alive across multiple replication
            # cron ticks so the raw replica deterministically receives a keepalive.
            $primary config set rdb-key-save-delay 2500000

            set capture [socket $primary_host $primary_port]
            fconfigure $capture -translation binary -buffering none

            puts -nonewline $capture "REPLCONF capa psync2 capa zstd capa lz4\r\n"
            flush $capture
            assert_equal "+OK" [string trim [gets $capture]]

            puts -nonewline $capture "PSYNC ? -1\r\n"
            flush $capture
            assert_match "+FULLRESYNC *" [string trim [gets $capture]]

            set presync_keepalives 0
            while {1} {
                set nread [gets $capture bulk_header]
                assert {$nread >= 0}
                set bulk_header [string trim $bulk_header]
                if {$bulk_header ne ""} break
                incr presync_keepalives
            }
            assert {$presync_keepalives > 0}
            assert_equal {$} [string index $bulk_header 0]
            set remaining [string range $bulk_header 1 end]
            while {$remaining > 0} {
                set chunk [read $capture $remaining]
                assert {$chunk ne ""}
                incr remaining -[string length $chunk]
            }

            wait_for_condition 50 100 {
                [regexp {state=online.*repl_compression=zstd} [$primary info replication]]
            } else {
                fail "Raw replica did not establish ZSTD compression"
            }

            # Crossing two raw frame budgets must produce multiple Zstd frames
            # on one VCS stream. Only stable frame and envelope magics are
            # inspected; codec header fields remain opaque.
            $primary set zstd:wire:capture [string repeat x [expr {2 * 1024 * 1024 + 128}]]

            fconfigure $capture -blocking 0
            set wire ""
            set zstd_magic "\x28\xb5\x2f\xfd"
            set vcs_envelope "\x56\x43\x53\x01\x02\x00\x02"
            set frame_count 0
            for {set attempt 0} {$attempt < 100 && $frame_count < 2} {incr attempt} {
                append wire [read $capture 65536]
                set frame_count 0
                set offset 0
                while {[set pos [string first $zstd_magic $wire $offset]] >= 0} {
                    incr frame_count
                    set offset [expr {$pos + [string length $zstd_magic]}]
                }
                if {$frame_count < 2} {
                    after 50
                }
            }
            assert {$frame_count >= 2}

            set envelope_count 0
            set offset 0
            while {[set pos [string first $vcs_envelope $wire $offset]] >= 0} {
                incr envelope_count
                set offset [expr {$pos + [string length $vcs_envelope]}]
            }
            assert_equal 1 $envelope_count
        } {
            if {$capture ne ""} {
                catch {close $capture}
            }
            catch {$primary config set rdb-key-save-delay $old_key_save_delay}
            catch {$primary config set repl-ping-replica-period $old_ping_period}
            catch {$primary config set repl-diskless-sync $old_diskless_sync}
            catch {$primary config set rdbcompression $old_rdbcompression}
            catch {$primary config set repl-compression $old_repl_compression}
        }
    }

    test {Backlog cursor stays pinned until the compressed batch fully drains} {
        # The cursor advances only on full out_buf drain: pause the replica and
        # uncompressed_bytes must freeze while master_repl_offset grows.
        $primary config set repl-compression lz4
        $primary flushall

        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            set replica_pid [srv 0 pid]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started"
            }
            wait_for_condition 50 200 {
                [string match {*state=online*repl_compression=lz4*} [$primary info replication]]
            } else {
                fail "Compression not active on replica"
            }

            $primary set pin:baseline baseline_val
            wait_for_ofs_sync $primary $replica
            # The link must survive on the same connection (no resync).
            set sync_full_before [status $primary sync_full]
            set sync_partial_before [status $primary sync_partial_ok]

            pause_process $replica_pid
            set pause_code [catch {

                # Pseudo-random 100KB block (past LZ4's 64KB window): ratio ~1
                # overfills the socket buffers and leaves out_buf mid-batch.
                expr {srand(51555)}
                set payload ""
                while {[string length $payload] < 102400} {
                    set chunk ""
                    for {set i 0} {$i < 4096} {incr i} {
                        append chunk [format %c [expr {int(rand()*256)}]]
                    }
                    append payload $chunk
                }
                for {set i 0} {$i < 200} {incr i} {
                    $primary set "pin:burst:$i" $payload
                }

                # Wait for the residual kernel-buffer drain to settle.
                set previous_uncompressed_bytes [replica_line_uncompressed_bytes $primary]
                set stable_samples 0
                for {set i 0} {$i < 100 && $stable_samples < 3} {incr i} {
                    after 100
                    set current_uncompressed_bytes [replica_line_uncompressed_bytes $primary]
                    if {$current_uncompressed_bytes == $previous_uncompressed_bytes} {
                        incr stable_samples
                    } else {
                        set stable_samples 0
                    }
                    set previous_uncompressed_bytes $current_uncompressed_bytes
                }
                assert_equal 3 $stable_samples

                # Frozen cursor: two samples with writes in between must be equal.
                set uncompressed_bytes_before [replica_line_uncompressed_bytes $primary]
                set repl_offset_before [status $primary master_repl_offset]
                for {set i 0} {$i < 20} {incr i} {
                    $primary set "pin:tick:$i" tick_val
                }
                after 300
                set uncompressed_bytes_after [replica_line_uncompressed_bytes $primary]
                set repl_offset_after [status $primary master_repl_offset]

                assert {$repl_offset_after > $repl_offset_before}
                assert_equal $uncompressed_bytes_before $uncompressed_bytes_after
            } pause_result pause_opts]
            resume_process $replica_pid
            if {$pause_code} {
                return -options $pause_opts $pause_result
            }

            # Pinned batches drain after the replica resumes.
            wait_for_ofs_sync $primary $replica
            assert {[$replica get pin:burst:199] eq $payload}
            assert_equal tick_val [$replica get pin:tick:19]
            assert {[replica_line_uncompressed_bytes $primary] > $uncompressed_bytes_after}
            assert_equal $sync_full_before [status $primary sync_full]
            assert_equal $sync_partial_before [status $primary sync_partial_ok]

            $replica replicaof no one
        }

        $primary config set repl-compression no
    }

    test {Compressed replica obeys the hard output buffer limit and resynchronizes} {
        $primary config set repl-compression lz4
        $primary config set repl-backlog-size 1mb
        $primary config set client-output-buffer-limit "replica 4mb 0 0"
        $primary flushall

        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            set replica_pid [srv 0 pid]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 200 {
                [s 0 master_link_status] eq {up} &&
                [string match {*state=online*repl_compression=lz4*} [$primary info replication]]
            } else {
                fail "Compressed replication not established"
            }

            set sync_full_before [status $primary sync_full]
            set cob_disconnections_before [s -1 client_output_buffer_limit_disconnections]
            expr {srand(99173)}
            set payload [randstring [expr {256 * 1024}] [expr {256 * 1024}]]
            set last_key ""

            pause_process $replica_pid
            set pause_code [catch {
                # Incompressible writes fill the compressed staging buffer and
                # pin raw backlog blocks while the replica cannot read.
                for {set i 0} {$i < 128} {incr i} {
                    set last_key "cob:$i"
                    $primary set $last_key $payload
                    if {[status $primary connected_slaves] == 0} break
                }

                wait_for_condition 100 100 {
                    [status $primary connected_slaves] == 0
                } else {
                    fail "Primary did not disconnect compressed replica at the hard output buffer limit"
                }
                wait_for_condition 100 100 {
                    [s -1 client_output_buffer_limit_disconnections] > $cob_disconnections_before
                } else {
                    fail "Primary did not record the output buffer limit disconnection"
                }

                assert_equal "" [string trim [$primary client list type replica]]
                wait_for_condition 100 100 {
                    [status $primary repl_backlog_histlen] <= 2 * 1024 * 1024
                } else {
                    fail "Compressed replica backlog reference was not released"
                }
            } pause_result pause_options]
            resume_process $replica_pid
            if {$pause_code} {
                return -options $pause_options $pause_result
            }

            # The burst is larger than the 1 MiB backlog, so reconnection needs
            # a full sync. Incremental compression must be active afterward.
            wait_for_condition 300 100 {
                [status $primary sync_full] == $sync_full_before + 1 &&
                [s 0 master_link_status] eq {up} &&
                [string match {*state=online*repl_compression=lz4*} [$primary info replication]]
            } else {
                fail "Replica did not resynchronize with compression after the output buffer disconnect"
            }
            assert_equal $payload [$replica get $last_key]

            $primary set cob:after-resync delivered
            wait_for_condition 50 100 {
                [$replica get cob:after-resync] eq {delivered}
            } else {
                fail "Replication did not continue after compressed resynchronization"
            }

            $replica replicaof no one
        }

        $primary config set repl-compression no
        $primary config set repl-backlog-size 10mb
        $primary config set client-output-buffer-limit "replica 256mb 64mb 60"
    }

    foreach mode $batch_modes {
        test "Compressed partial resync with $mode preserves data and decoded ACK offsets" {
            $primary config set repl-compression $mode
            $primary flushall

            start_server [list overrides [list save "" repl-compression $mode repl-diskless-load swapdb]] {
                set replica [srv 0 client]
                $replica replicaof $primary_host $primary_port

                wait_for_condition 50 200 {
                    [s 0 master_link_status] eq {up} &&
                    [string match "*state=online*repl_compression=$mode*" [$primary info replication]]
                } else {
                    fail "$mode replication compression not established"
                }

                $primary set key1 value1
                wait_for_condition 50 100 {
                    [$replica get key1] eq {value1}
                } else {
                    fail "Initial replication failed with $mode"
                }

                # Killing the primary-side connection preserves the replica's
                # cached offset and exercises a compressed partial resync.
                set full_before [status $primary sync_full]
                set partial_before [status $primary sync_partial_ok]
                $primary client kill type replica

                wait_for_condition 50 200 {
                    [s 0 master_link_status] eq {up} &&
                    [string match "*state=online*repl_compression=$mode*" [$primary info replication]] &&
                    [status $primary sync_partial_ok] == $partial_before + 1
                } else {
                    fail "$mode partial resync did not complete"
                }
                assert_equal $full_before [status $primary sync_full]

                # WAIT compares logical RESP offsets. A highly compressible
                # write only reaches the target if the replica advances by
                # decoded bytes, not by the smaller number of wire bytes.
                set ack_payload [string repeat x 262144]
                $primary set key2 value2
                $primary set key3 $ack_payload
                assert_equal 1 [$primary wait 1 5000]
                assert_equal value1 [$replica get key1]
                assert_equal value2 [$replica get key2]
                assert_equal $ack_payload [$replica get key3]

                $replica replicaof no one
            }
            $primary config set repl-compression no
        }
    }

    test {Compressed replication survives an interrupted frame and reconverges} {
        # A replication frame remains open for the life of its connection, so
        # dropping the link interrupts the frame without a closing marker.
        $primary config set repl-compression lz4
        $primary flushall

        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 200 {
                [s 0 master_link_status] eq {up} &&
                [string match {*state=online*repl_compression=lz4*} [$primary info replication]]
            } else {
                fail "Compressed replication not established"
            }

            # Baseline sync so recovery is a clean reconverge from a known point.
            $primary set trunc:baseline baseline_val
            wait_for_ofs_sync $primary $replica
            set full_before [status $primary sync_full]
            set partial_before [status $primary sync_partial_ok]

            # Keep writing after the disconnect so either a partial or full
            # resynchronization must converge to a newer offset.
            set bigval [string repeat "abcdefghij0123456789" 52429] ;# ~1 MiB
            for {set i 0} {$i < 60} {incr i} {
                $primary set "trunc:load:$i" $bigval
                if {$i == 30} {
                    # Kill from the primary side while the stream frame is open.
                    $primary client kill type replica
                }
            }

            # The truncated link forces the replica to reconnect and resync
            # (partial or full, either is fine) and re-establish compression.
            $primary set trunc:final final_val
            wait_for_condition 100 200 {
                [s 0 master_link_status] eq {up} &&
                [string match {*state=online*repl_compression=lz4*} [$primary info replication]] &&
                [status $primary sync_full] + [status $primary sync_partial_ok] > $full_before + $partial_before
            } else {
                fail "Replica did not reconnect with compression after mid-frame truncation"
            }

            # (a) The replica survived the truncated frame: no assertion or
            # crash was logged.
            assert_equal 0 [count_log_message 0 "*=== ASSERTION FAILED ===*"]
            assert_equal 0 [count_log_message 0 "*crashed by signal*"]

            # (b) The link is back up (asserted in the wait above) and (c) the
            # datasets converge byte-identically once offsets align.
            wait_for_ofs_sync $primary $replica
            assert_equal [$primary debug digest] [$replica debug digest]

            $replica replicaof no one
        }
        $primary config set repl-compression no
    }

    if {!$::tls} {
        foreach mode $batch_modes {
            test "Corrupt $mode replication stream disconnects cleanly and recovers" {
                $primary config set repl-compression $mode
                $primary flushall
                $primary set corrupt:baseline baseline_val
                $primary save

                set rdb_payload [read_binary_file [server_rdb_path $primary]]
                if {$mode eq "zstd"} {
                    # Reuse a complete checksummed Zstd RDB frame as replication
                    # input, then corrupt its checksum after changing only the
                    # VCS stream kind.
                    set old_rdbcompression [lindex [$primary config get rdbcompression] 1]
                    $primary config set rdbcompression zstd
                    $primary save
                    set corrupt_stream [read_binary_file [server_rdb_path $primary]]
                    $primary config set rdbcompression $old_rdbcompression
                    set corrupt_stream [string replace $corrupt_stream 6 6 "\x02"]
                    set checksum_pos [expr {[string length $corrupt_stream] - 1}]
                    binary scan [string index $corrupt_stream $checksum_pos] cu checksum_byte
                    set flipped_checksum_byte [binary format H2 [format %02x [expr {$checksum_byte ^ 1}]]]
                    set corrupt_stream [string replace $corrupt_stream $checksum_pos $checksum_pos \
                        $flipped_checksum_byte]
                } else {
                    # Valid VCS replication envelope followed by an invalid LZ4 frame.
                    set corrupt_stream [binary format H* 56435301010002]
                    append corrupt_stream [string repeat "\x00" 64]
                }

                set fake_pid ""
                with_cleanup {
                    lassign [start_fake_primary_with_stream $rdb_payload $corrupt_stream] fake_pid fake_port

                    start_server [list overrides [list save "" repl-compression $mode repl-diskless-load swapdb]] {
                        set replica [srv 0 client]
                        set replica_loglines [count_log_lines 0]
                        $replica replicaof 127.0.0.1 $fake_port

                        wait_for_log_messages 0 {"*replication stream decompression failure*"} \
                            $replica_loglines 100 100
                        wait_for_condition 50 100 {
                            [s 0 master_link_status] eq {down}
                        } else {
                            fail "Replica did not disconnect from the corrupt $mode stream"
                        }
                        assert_equal {PONG} [$replica ping]
                        assert_equal {baseline_val} [$replica get corrupt:baseline]

                        # A fresh replication session must not retain any corrupt
                        # decoder state from the failed link.
                        $replica replicaof $primary_host $primary_port
                        wait_for_condition 50 200 {
                            [s 0 master_link_status] eq {up} &&
                            [$replica get corrupt:baseline] eq {baseline_val} &&
                            [string match "*state=online*repl_compression=$mode*" [$primary info replication]]
                        } else {
                            fail "Replica did not recover after $mode stream corruption"
                        }

                        $replica replicaof no one
                    }
                } {
                    if {$fake_pid ne ""} {catch {exec kill $fake_pid}}
                    $primary config set repl-compression no
                }
            }

            test "Corrupt buffered $mode dual-channel stream retries without promoting the replica" {
                $primary config set repl-compression $mode
                $primary flushall
                $primary set dual-corrupt:baseline baseline_val
                $primary save

                set rdb_payload [read_binary_file [server_rdb_path $primary]]
                set codec_hex [expr {$mode eq "zstd" ? "02" : "01"}]
                set corrupt_stream [binary format H* "56435301${codec_hex}0002"]
                append corrupt_stream [string repeat "\x00" 64]

                set fake_pid ""
                with_cleanup {
                    lassign [start_fake_dual_channel_primary_with_stream $rdb_payload $corrupt_stream] fake_pid fake_port

                    start_server [list overrides [list save "" repl-compression $mode dual-channel-replication-enabled yes repl-diskless-load swapdb]] {
                        set replica [srv 0 client]
                        set replica_loglines [count_log_lines 0]
                        $replica replicaof 127.0.0.1 $fake_port

                        wait_for_log_messages 0 {"*Dual-channel replication stream decompression failure*"} \
                            $replica_loglines 100 100
                        wait_for_condition 50 100 {
                            [lindex [$replica role] 0] eq {slave} &&
                            [s 0 master_host] eq {127.0.0.1} &&
                            [s 0 master_port] == $fake_port &&
                            [s 0 master_link_status] eq {down}
                        } else {
                            fail "Replica did not retain its primary after buffered $mode stream corruption"
                        }
                        assert_equal {baseline_val} [$replica get dual-corrupt:baseline]

                        $replica replicaof $primary_host $primary_port
                        wait_for_condition 50 200 {
                            [s 0 master_link_status] eq {up} &&
                            [$replica get dual-corrupt:baseline] eq {baseline_val} &&
                            [string match "*state=online*repl_compression=$mode*" [$primary info replication]]
                        } else {
                            fail "Replica did not recover after dual-channel $mode stream corruption"
                        }

                        $replica replicaof no one
                    }
                } {
                    if {$fake_pid ne ""} {catch {exec kill $fake_pid}}
                    $primary config set repl-compression no
                }
            }
        }
    }

    test {Replica with repl-compression lz4 handles a plaintext primary (passthrough)} {
        # Primary has compression OFF, replica ON: the replica advertises the
        # capability but the primary sends plaintext, so the replica must pass
        # the stream through untouched rather than expecting a VCS envelope.
        $primary config set repl-compression no
        $primary flushall

        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started (primary plaintext, replica compression on)"
            }

            # Incremental writes arrive as plaintext; passthrough must deliver them.
            for {set i 0} {$i < 50} {incr i} {
                $primary set "pt:$i" [string repeat "payload$i " 20]
            }
            wait_for_condition 50 100 {
                [$replica get pt:49] eq [string repeat "payload49 " 20]
            } else {
                fail "Replica did not receive plaintext data via passthrough"
            }
            assert_equal [$primary dbsize] [$replica dbsize]

            # The link remained plaintext.
            assert_equal 0 [string match {*repl_compression=lz4*} [$primary info replication]]

            # Disabling a link already classified as plaintext must not cause
            # an unnecessary reconnect.
            set full_before [status $primary sync_full]
            set partial_before [status $primary sync_partial_ok]
            $replica config set repl-compression no
            after 1500
            assert_equal $full_before [status $primary sync_full]
            assert_equal $partial_before [status $primary sync_partial_ok]
            assert_equal up [s 0 master_link_status]

            $replica replicaof no one
        }
    }

    if {$::replcompression_zstd_supported} {
        test {ZSTD replica advertises LZ4 fallback and renegotiates to ZSTD} {
            $primary config set repl-compression lz4
            $primary flushall

            start_server {overrides {save "" repl-compression zstd repl-diskless-load swapdb}} {
                set replica [srv 0 client]
                $replica replicaof $primary_host $primary_port

                wait_for_condition 50 200 {
                    [s 0 master_link_status] eq {up} &&
                    [regexp -all {repl_compression=lz4} [$primary info replication]] == 1
                } else {
                    fail "ZSTD replica did not negotiate LZ4 fallback"
                }

                $primary set fallback:lz4 initial
                wait_for_condition 50 100 {
                    [$replica get fallback:lz4] eq {initial}
                } else {
                    fail "LZ4 fallback did not replicate data"
                }

                set full_before [status $primary sync_full]
                set partial_before [status $primary sync_partial_ok]

                # The primary prefers its configured ZSTD codec when available.
                $primary config set repl-compression zstd
                wait_for_condition 50 200 {
                    [s 0 master_link_status] eq {up} &&
                    [regexp -all {repl_compression=zstd} [$primary info replication]] == 1 &&
                    [status $primary sync_partial_ok] == $partial_before + 1
                } else {
                    fail "Primary did not renegotiate LZ4 fallback to ZSTD"
                }

                $primary set fallback:zstd delivered
                wait_for_condition 50 100 {
                    [$replica get fallback:zstd] eq {delivered}
                } else {
                    fail "ZSTD link did not replicate data"
                }
                assert_equal $full_before [status $primary sync_full]
                assert_equal [$primary debug digest] [$replica debug digest]

                $replica replicaof no one
            }
            $primary config set repl-compression no
        }

        test {ZSTD primary leaves an LZ4-only replica plaintext} {
            $primary config set repl-compression zstd
            $primary flushall

            start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
                set replica [srv 0 client]
                $replica replicaof $primary_host $primary_port

                wait_for_condition 50 200 {
                    [s 0 master_link_status] eq {up}
                } else {
                    fail "LZ4-only replica did not connect to ZSTD primary"
                }
                assert_equal 0 [regexp -all {repl_compression=} [$primary info replication]]

                $primary set zstd:lz4-only delivered
                wait_for_condition 50 100 {
                    [$replica get zstd:lz4-only] eq {delivered}
                } else {
                    fail "Plaintext fallback did not replicate data"
                }

                $replica replicaof no one
            }
            $primary config set repl-compression no
        }
    }

    test {Replica config change waits for an in-progress full sync} {
        $primary config set repl-compression lz4
        $primary config set rdb-key-save-delay 1000
        $primary flushall
        $primary debug populate 5000 sync-config: 100

        set full_before [status $primary sync_full]
        set partial_before [status $primary sync_partial_ok]
        set _code [catch {
            start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
                set replica [srv 0 client]
                $replica replicaof $primary_host $primary_port

                wait_for_condition 100 100 {
                    [s 0 master_sync_in_progress] == 1 &&
                    [status $primary sync_full] == $full_before + 1
                } else {
                    fail "Full sync did not start"
                }

                $replica config set repl-compression no
                after 1500
                assert_equal 1 [s 0 master_sync_in_progress]
                assert_equal [expr {$full_before + 1}] [status $primary sync_full]

                wait_for_condition 200 100 {
                    [s 0 master_link_status] eq {up} &&
                    [regexp -all {repl_compression=lz4} [$primary info replication]] == 0 &&
                    [status $primary sync_partial_ok] == $partial_before + 1
                } else {
                    fail "Replica did not renegotiate after the full sync completed"
                }
                assert_equal [expr {$full_before + 1}] [status $primary sync_full]

                $replica replicaof no one
            }
        } _res _opts]
        $primary config set rdb-key-save-delay 0
        $primary config set repl-compression no
        return -options $_opts $_res
    }

    test {Replica repl-compression flips renegotiate upstream in both directions} {
        $primary config set repl-compression lz4

        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 200 {
                [s 0 master_link_status] eq {up} &&
                [regexp -all {repl_compression=lz4} [$primary info replication]] == 1
            } else {
                fail "Compressed replication not established"
            }

            set full_before [status $primary sync_full]
            set partial_before [status $primary sync_partial_ok]

            # The whole command rolls back, so the compressed link stays up.
            assert_error {*argument 'maxmemory-policy'*} {
                $replica config set repl-compression no maxmemory-policy not-a-policy
            }
            assert_equal lz4 [lindex [$replica config get repl-compression] 1]
            after 1500
            assert_equal $partial_before [status $primary sync_partial_ok]
            assert_equal 1 [regexp -all {repl_compression=lz4} [$primary info replication]]

            # Returning to the advertised state before cron runs does not
            # require renegotiating the existing link.
            $replica debug pause-cron 1
            set pause_code [catch {
                $replica config set repl-compression no
                $replica config set repl-compression lz4
            } pause_result pause_opts]
            $replica debug pause-cron 0
            if {$pause_code} {
                return -options $pause_opts $pause_result
            }
            after 1500
            assert_equal $partial_before [status $primary sync_partial_ok]
            assert_equal 1 [regexp -all {repl_compression=lz4} [$primary info replication]]

            set transition 0
            foreach {mode expected_compressed} {no 0 lz4 1} {
                incr transition
                $replica config set repl-compression $mode
                wait_for_condition 50 200 {
                    [s 0 master_link_status] eq {up} &&
                    [regexp -all {repl_compression=lz4} [$primary info replication]] == $expected_compressed &&
                    [status $primary sync_partial_ok] == $partial_before + $transition
                } else {
                    fail "Replica did not renegotiate after setting repl-compression $mode"
                }

                $primary set "replica-flip:$mode" "value:$mode"
                wait_for_condition 50 100 {
                    [$replica get "replica-flip:$mode"] eq "value:$mode"
                } else {
                    fail "Data did not replicate after setting repl-compression $mode"
                }
            }
            assert_equal $full_before [status $primary sync_full]
            $replica replicaof no one
        }
        $primary config set repl-compression no
    }

    test {Primary config flips preserve independent compressed and plaintext replica links} {
        $primary config set repl-compression no
        $primary flushall

        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set compressed_replica [srv 0 client]
            $compressed_replica replicaof $primary_host $primary_port
            wait_for_sync $compressed_replica

            start_server {overrides {save "" repl-compression no repl-diskless-load swapdb}} {
                set plaintext_replica [srv 0 client]
                $plaintext_replica replicaof $primary_host $primary_port
                wait_for_sync $plaintext_replica

                assert_equal 0 [regexp -all {repl_compression=lz4} [$primary info replication]]
                set full_before [status $primary sync_full]
                set partial_before [status $primary sync_partial_ok]

                # Only the capable replica renegotiates when the primary enables
                # compression; the opted-out replica remains on its link.
                $primary config set repl-compression lz4
                wait_for_condition 50 200 {
                    [s 0 master_link_status] eq {up} &&
                    [status $compressed_replica master_link_status] eq {up} &&
                    [regexp -all {repl_compression=lz4} [$primary info replication]] == 1 &&
                    [status $primary sync_partial_ok] == $partial_before + 1
                } else {
                    fail "Mixed replica links did not converge after enabling compression"
                }
                $primary set mixed:compressed delivered
                wait_for_condition 50 100 {
                    [$compressed_replica get mixed:compressed] eq {delivered} &&
                    [$plaintext_replica get mixed:compressed] eq {delivered}
                } else {
                    fail "Mixed replica links did not both receive compressed-phase data"
                }

                # Disabling compression again reconnects only the compressed
                # link and leaves both replicas receiving plaintext.
                $primary config set repl-compression no
                wait_for_condition 50 200 {
                    [s 0 master_link_status] eq {up} &&
                    [status $compressed_replica master_link_status] eq {up} &&
                    [regexp -all {repl_compression=lz4} [$primary info replication]] == 0 &&
                    [status $primary sync_partial_ok] == $partial_before + 2
                } else {
                    fail "Mixed replica links did not converge after disabling compression"
                }
                assert_equal $full_before [status $primary sync_full]

                $primary set mixed:plaintext delivered
                wait_for_condition 50 100 {
                    [$compressed_replica get mixed:plaintext] eq {delivered} &&
                    [$plaintext_replica get mixed:plaintext] eq {delivered}
                } else {
                    fail "Mixed replica links did not both receive plaintext-phase data"
                }
                assert_equal [$primary debug digest] [$compressed_replica debug digest]
                assert_equal [$primary debug digest] [$plaintext_replica debug digest]

                $plaintext_replica replicaof no one
            }
            $compressed_replica replicaof no one
        }
    }

    if {$::replcompression_zstd_supported} {
        test {Primary codec flips preserve ZSTD, LZ4-only, and plaintext replica links} {
            $primary config set repl-compression no
            $primary flushall

            start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
                set lz4_replica [srv 0 client]
                $lz4_replica replicaof $primary_host $primary_port
                wait_for_sync $lz4_replica

                start_server {overrides {save "" repl-compression zstd repl-diskless-load swapdb}} {
                    set zstd_replica [srv 0 client]
                    $zstd_replica replicaof $primary_host $primary_port
                    wait_for_sync $zstd_replica

                    start_server {overrides {save "" repl-compression no repl-diskless-load swapdb}} {
                        set plain_replica [srv 0 client]
                        $plain_replica replicaof $primary_host $primary_port
                        wait_for_sync $plain_replica

                        set full_before [status $primary sync_full]
                        set partial_before [status $primary sync_partial_ok]

                        # Only the Zstd-capable replica upgrades. The LZ4-only
                        # and opted-out replicas remain on plaintext links.
                        $primary config set repl-compression zstd
                        wait_for_condition 100 200 {
                            [regexp -all {repl_compression=zstd} [$primary info replication]] == 1 &&
                            [regexp -all {repl_compression=lz4} [$primary info replication]] == 0 &&
                            [status $primary sync_partial_ok] == $partial_before + 1
                        } else {
                            fail "Mixed capability replicas did not converge to ZSTD"
                        }

                        $primary set mixed-codecs:zstd-phase delivered
                        wait_for_condition 50 100 {
                            [$lz4_replica get mixed-codecs:zstd-phase] eq {delivered} &&
                            [$zstd_replica get mixed-codecs:zstd-phase] eq {delivered} &&
                            [$plain_replica get mixed-codecs:zstd-phase] eq {delivered}
                        } else {
                            fail "Data did not reach every replica during the ZSTD phase"
                        }

                        # Moving to LZ4 upgrades the LZ4-only replica and moves
                        # the Zstd replica to its advertised LZ4 fallback.
                        $primary config set repl-compression lz4
                        wait_for_condition 100 200 {
                            [regexp -all {repl_compression=lz4} [$primary info replication]] == 2 &&
                            [regexp -all {repl_compression=zstd} [$primary info replication]] == 0 &&
                            [status $primary sync_partial_ok] == $partial_before + 3
                        } else {
                            fail "Mixed capability replicas did not converge from ZSTD to LZ4"
                        }
                        assert_equal $full_before [status $primary sync_full]

                        $primary set mixed-codecs:final delivered
                        wait_for_condition 50 100 {
                            [$lz4_replica get mixed-codecs:final] eq {delivered} &&
                            [$zstd_replica get mixed-codecs:final] eq {delivered} &&
                            [$plain_replica get mixed-codecs:final] eq {delivered}
                        } else {
                            fail "Data did not reach every replica after codec fallback"
                        }
                        assert_equal [$primary debug digest] [$lz4_replica debug digest]
                        assert_equal [$primary debug digest] [$zstd_replica debug digest]
                        assert_equal [$primary debug digest] [$plain_replica debug digest]

                        $plain_replica replicaof no one
                    }
                    $zstd_replica replicaof no one
                }
                $lz4_replica replicaof no one
            }
            $primary config set repl-compression no
        }
    }

    test {Dual-channel full sync with compression delivers writes made during load} {
        $primary config set repl-compression lz4
        $primary config set dual-channel-replication-enabled yes
        $primary config set rdb-key-save-delay 100
        $primary flushall
        $primary debug populate 10000 dc: 100

        start_server {overrides {save "" repl-compression lz4 dual-channel-replication-enabled yes}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port

            # rdb-key-save-delay stretches the RDB stage; catch the sync window.
            wait_for_condition 500 10 {
                [s 0 master_sync_in_progress] == 1 &&
                [string match {*state=bg_transfer*repl_compression=lz4*} [$primary info replication]]
            } else {
                fail "Dual-channel sync did not start"
            }

            # Writes made during load reach the replica via the compressed main
            # channel: +CONTINUE starts compression, put-online must not restart
            # it (a second init would emit a new envelope mid-frame).
            for {set i 0} {$i < 200} {incr i} {
                $primary set "during_load:$i" "value_$i"
            }
            # This compresses below one socket read but expands past one decode
            # budget, exercising streamReplDataBufToDb's resumable decode loop.
            set during_load_payload [string repeat x [expr {2 * 1024 * 1024}]]
            $primary set during_load:large $during_load_payload
            assert_equal 1 [s 0 master_sync_in_progress]
            assert_match {*state=bg_transfer*repl_compression=lz4*} [$primary info replication]

            wait_for_condition 100 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not up after dual-channel sync"
            }

            wait_for_condition 50 100 {
                [$replica get "during_load:199"] eq {value_199}
            } else {
                fail "Writes made during load did not reach the replica"
            }
            for {set i 0} {$i < 200} {incr i} {
                assert_equal "value_$i" [$replica get "during_load:$i"]
            }
            assert_equal $during_load_payload [$replica get during_load:large]
            assert_match "*repl_compression=lz4*" [$primary info replication]
            wait_for_ofs_sync $primary $replica

            # A double init would emit a second envelope mid-frame on the first
            # post-online write, corrupting the replica and forcing a resync.
            # Stable sync counters prove the link survived that first write.
            set sync_full_before [s -1 sync_full]
            set sync_partial_before [s -1 sync_partial_ok]
            $primary set post_online_probe delivered
            wait_for_condition 50 100 {
                [$replica get post_online_probe] eq {delivered}
            } else {
                fail "Post-online write did not reach the replica"
            }
            assert_equal $sync_full_before [s -1 sync_full]
            assert_equal $sync_partial_before [s -1 sync_partial_ok]

            $replica replicaof no one
        }
        $primary config set rdb-key-save-delay 0
        $primary config set dual-channel-replication-enabled no
        $primary config set repl-compression no
    }

    if {$::replcompression_zstd_supported} {
        test {Dual-channel ZSTD stream handles bounded frames during load} {
            $primary config set repl-compression zstd
            $primary config set dual-channel-replication-enabled yes
            $primary config set rdb-key-save-delay 100
            $primary flushall
            $primary debug populate 10000 zstd-dc: 100

            start_server {overrides {save "" repl-compression zstd dual-channel-replication-enabled yes}} {
                set replica [srv 0 client]
                $replica replicaof $primary_host $primary_port

                wait_for_condition 500 10 {
                    [s 0 master_sync_in_progress] == 1 &&
                    [string match {*state=bg_transfer*repl_compression=zstd*} [$primary info replication]]
                } else {
                    fail "Dual-channel ZSTD sync did not start"
                }

                set payload [string repeat x [expr {2 * 1024 * 1024}]]
                $primary set zstd-during-load $payload

                wait_for_condition 100 100 {
                    [s 0 master_link_status] eq {up} &&
                    [$replica get zstd-during-load] eq $payload
                } else {
                    fail "Dual-channel ZSTD stream did not preserve buffered writes"
                }
                wait_for_ofs_sync $primary $replica

                $replica replicaof no one
            }
            $primary config set rdb-key-save-delay 0
            $primary config set dual-channel-replication-enabled no
            $primary config set repl-compression no
        }
    }

    set dual_transition_cases {
        {no lz4 lz4}
        {lz4 no lz4}
    }
    if {$::replcompression_zstd_supported} {
        lappend dual_transition_cases \
            {no zstd zstd} \
            {zstd lz4 zstd}
    }
    foreach transition_case $dual_transition_cases {
        lassign $transition_case initial final replica_mode
        test "Dual-channel load converges after primary repl-compression $initial->$final with $replica_mode replica" {
            set initial_compressed [expr {
                $initial ne "no" &&
                ($initial eq $replica_mode || ($initial eq "lz4" && $replica_mode eq "zstd"))
            }]
            set final_algo [expr {
                $final eq $replica_mode || ($final eq "lz4" && $replica_mode eq "zstd") ? $final : "no"
            }]
            set final_compressed [expr {$final_algo ne "no"}]
            $primary config set repl-compression $initial
            $primary config set dual-channel-replication-enabled yes
            $primary config set rdb-key-save-delay 100
            $primary flushall
            $primary debug populate 10000 midload: 100

            start_server [list overrides [list save "" repl-compression $replica_mode dual-channel-replication-enabled yes]] {
                set replica [srv 0 client]
                $replica replicaof $primary_host $primary_port

                wait_for_condition 500 10 {
                    [s 0 master_sync_in_progress] == 1 &&
                    [string match {*state=bg_transfer*} [$primary info replication]] &&
                    [regexp -all {repl_compression=(lz4|zstd)} [$primary info replication]] == $initial_compressed
                } else {
                    fail "Dual-channel $initial stream did not reach the load window"
                }

                # The command-stream decision is fixed at +CONTINUE. Traffic
                # buffered before the flip must survive cron reconciliation.
                for {set i 0} {$i < 20} {incr i} {
                    $primary set "during_load:$i" "value_$i"
                }
                set full_before [status $primary sync_full]
                set partial_before [status $primary sync_partial_ok]
                $primary config set repl-compression $final
                assert_equal 1 [s 0 master_sync_in_progress]
                assert_match {*state=bg_transfer*} [$primary info replication]
                assert_equal $initial_compressed \
                    [regexp -all {repl_compression=(lz4|zstd)} [$primary info replication]]

                wait_for_condition 100 100 {
                    [status $primary sync_partial_ok] == $partial_before + 1 &&
                    [s 0 master_link_status] eq {up} &&
                    [regexp -all {repl_compression=(lz4|zstd)} [$primary info replication]] == $final_compressed
                } else {
                    fail "Dual-channel link did not renegotiate to $final"
                }
                if {$final_compressed} {
                    assert_equal 1 [regexp -all "repl_compression=$final_algo" [$primary info replication]]
                }
                assert_equal $full_before [status $primary sync_full]

                for {set i 0} {$i < 20} {incr i} {
                    assert_equal "value_$i" [$replica get "during_load:$i"]
                }
                $primary set midload_probe delivered
                wait_for_condition 50 100 {
                    [$replica get midload_probe] eq {delivered}
                } else {
                    fail "Post-renegotiation write did not reach the replica"
                }
                assert_equal [expr {$partial_before + 1}] [status $primary sync_partial_ok]
                assert_equal $full_before [status $primary sync_full]

                $replica replicaof no one
            }
            $primary config set rdb-key-save-delay 0
            $primary config set dual-channel-replication-enabled no
            $primary config set repl-compression no
        }
    }

}

# ============================================================
# Multi-replica compressed replication tests
# ============================================================

# Multiple replicas distribute across threads and stay in sync.
# io-threads-always-active starts off during the handshakes: an offloaded
# REPLCONF reply can leave pending output that makes the primary reject PSYNC,
# and the legacy-SYNC fallback suppresses the ACK that diskless sync waits for
# (upstream race). It is enabled once all replicas are streaming.
start_server {tags {"repl"} overrides {save "" io-threads 4 repl-compression lz4}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    test {Multiple replicas all stay in sync under load} {
        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set replica1 [srv 0 client]
            $replica1 replicaof $primary_host $primary_port
            wait_for_sync $replica1

            start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
                set replica2 [srv 0 client]
                $replica2 replicaof $primary_host $primary_port
                wait_for_sync $replica2

                start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
                    set replica3 [srv 0 client]
                    $replica3 replicaof $primary_host $primary_port
                    wait_for_sync $replica3

                    wait_for_condition 50 200 {
                        [regexp -all "repl_compression=lz4" [$primary info replication]] >= 3
                    } else {
                        fail "Not all replicas have compression active"
                    }

                    # Handshakes are done; run the load phase on IO threads.
                    $primary config set io-threads-always-active yes

                    for {set i 0} {$i < 500} {incr i} {
                        $primary set "multi_repl:$i" [string repeat "x" 100]
                    }

                    wait_for_condition 100 200 {
                        [$replica1 dbsize] == [$primary dbsize] &&
                        [$replica2 dbsize] == [$primary dbsize] &&
                        [$replica3 dbsize] == [$primary dbsize]
                    } else {
                        fail "Not all replicas caught up: r1=[$replica1 dbsize] r2=[$replica2 dbsize] r3=[$replica3 dbsize] primary=[$primary dbsize]"
                    }

                    set primary_digest [$primary debug digest]
                    assert_equal $primary_digest [$replica1 debug digest]
                    assert_equal $primary_digest [$replica2 debug digest]
                    assert_equal $primary_digest [$replica3 debug digest]

                    $replica3 replicaof no one
                }
                $replica2 replicaof no one
            }
            $replica1 replicaof no one
        }
    }
}

# Chained replication: each hop negotiates compression independently, and the
# middle node simultaneously decodes its primary link on the main thread while
# encoding for its own replica on IO threads.
start_server {tags {"repl"} overrides {save "" repl-compression lz4}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    test {Chained replication compresses each hop independently} {
        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb io-threads 4 io-threads-always-active yes}} {
            set middle [srv 0 client]
            set middle_host [srv 0 host]
            set middle_port [srv 0 port]

            start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
                set leaf [srv 0 client]

                $middle replicaof $primary_host $primary_port
                wait_for_sync $middle
                $leaf replicaof $middle_host $middle_port
                wait_for_sync $leaf

                # Both hops negotiated compression.
                wait_for_condition 100 200 {
                    [regexp -all "repl_compression=lz4" [$primary info replication]] >= 1 &&
                    [regexp -all "repl_compression=lz4" [$middle info replication]] >= 1
                } else {
                    fail "Compression not active on both hops"
                }

                # Writes flow primary -> middle -> leaf across two compressed hops.
                for {set i 0} {$i < 200} {incr i} {
                    $primary set "chain:$i" "chain_value_$i"
                }
                wait_for_condition 100 200 {
                    [$leaf dbsize] == [$primary dbsize]
                } else {
                    fail "Leaf did not catch up: leaf=[$leaf dbsize] primary=[$primary dbsize]"
                }
                assert_equal "chain_value_0" [$leaf get chain:0]
                assert_equal "chain_value_99" [$leaf get chain:99]
                assert_equal "chain_value_199" [$leaf get chain:199]

                # The leaf flip automatically renegotiates only the second hop.
                set full_before [status $middle sync_full]
                set partial_before [status $middle sync_partial_ok]
                $leaf config set repl-compression no
                wait_for_condition 100 200 {
                    [status $leaf master_link_status] eq {up} &&
                    [regexp -all {repl_compression=lz4} [$middle info replication]] == 0 &&
                    [status $middle sync_partial_ok] == $partial_before + 1
                } else {
                    fail "Hop2 did not renegotiate to plaintext"
                }
                assert_equal $full_before [status $middle sync_full]
                assert {[regexp -all {repl_compression=lz4} [$primary info replication]] >= 1}

                # Data still flows end-to-end over mixed hops.
                $primary set chain:final final_val
                wait_for_condition 100 200 {
                    [$leaf get chain:final] eq {final_val}
                } else {
                    fail "Write did not reach leaf after hop2 renegotiated plaintext"
                }

                $leaf replicaof no one
            }
            $middle replicaof no one
        }
    }
}


}

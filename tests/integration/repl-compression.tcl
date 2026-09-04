tags {"repl external:skip"} {

# uncompressed_bytes= from the replica line of the primary's INFO replication.
proc replica_line_uncompressed_bytes {primary} {
    set info [$primary info replication]
    assert {[regexp {uncompressed_bytes=([0-9]+)} $info -> ub]}
    return $ub
}

# ============================================================
# Config CRUD — single-server tests, no replication needed
# ============================================================

start_server {overrides {save "" repl-compression no}} {

    test {repl-compression config: default, set, and survives CONFIG REWRITE and restart} {
        assert_equal "no" [lindex [r config get repl-compression] 1]

        r config set repl-compression yes
        assert_equal "yes" [lindex [r config get repl-compression] 1]
        r config set repl-compression lz4
        assert_equal "lz4" [lindex [r config get repl-compression] 1]
        r config rewrite

        restart_server 0 true false

        assert_equal "lz4" [lindex [r config get repl-compression] 1]

        r config set repl-compression no
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
    foreach {compression_mode diskless_load} {
        lz4 swapdb
        lz4 disabled
        yes swapdb
    } {
        test "Replica negotiates $compression_mode compression (repl-diskless-load $diskless_load)" {
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

                    # Full sync completes, then compression activates for the
                    # post-sync incremental stream (the full-sync RDB itself is
                    # never compressed by this capability).
                    wait_for_condition 50 100 {
                        [regexp -all "compression=lz4" [$primary info replication]] >= 1
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

    test {Compressed replication handles compressible and incompressible values across batch boundaries} {
        $primary config set repl-compression lz4
        $primary flushall

        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 200 {
                [s 0 master_link_status] eq {up} &&
                [string match {*state=online*compression=lz4*} [$primary info replication]]
            } else {
                fail "Compressed replication not established"
            }

            # The compressible value spans several 1 MiB raw batches.
            set bigval [string repeat "abcdefghij0123456789" 209715]
            $primary set batch:compressible $bigval
            wait_for_condition 50 200 {
                [$replica get batch:compressible] eq $bigval
            } else {
                fail "Compressible value did not replicate intact"
            }

            # A deterministic ratio≈1 payload exercises worst-case compressed
            # output sizing across multiple raw batches.
            expr {srand(424242)}
            set payload ""
            while {[string length $payload] < 1572864} {
                set chunk ""
                for {set i 0} {$i < 4096} {incr i} {
                    append chunk [format %c [expr {int(rand()*256)}]]
                }
                append payload $chunk
            }
            $primary set batch:incompressible $payload
            wait_for_condition 50 200 {
                [$replica get batch:incompressible] eq $payload
            } else {
                fail "Incompressible value did not replicate intact"
            }

            $replica replicaof no one
        }
        $primary config set repl-compression no
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
                [string match {*state=online*compression=lz4*} [$primary info replication]]
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
                set prev [replica_line_uncompressed_bytes $primary]
                set stable_samples 0
                for {set i 0} {$i < 100 && $stable_samples < 3} {incr i} {
                    after 100
                    set cur [replica_line_uncompressed_bytes $primary]
                    if {$cur == $prev} {
                        incr stable_samples
                    } else {
                        set stable_samples 0
                    }
                    set prev $cur
                }
                assert_equal 3 $stable_samples

                # Frozen cursor: two samples with writes in between must be equal.
                set ub1 [replica_line_uncompressed_bytes $primary]
                set off1 [status $primary master_repl_offset]
                for {set i 0} {$i < 20} {incr i} {
                    $primary set "pin:tick:$i" tick_val
                }
                after 300
                set ub2 [replica_line_uncompressed_bytes $primary]
                set off2 [status $primary master_repl_offset]

                assert {$off2 > $off1}
                assert_equal $ub1 $ub2
            } pause_result pause_opts]
            resume_process $replica_pid
            if {$pause_code} {
                return -options $pause_opts $pause_result
            }

            # Pinned batches drain after the replica resumes.
            wait_for_ofs_sync $primary $replica
            assert {[$replica get pin:burst:199] eq $payload}
            assert_equal tick_val [$replica get pin:tick:19]
            assert {[replica_line_uncompressed_bytes $primary] > $ub2}
            assert_equal $sync_full_before [status $primary sync_full]
            assert_equal $sync_partial_before [status $primary sync_partial_ok]

            $replica replicaof no one
        }

        $primary config set repl-compression no
    }

    test {Compressed partial resync preserves data and decoded ACK offsets} {
        $primary config set repl-compression lz4
        $primary flushall

        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 200 {
                [s 0 master_link_status] eq {up} &&
                [string match {*state=online*compression=lz4*} [$primary info replication]]
            } else {
                fail "Compressed replication not established"
            }

            $primary set key1 value1
            wait_for_condition 50 100 {
                [$replica get key1] eq {value1}
            } else {
                fail "Initial replication failed"
            }

            # Killing the primary-side connection preserves the replica's
            # cached offset and exercises a compressed partial resync.
            set full_before [status $primary sync_full]
            set partial_before [status $primary sync_partial_ok]
            $primary client kill type replica

            wait_for_condition 50 200 {
                [s 0 master_link_status] eq {up} &&
                [string match {*state=online*compression=lz4*} [$primary info replication]] &&
                [status $primary sync_partial_ok] == $partial_before + 1
            } else {
                fail "Compressed partial resync did not complete"
            }
            assert_equal $full_before [status $primary sync_full]

            # WAIT compares logical RESP offsets. A highly compressible write
            # only reaches the target if the replica advances by decoded bytes,
            # not by the much smaller number of wire bytes.
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

    test {Compressed replication survives a mid-frame disconnect and reconverges} {
        # The partial-resync test above cuts the link cleanly between frames.
        # This one cuts it WHILE a compressed frame is on the wire: the replica
        # must handle the truncated frame without crashing, reconnect, and end
        # up byte-identical to the primary.
        $primary config set repl-compression lz4
        $primary flushall

        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 200 {
                [s 0 master_link_status] eq {up} &&
                [string match {*state=online*compression=lz4*} [$primary info replication]]
            } else {
                fail "Compressed replication not established"
            }

            # Baseline sync so recovery is a clean reconverge from a known point.
            $primary set trunc:baseline baseline_val
            wait_for_ofs_sync $primary $replica
            set full_before [status $primary sync_full]
            set partial_before [status $primary sync_partial_ok]

            # A repeated-character value spans several 1 MiB raw batches, so a
            # single write is itself a multi-batch compressed frame. Bursting
            # them keeps the primary's output buffer backed up (loopback send
            # buffer is far smaller than the queued compressed bytes), so a
            # frame is genuinely mid-write when we cut the link.
            set bigval [string repeat "abcdefghij0123456789" 52429] ;# ~1 MiB
            for {set i 0} {$i < 60} {incr i} {
                $primary set "trunc:load:$i" $bigval
                if {$i == 30} {
                    # Mid-burst kill from the PRIMARY side: compressed output is
                    # still draining to the replica, truncating a frame in flight.
                    $primary client kill type replica
                }
            }

            # The truncated link forces the replica to reconnect and resync
            # (partial or full, either is fine) and re-establish compression.
            $primary set trunc:final final_val
            wait_for_condition 100 200 {
                [s 0 master_link_status] eq {up} &&
                [string match {*state=online*compression=lz4*} [$primary info replication]] &&
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

            # The link remained plaintext; no bytes entered the decompressor.
            assert_equal 0 [string match {*compression=lz4*} [$primary info replication]]
            assert_equal 0 [s 0 total_repl_decompressed_bytes]

            $replica replicaof no one
        }
    }

    test {Replica repl-compression flips renegotiate upstream in both directions} {
        $primary config set repl-compression lz4

        start_server {overrides {save "" repl-compression lz4 repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 200 {
                [s 0 master_link_status] eq {up} &&
                [regexp -all {compression=lz4} [$primary info replication]] == 1
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
            assert_equal 1 [regexp -all {compression=lz4} [$primary info replication]]

            # Returning to the advertised state before cron runs does not
            # require renegotiating the existing link.
            $replica debug pause-cron 1
            try {
                $replica config set repl-compression no
                $replica config set repl-compression lz4
            } finally {
                $replica debug pause-cron 0
            }
            after 1500
            assert_equal $partial_before [status $primary sync_partial_ok]
            assert_equal 1 [regexp -all {compression=lz4} [$primary info replication]]

            set transition 0
            foreach {mode expected_compressed} {no 0 lz4 1} {
                incr transition
                $replica config set repl-compression $mode
                wait_for_condition 50 200 {
                    [s 0 master_link_status] eq {up} &&
                    [regexp -all {compression=lz4} [$primary info replication]] == $expected_compressed &&
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

                assert_equal 0 [regexp -all {compression=lz4} [$primary info replication]]
                set full_before [status $primary sync_full]
                set partial_before [status $primary sync_partial_ok]

                # Only the capable replica renegotiates when the primary enables
                # compression; the opted-out replica remains on its link.
                $primary config set repl-compression lz4
                wait_for_condition 50 200 {
                    [s 0 master_link_status] eq {up} &&
                    [status $compressed_replica master_link_status] eq {up} &&
                    [regexp -all {compression=lz4} [$primary info replication]] == 1 &&
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
                    [regexp -all {compression=lz4} [$primary info replication]] == 0 &&
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
                [string match {*state=bg_transfer*compression=lz4*} [$primary info replication]]
            } else {
                fail "Dual-channel sync did not start"
            }

            # Writes made during load reach the replica via the compressed main
            # channel: +CONTINUE starts compression, put-online must not restart
            # it (a second init would emit a new envelope mid-frame).
            for {set i 0} {$i < 200} {incr i} {
                $primary set "during_load:$i" "value_$i"
            }
            assert_equal 1 [s 0 master_sync_in_progress]
            assert_match {*state=bg_transfer*compression=lz4*} [$primary info replication]

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
            assert_match "*compression=lz4*" [$primary info replication]
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

    foreach {initial final expected_compressed} {no lz4 1 lz4 no 0} {
        test "Dual-channel load converges after primary repl-compression $initial->$final" {
            $primary config set repl-compression $initial
            $primary config set dual-channel-replication-enabled yes
            $primary config set rdb-key-save-delay 100
            $primary flushall
            $primary debug populate 10000 midload: 100

            start_server {overrides {save "" repl-compression lz4 dual-channel-replication-enabled yes}} {
                set replica [srv 0 client]
                $replica replicaof $primary_host $primary_port

                wait_for_condition 500 10 {
                    [s 0 master_sync_in_progress] == 1 &&
                    [string match {*state=bg_transfer*} [$primary info replication]] &&
                    [regexp -all {compression=lz4} [$primary info replication]] == [expr {$initial eq "lz4"}]
                } else {
                    fail "Dual-channel $initial stream did not reach the load window"
                }

                # The command-stream decision is fixed at +CONTINUE. Traffic
                # buffered before the flip must survive the put-online resync.
                for {set i 0} {$i < 20} {incr i} {
                    $primary set "during_load:$i" "value_$i"
                }
                set full_before [status $primary sync_full]
                set partial_before [status $primary sync_partial_ok]
                $primary config set repl-compression $final
                assert_equal 1 [s 0 master_sync_in_progress]
                assert_match {*state=bg_transfer*} [$primary info replication]
                assert_equal [expr {$initial eq "lz4"}] \
                    [regexp -all {compression=lz4} [$primary info replication]]

                wait_for_condition 100 100 {
                    [status $primary sync_partial_ok] == $partial_before + 1 &&
                    [s 0 master_link_status] eq {up} &&
                    [regexp -all {compression=lz4} [$primary info replication]] == $expected_compressed
                } else {
                    fail "Dual-channel link did not renegotiate to $final"
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
                        [regexp -all "compression=lz4" [$primary info replication]] >= 3
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
                    [regexp -all "compression=lz4" [$primary info replication]] >= 1 &&
                    [regexp -all "compression=lz4" [$middle info replication]] >= 1
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
                    [regexp -all {compression=lz4} [$middle info replication]] == 0 &&
                    [status $middle sync_partial_ok] == $partial_before + 1
                } else {
                    fail "Hop2 did not renegotiate to plaintext"
                }
                assert_equal $full_before [status $middle sync_full]
                assert {[regexp -all {compression=lz4} [$primary info replication]] >= 1}

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

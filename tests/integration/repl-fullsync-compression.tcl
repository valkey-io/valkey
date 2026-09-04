# Full-sync streaming compression: end-to-end coverage across disk-based,
# diskless, and dual-channel full sync, both replica load paths, negative cases
# (truncation, corruption, premature EOF, non-capable cohorts), and byte
# accounting. Asymmetric policy: an all-capable group gets a compressed round; any
# non-capable member forces one plaintext round for all.

# --- helpers --------------------------------------------------------------

proc rdb_is_compressed {client} {
    set header [read_binary_file_prefix [server_rdb_path $client] 3]
    return [expr {$header eq "VCS"}]
}

# Compressible, multi-type dataset so an LZ4 frame is actually produced.
proc populate_compressible_dataset {client prefix {n 300}} {
    $client flushall
    for {set i 0} {$i < $n} {incr i} {
        $client set "${prefix}:str:$i" [string repeat "${prefix}:payload:$i " 16]
    }
    $client rpush "${prefix}:list" a b c d e f g h
    $client sadd "${prefix}:set" alpha beta gamma delta
    $client zadd "${prefix}:zset" 1 one 2 two 3 three 4 four
    $client hset "${prefix}:hash" f1 v1 f2 [string repeat "${prefix}:hashval " 8]
    $client xadd "${prefix}:stream" * f1 s1 f2 [string repeat "${prefix}:streamval " 4]
    $client xadd "${prefix}:stream" * f1 s2 f2 tail
}

# Wait for the link up, then assert digests match. Generous 300x100 budget for
# slow/shared primaries and the macOS timing fix for the delay-based piggyback tests.
proc assert_replica_synced {primary replica {tag ""}} {
    wait_for_condition 300 100 {
        [status $replica master_link_status] eq "up"
    } else {
        fail "replica link not up after full sync $tag"
    }
    assert_equal [$primary debug digest] [$replica debug digest] "replica digest mismatch after full sync $tag"
}

# Single-shot fake primary (tests/helpers/fake_primary.tcl): answers the handshake,
# announces $announce bytes, sends $payload, closes. Returns {pid port}.
proc start_fake_primary {payload announce} {
    set payload_file [tmpfile fake-primary-payload]
    write_binary_file $payload_file $payload
    set port [find_available_port $::baseport $::portcount]
    set pid [exec [info nameofexecutable] tests/helpers/fake_primary.tcl $port $payload_file $announce &]
    wait_for_condition 50 50 {
        [ping_server 127.0.0.1 $port]
    } else {
        fail "Failed to start fake primary"
    }
    return [list $pid $port]
}

# Park both replicas in WAIT_BGSAVE_START together: a slow manual BGSAVE
# occupies the RDB child slot (syncCommand defers to replicationCron), so the
# following grouped round makes its group-AND capability decision over both
# waiters deterministically. Resetting the delay lets that round run fast.
proc park_replicas_for_grouped_bgsave {primary replica1 replica2 primary_host primary_port} {
    $primary config set rdb-key-save-delay 5000
    assert_match {*Background saving started*} [$primary bgsave]
    wait_for_condition 200 10 {
        [status $primary rdb_bgsave_in_progress] eq 1
    } else {
        $primary config set rdb-key-save-delay 0
        fail "manual BGSAVE did not start"
    }

    $replica1 replicaof $primary_host $primary_port
    $replica2 replicaof $primary_host $primary_port
    wait_for_condition 200 10 {
        [status $primary connected_slaves] == 2 &&
        [status $primary rdb_bgsave_in_progress] eq 1
    } else {
        $primary config set rdb-key-save-delay 0
        fail "both replicas did not register before manual BGSAVE finished (grouping not achieved)"
    }

    $primary config set rdb-key-save-delay 0
}

# ============================================================================
# Disk-based full sync: ONE shared primary fixture. rdbcompression is MODIFIABLE,
# so each test sets its mode and repopulates its own prefix; the compression-off
# test restores the fixture default (lz4) at its end for later shared tests.
# Replicas are per-test nested start_servers because their capability configs differ.
# ============================================================================

start_server {tags {"repl rdb-compression external:skip needs:debug"} overrides {save "" enable-debug-command local}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    $primary config set repl-diskless-sync no
    $primary config set rdbcompression lz4
    $primary config set rdb-del-sync-files no

    start_server {overrides {save "" enable-debug-command local rdbcompression lz4}} {
        set replica [srv 0 client]

        test {Disk full sync: all-capable group produces a compressed RDB that loads} {
            $primary config set rdbcompression lz4
            populate_compressible_dataset $primary "cap"
            set primary_loglines [count_log_lines -1]

            $replica replicaof $primary_host $primary_port
            assert_replica_synced $primary $replica "(case1 capable)"

            assert {[file exists [server_rdb_path $primary]]}
            assert_equal 1 [rdb_is_compressed $primary]
            wait_for_log_messages -1 {"*Disk-based full sync with compression: lz4*"} $primary_loglines 50 100

            $primary set cap:post "after-sync"
            wait_for_value_to_propagate_to_replica $primary $replica cap:post

            $replica replicaof no one
        }
    }

    # Both scenarios fall back to plaintext: the replica advertises compress-sync
    # per its own rdbcompression, and the primary's mode is set per iteration.
    start_server {overrides {save "" enable-debug-command local rdbcompression lz4}} {
        set replica [srv 0 client]

        foreach {scenario primary_mode replica_mode prefix} {
            primary-compression-off yes lz4 off
            replica-not-capable     lz4 yes nocap
        } {
            test "Disk full sync: $scenario yields a plaintext RDB that loads" {
                $primary config set rdbcompression $primary_mode
                $replica config set rdbcompression $replica_mode
                populate_compressible_dataset $primary $prefix

                $replica replicaof $primary_host $primary_port
                assert_replica_synced $primary $replica "($scenario)"

                assert_equal 0 [rdb_is_compressed $primary]

                $replica replicaof no one
            }
        }
        # Restore the fixture default for subsequent shared-primary tests.
        $primary config set rdbcompression lz4
    }

    # Regression: a size-framed ($<len>, no EOF mark) compressed disk RDB, loaded
    # directly from the socket via replicaLoadPrimaryRDBFromSocket. Decompression
    # was once gated on the EOF mark (usemark), so the VCS frame was read as a raw
    # RDB ("Wrong signature ... VCS"), causing an infinite resync loop.
    start_server {overrides {save "" enable-debug-command local rdbcompression lz4 repl-diskless-load swapdb}} {
        set replica [srv 0 client]

        test {Disk-based compressed full sync is decompressed on a diskless-load (swapdb) replica} {
            $primary config set rdbcompression lz4
            populate_compressible_dataset $primary "diskmaster"
            set replica_loglines [count_log_lines 0]

            $replica replicaof $primary_host $primary_port
            assert_replica_synced $primary $replica "(disk-master, diskless-load swapdb)"

            assert_equal 1 [rdb_is_compressed $primary]
            # Socket decode path logs "from primary"; match it so a file load cannot satisfy this.
            wait_for_log_messages 0 {"*Loading compressed RDB (algo=lz4) from primary*"} $replica_loglines 50 100

            $primary set diskmaster:post "after-sync"
            wait_for_value_to_propagate_to_replica $primary $replica diskmaster:post

            $replica replicaof no one
        }
    }
}

# Mixed disk group: force both replicas to park in WAIT_BGSAVE_START together so
# the group-AND decision is exercised directly. A slow manual BGSAVE occupies the
# RDB child slot; a disk replica can't start its own BGSAVE (syncCommand defers to
# replicationCron), so both park. The cron then groups both waiters and the AND
# clears the compress-sync bit. Deterministic because the manual BGSAVE is still
# running when both register (asserted via connected_slaves==2); one round is
# asserted via the "Starting BGSAVE for SYNC" delta.

start_server {tags {"repl rdb-compression external:skip needs:debug"} overrides {save "" enable-debug-command local}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    $primary config set repl-diskless-sync no
    $primary config set rdbcompression lz4
    $primary config set rdb-del-sync-files no
    # Enough keys so the manual BGSAVE stays alive for both replicas to register.
    populate_compressible_dataset $primary "grp" 800

    start_server {overrides {save "" enable-debug-command local rdbcompression lz4}} {
        set capable [srv 0 client]

        start_server {overrides {save "" enable-debug-command local rdbcompression yes}} {
            set noncap [srv 0 client]

            test {Disk full sync: a single grouped BGSAVE with a non-capable member is plaintext for all} {
                # One round == one "Starting BGSAVE for SYNC" in the primary log.
                # srv -2 is the primary inside this doubly-nested scope.
                set rounds_before [count_log_message -2 {Starting BGSAVE for SYNC}]
                set primary_loglines [count_log_lines -2]

                park_replicas_for_grouped_bgsave $primary $capable $noncap $primary_host $primary_port

                assert_replica_synced $primary $capable "(case3b capable, grouped)"
                assert_replica_synced $primary $noncap   "(case3b non-capable, grouped)"

                # Exactly one BGSAVE served both -> they were grouped (AND precondition).
                assert_equal 1 [expr {[count_log_message -2 {Starting BGSAVE for SYNC}] - $rounds_before}]

                # AND result: the grouped RDB is plaintext (capable replica downgraded too).
                assert_equal 0 [rdb_is_compressed $primary]
                verify_no_log_message -2 "*Disk-based full sync with compression: lz4*" $primary_loglines

                $noncap replicaof no one
                $capable replicaof no one
            }
        }
    }
}

# ============================================================================
# Piggyback (in-flight BGSAVE join): all four quadrants of (running save
# format) x (joiner capability). A joiner attaches whenever it can load the
# running save's format; a non-capable joiner must NOT attach to a compressed
# save and waits for the next, plaintext, save. Capability is advertised from
# rdbcompression at handshake time, so one shared replica pair serves every
# row via CONFIG SET. rdb-key-save-delay 13000 (~2s over 155 keys x 13ms: above
# one PSYNC round trip, under wait_for_sync's ~5s budget on slow runners) keeps
# the save in flight while the joiner arrives.
# ============================================================================

start_server {tags {"repl rdb-compression external:skip needs:debug"} overrides {save "" enable-debug-command local}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    $primary config set repl-diskless-sync no
    $primary config set rdb-del-sync-files no

    start_server {overrides {save "" enable-debug-command local rdbcompression lz4}} {
        set trigger [srv 0 client]

        start_server {overrides {save "" enable-debug-command local rdbcompression lz4}} {
            set joiner [srv 0 client]

            foreach {name primary_mode joiner_mode should_join expected_compressed} {
                "plain save, non-capable joiner attaches"                   yes yes 1 0
                "plain save, capable joiner attaches"                       yes lz4 1 0
                "compressed save, non-capable joiner waits for a new save"  lz4 yes 0 0
                "compressed save, capable joiner attaches"                  lz4 lz4 1 1
            } {
                test "Piggyback: $name" {
                    with_cleanup {
                        $primary config set rdbcompression $primary_mode
                        $joiner config set rdbcompression $joiner_mode
                        $primary config set rdb-key-save-delay 13000
                        populate_compressible_dataset $primary piggy 150
                        set primary_loglines [count_log_lines -2]

                        $trigger replicaof $primary_host $primary_port
                        if {$primary_mode eq "lz4"} {
                            wait_for_log_messages -2 {"*Disk-based full sync with compression: lz4*"} \
                                $primary_loglines 50 100
                        } else {
                            wait_for_log_messages -2 {"*Starting BGSAVE for SYNC with target: disk*"} \
                                $primary_loglines 50 100
                        }

                        $joiner replicaof $primary_host $primary_port
                        if {$should_join} {
                            wait_for_log_messages -2 {"*Waiting for end of BGSAVE for SYNC*"} \
                                $primary_loglines 50 100
                            # Prove the joiner piggybacked onto the running save rather than
                            # triggering a second BGSAVE round: the primary is -2 in this
                            # doubly-nested start_server scope, so neither the attach-refusal
                            # nor the next-save-wait message must appear.
                            verify_no_log_message -2 "*Can't attach the replica to the current BGSAVE*" $primary_loglines
                            verify_no_log_message -2 "*Waiting for next BGSAVE for SYNC*" $primary_loglines
                        } else {
                            wait_for_log_messages -2 {"*Can't attach the replica to the current BGSAVE*"} \
                                $primary_loglines 50 100
                        }

                        $primary config set rdb-key-save-delay 0
                        assert_replica_synced $primary $trigger "($name, trigger)"
                        assert_replica_synced $primary $joiner "($name, joiner)"
                        assert_equal $expected_compressed [rdb_is_compressed $primary]
                    } {
                        $primary config set rdb-key-save-delay 0
                        $joiner replicaof no one
                        $trigger replicaof no one
                    }
                }
            }
        }
    }
}

# ============================================================================
# Cross-cutting transfer-path coverage: disk-receive (BIO) load, dual-channel,
# diskless payload compression, and byte-accounting. Needs rdbcompression lz4.
# ============================================================================

tags {"repl external:skip"} {

# --- dual-channel: ONE shared primary fixture ------------------------------
# The byte-accounting test raises repl-diskless-sync-delay and
# repl-diskless-sync-max-replicas (both MODIFIABLE) at its start and restores them
# at its end so the shared primary stays neutral for the other tests.

start_server {overrides {save "" rdbcompression lz4 repl-diskless-sync yes repl-diskless-sync-delay 0 dual-channel-replication-enabled yes}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    # Both load paths under dual-channel: disabled -> RDB to disk (BIO), rdbLoad()
    # decompresses; swapdb -> load compressed payload from the socket (also exercises
    # capa-compression advertising in the dual-channel handshake).
    foreach load_mode {disabled swapdb} {
        test "Dual-channel + compression full sync delivers identical data (repl-diskless-load $load_mode)" {
            set primary_loglines [count_log_lines 0]
            populate_compressible_dataset $primary "dc-$load_mode"

            start_server [list overrides [list save "" rdbcompression lz4 repl-diskless-load $load_mode dual-channel-replication-enabled yes]] {
                set replica [srv 0 client]
                set replica_loglines [count_log_lines 0]
                $replica replicaof $primary_host $primary_port

                assert_replica_synced $primary $replica "(dual-channel $load_mode)"

                wait_for_log_messages -1 {"*using: dual-channel*"} $primary_loglines 50 100
                wait_for_log_messages -1 {"*Diskless full sync with compression: lz4*"} $primary_loglines 50 100
                # swapdb loads inline from the socket ("from primary"); disabled loads from the received file.
                if {$load_mode eq "swapdb"} {
                    wait_for_log_messages 0 {"*Loading compressed RDB (algo=lz4) from primary*"} $replica_loglines 50 100
                } else {
                    wait_for_log_messages 0 {"*Loading compressed RDB (algo=lz4) from *.rdb*"} $replica_loglines 50 100
                }

                $replica replicaof no one
            }
        }
    }

    # Aggregate output accounting: one BGSAVE serving TWO dual-channel replicas
    # writes the payload to both sockets from a single connset, so
    # total_net_repl_output_bytes must cover both. Old single-stream accounting
    # reported ~half and fails the >=75% check below. Needs the delay window + 2-replica cap (restored at the end).
    test {Dual-channel diskless full sync counts output bytes across all sockets} {
        $primary config set repl-diskless-sync-delay 1000
        $primary config set repl-diskless-sync-max-replicas 2
        populate_compressible_dataset $primary agg 2000

        start_server {overrides {save "" rdbcompression lz4 repl-diskless-load swapdb dual-channel-replication-enabled yes}} {
            set replica1 [srv 0 client]

            start_server {overrides {save "" rdbcompression lz4 repl-diskless-load swapdb dual-channel-replication-enabled yes}} {
                set replica2 [srv 0 client]

                # Attach both within the delay window so one BGSAVE groups both channels into one connset.
                set rounds_before [count_log_message -2 {Starting BGSAVE for SYNC}]
                $primary config resetstat
                $replica1 replicaof $primary_host $primary_port
                $replica2 replicaof $primary_host $primary_port

                assert_replica_synced $primary $replica1 "(agg r1)"
                assert_replica_synced $primary $replica2 "(agg r2)"

                # One grouped BGSAVE served both: precondition for the aggregation to be exercised.
                assert_equal 1 [expr {[count_log_message -2 {Starting BGSAVE for SYNC}] - $rounds_before}]

                # The child reports per-connset output bytes over the child-info pipe,
                # which the parent drains asynchronously after link-up. Wait for that
                # to land before sampling the counters.
                wait_for_condition 50 100 {
                    [status $primary total_net_repl_output_bytes] > 0
                } else {
                    fail "primary did not aggregate dual-channel output bytes"
                }

                set out [status $primary total_net_repl_output_bytes]
                set in1 [status $replica1 total_net_repl_input_bytes]
                set in2 [status $replica2 total_net_repl_input_bytes]
                set total_in [expr {$in1 + $in2}]

                assert_morethan $in1 0
                assert_morethan $in2 0
                # Lower bound catches old single-stream (~half) accounting; upper bound guards double-counting.
                assert {$out >= $total_in * 0.75}
                assert {$out <= $total_in * 1.25}

                $replica2 replicaof no one
                $replica1 replicaof no one
            }
        }

        # Restore the fixture defaults for the shared dual-channel primary.
        $primary config set repl-diskless-sync-delay 0
        $primary config set repl-diskless-sync-max-replicas 0
    }
}

# --- ordinary diskless: ONE shared primary fixture -------------------------
# Shared by the receive-to-disk (disabled) and direct-socket-load (swapdb) tests;
# replicas differ only in repl-diskless-load so they stay per-test.

start_server {overrides {save "" rdbcompression lz4 repl-diskless-sync yes repl-diskless-sync-delay 0}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    # disabled: replica copies the payload to a temp file, then rdbLoad() auto-decompresses.
    test {Disk-receive (repl-diskless-load disabled) + diskless compressed save loads correctly} {
        set primary_loglines [count_log_lines 0]
        populate_compressible_dataset $primary "diskrecv"

        start_server {overrides {save "" rdbcompression lz4 repl-diskless-load disabled}} {
            set replica [srv 0 client]
            set replica_loglines [count_log_lines 0]
            $replica replicaof $primary_host $primary_port

            assert_replica_synced $primary $replica "(disk-receive default load)"

            wait_for_log_messages -1 {"*target: replicas sockets*"} $primary_loglines 50 100
            wait_for_log_messages -1 {"*Diskless full sync with compression: lz4*"} $primary_loglines 50 100
            # Path B: disabled load reads from the received file; match "from <file>.rdb", not "from primary".
            wait_for_log_messages 0 {"*Loading compressed RDB (algo=lz4) from *.rdb*"} $replica_loglines 50 100

            $replica replicaof no one
        }
    }

    # Diskless compresses the RDB payload as an LZ4 VCS frame over the socket; the
    # $EOF:<mark> framing stays uncompressed so transfer boundaries are unchanged.
    test {Diskless full sync compresses the RDB payload for a capable replica} {
        set primary_loglines [count_log_lines 0]
        populate_compressible_dataset $primary diskless-full-sync

        start_server {overrides {save "" rdbcompression lz4 repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            set replica_loglines [count_log_lines 0]
            $replica replicaof $primary_host $primary_port

            assert_replica_synced $primary $replica "(compressed diskless full sync)"

            wait_for_log_messages -1 {"*target: replicas sockets*"} $primary_loglines 50 100
            wait_for_log_messages -1 {"*Diskless full sync with compression: lz4*"} $primary_loglines 50 100
            wait_for_log_messages 0 {"*Loading compressed RDB (algo=lz4) from primary*"} $replica_loglines 50 100

            $primary set diskless-full-sync:post after
            wait_for_value_to_propagate_to_replica $primary $replica diskless-full-sync:post

            $replica replicaof no one
        }
    }

}

# Checksum interaction: a compressed diskless full sync loads under rdbchecksum no.
# Isolated because rdbchecksum is an immutable startup-only override.

start_server {overrides {save "" rdbcompression lz4 repl-diskless-sync yes repl-diskless-sync-delay 0 rdbchecksum no}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    test {Compressed diskless full sync loads with rdbchecksum no on the primary} {
        assert_equal "no" [lindex [$primary config get rdbchecksum] 1]
        set primary_loglines [count_log_lines 0]
        populate_compressible_dataset $primary "cksum-off"

        start_server {overrides {save "" rdbcompression lz4 repl-diskless-load disabled}} {
            set replica [srv 0 client]
            set replica_loglines [count_log_lines 0]
            $replica replicaof $primary_host $primary_port

            assert_replica_synced $primary $replica "(checksum off)"

            wait_for_log_messages -1 {"*Diskless full sync with compression: lz4*"} $primary_loglines 50 100
            # Disabled load reads from the received file; match "from <file>.rdb", not "from primary".
            wait_for_log_messages 0 {"*Loading compressed RDB (algo=lz4) from *.rdb*"} $replica_loglines 50 100

            $replica replicaof no one
        }
    }
}

# Mid-transfer link drop on a compressed diskless full sync: swapdb's inline
# socket decompressor (replicaLoadPrimaryRDBFromSocket TRUNCATED path) sees a clean
# EOF before the LZ4 frame end. That is recoverable truncation, NOT corruption, so
# the replica must survive, retry, and resync. Determinism: compression finishes
# almost instantly, so stretch the child with a per-key save delay over a large
# dataset; watch the replica log (not INFO) since it is blocked in the synchronous load.

start_server {overrides {save "" rdbcompression lz4 repl-diskless-sync yes repl-diskless-sync-delay 0}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    test {Compressed diskless full sync recovers from a mid-transfer link drop (truncation, not corruption)} {
        populate_compressible_dataset $primary "trunc" 2000
        set sync_full_before [status $primary sync_full]

        start_server {overrides {save "" rdbcompression lz4 repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            set replica_loglines [count_log_lines 0]

            # 5ms/key over ~2000 keys keeps the compressed stream in flight long enough to interrupt.
            $primary config set rdb-key-save-delay 5000

            $replica replicaof $primary_host $primary_port

            # Catch the replica once it starts the inline socket decode (the TRUNCATED
            # path under test); the log line is emitted at decode start.
            wait_for_log_messages 0 {"*Loading compressed RDB*"} $replica_loglines 300 100

            # Kill the in-flight transfer so the stream reader hits a clean EOF mid-frame.
            # The ~5s remaining load window dwarfs the detect-and-kill latency.
            $primary client kill type replica

            $primary config set rdb-key-save-delay 0

            assert_equal {PONG} [$replica ping]

            # Retries and converges on a clean resync (reconnect driven by the cron, ~1s).
            assert_replica_synced $primary $replica "(truncation recovery)"

            # Interrupted attempt + retry each count one full sync, proving the kill landed mid-transfer.
            assert {[status $primary sync_full] >= $sync_full_before + 2}

            # The interrupted transfer was handled as a recoverable load failure, not a
            # crash or corruption abort (no panic/assertion below).
            assert {[count_log_message 0 "Loading compressed RDB"] >= 1}
            assert {[count_log_message 0 "Failed trying to load the PRIMARY synchronization DB"] >= 1}
            assert_equal 0 [count_log_message 0 "=== ASSERTION FAILED ==="]
            assert {[count_log_message 0 "REPLICA sync: Finished with success"] >= 1}

            $primary set trunc:post "after-recovery"
            wait_for_value_to_propagate_to_replica $primary $replica trunc:post

            $replica replicaof no one
        }
    }
}

# Mixed diskless cohort (one capable, one not) falls back to plaintext for all: the
# group-AND clears the compress-sync bit. A slow manual BGSAVE occupies the child
# slot so both replicas park in WAIT_BGSAVE_START together, exercising the AND deterministically.
start_server {overrides {save "" rdbcompression lz4 repl-diskless-sync yes repl-diskless-sync-delay 0}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    # Enough keys so the manual BGSAVE stays alive for both replicas to register.
    populate_compressible_dataset $primary mixed-diskless 800

    start_server {overrides {save "" rdbcompression lz4 repl-diskless-load swapdb}} {
        set replica_capable [srv 0 client]

        start_server {overrides {save "" rdbcompression yes repl-diskless-load swapdb}} {
            set replica_plain [srv 0 client]

            test {Mixed diskless cohort falls back to a plaintext RDB payload} {
                set rounds_before [count_log_message -2 {Starting BGSAVE for SYNC}]
                set primary_loglines [count_log_lines -2]

                park_replicas_for_grouped_bgsave $primary $replica_capable $replica_plain $primary_host $primary_port

                assert_replica_synced $primary $replica_capable "(mixed diskless capable)"
                assert_replica_synced $primary $replica_plain "(mixed diskless plain)"

                # A single grouped diskless round served both replicas.
                assert_equal 1 [expr {[count_log_message -2 {Starting BGSAVE for SYNC}] - $rounds_before}]

                # AND result: plaintext round, so no compression NOTICE.
                verify_no_log_message -2 "*Diskless full sync with compression: lz4*" $primary_loglines

                $replica_plain replicaof no one
                $replica_capable replicaof no one
            }
        }
    }
}

# Fake-primary negative coverage for the size-framed compressed socket load. A
# complete VCS/LZ4 stream is captured from a throwaway server's dump.rdb (SAVE with
# rdbcompression lz4 writes a full VCS stream), then replayed by fake_primary.tcl
# with a controlled bulk header and payload. The fake primary is plain TCP, so skip under TLS.

if {!$::tls} {

# Capture a complete compressed stream once for all fake-primary tests.
set compressed_payload ""
start_server {overrides {save "" rdbcompression lz4}} {
    set src [srv 0 client]
    for {set i 0} {$i < 32} {incr i} {
        $src set src:key:$i [string repeat "srcval:$i " 8]
    }
    $src save
    set src_dump [server_rdb_path $src]
    set compressed_payload [read_binary_file $src_dump]
    assert_equal "VCS" [string range $compressed_payload 0 2]
}

# Recoverable outcomes share one replica. Each test clears the dataset and
# counters, then always detaches and stops its fake primary.

start_server {overrides {save "" rdbcompression lz4 repl-diskless-load swapdb}} {
    set replica [srv 0 client]

    # Both size-framing mismatches decode a complete frame but fail the
    # consumed-equals-announced check: either fewer bytes arrive than announced,
    # or trailing bytes remain after the frame boundary.
    set padded_payload "$compressed_payload[string repeat J 100]"
    foreach {name payload announced_size} [list \
        "stream ends before the announced size" \
            $compressed_payload [expr {[string length $compressed_payload] + 100}] \
        "trailing bytes remain inside the announced size" \
            $padded_payload [string length $padded_payload] \
    ] {
        test "Compressed socket load fails when $name" {
            set fake_pid ""
            with_cleanup {
                $replica replicaof no one
                $replica flushall
                $replica config resetstat
                set replica_loglines [count_log_lines 0]
                lassign [start_fake_primary $payload $announced_size] fake_pid fake_port

                $replica replicaof 127.0.0.1 $fake_port
                wait_for_log_messages 0 {"*ended before the announced transfer size*"} \
                    $replica_loglines 100 100

                assert_equal {PONG} [$replica ping]
                assert_equal 0 [$replica dbsize]
            } {
                $replica replicaof no one
                if {$fake_pid ne ""} {catch {exec kill $fake_pid}}
            }
        }
    }

    # Complete frame with the exact announced size: the load succeeds and input
    # accounting must count every encoded wire byte: total_net_repl_input_bytes ==
    # N + "$<N>\r\n" bulk header. Tight enough to catch a lost footer.
    test {Compressed socket load counts exactly the encoded wire bytes on a successful load} {
        set fake_pid ""
        with_cleanup {
            $replica replicaof no one
            $replica flushall
            $replica config resetstat
            set n [string length $compressed_payload]
            lassign [start_fake_primary $compressed_payload $n] fake_pid fake_port

            $replica replicaof 127.0.0.1 $fake_port
            wait_for_condition 100 100 {
                [$replica dbsize] > 0
            } else {
                fail "replica did not load the compressed payload"
            }
            # Stop before the dropped link (fake primary closed) can reconnect.
            $replica replicaof no one

            # Bulk header "$<N>\r\n" = 1 ('$') + digits(N) + 2 ("\r\n").
            set meta [expr {3 + [string length $n]}]
            set expected [expr {$n + $meta}]
            set got [status $replica total_net_repl_input_bytes]
            assert_equal $expected $got \
                "net-input $got != N $n + bulk header $meta (expected $expected)"
        } {
            $replica replicaof no one
            if {$fake_pid ne ""} {catch {exec kill $fake_pid}}
        }
    }

    # Announce N but send a truncated prefix and close: the load fails before the
    # frame end, but residual wire bytes must be counted first. Truncation is
    # recoverable, so the replica survives; input == bytes sent + "$<N>\r\n" header.
    test {Compressed socket load counts every wire byte received on a failing (truncated) transfer} {
        set fake_pid ""
        with_cleanup {
            $replica replicaof no one
            $replica flushall
            $replica config resetstat
            set full_len [string length $compressed_payload]
            set bytes_sent [expr {$full_len / 2}]
            set truncated [string range $compressed_payload 0 [expr {$bytes_sent - 1}]]
            set announce $full_len

            set replica_loglines [count_log_lines 0]
            lassign [start_fake_primary $truncated $announce] fake_pid fake_port

            $replica replicaof 127.0.0.1 $fake_port
            wait_for_log_messages 0 {"*Failed trying to load the PRIMARY synchronization DB*"} \
                $replica_loglines 100 100

            # Stop retrying before reading stats to keep the accounting window clean.
            $replica replicaof no one
            assert_equal {PONG} [$replica ping]

            # "$<N>\r\n" bulk header = 1 ('$') + digits(N) + 2 ("\r\n").
            set meta [expr {3 + [string length $announce]}]
            set expected [expr {$bytes_sent + $meta}]
            set got [status $replica total_net_repl_input_bytes]
            assert_equal $expected $got \
                "net-input $got != bytes_sent $bytes_sent + bulk header $meta (expected $expected)"
        } {
            $replica replicaof no one
            if {$fake_pid ne ""} {catch {exec kill $fake_pid}}
        }
    }
}

# Corrupt compressed payloads: a flipped byte mid-frame (caught during parse) and a
# flipped last byte (checksum mismatch at frame finish) converge on the same fatal
# path. A corrupt diskless load is terminal (rdbReportError exits), so each variant runs in its own fresh replica.
test {Corrupted compressed stream (mid-frame and footer) aborts the diskless load} {
    foreach {label offset_expr} {
        mid-frame {[string length $compressed_payload] / 2}
        footer    {[string length $compressed_payload] - 1}
    } {
        start_server {overrides {save "" rdbcompression lz4 repl-diskless-load swapdb}} {
            set replica [srv 0 client]
            set corrupt_at [expr $offset_expr]
            assert {$corrupt_at > 8} ;# flip a byte well past the 7-byte VCS envelope
            binary scan [string index $compressed_payload $corrupt_at] c byte_val
            set corrupted [string replace $compressed_payload $corrupt_at $corrupt_at \
                               [binary format c [expr {$byte_val ^ 0xff}]]]
            lassign [start_fake_primary $corrupted [string length $corrupted]] fake_pid fake_port

            $replica replicaof 127.0.0.1 $fake_port

            wait_for_condition 100 100 {
                ![is_alive [srv 0 pid]]
            } else {
                fail "replica did not exit on a corrupt compressed stream ($label)"
            }
            set stdout [srv 0 stdout]
            assert_equal 1 [count_message_lines $stdout "Corrupt streaming-compressed RDB input"]
            assert_equal 1 [count_message_lines $stdout "Terminating server after rdb file reading failure."]
            catch {exec kill $fake_pid}
        }
    }
}

} ;# end !tls

} ;# end tags

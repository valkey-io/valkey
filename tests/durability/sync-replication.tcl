# Tests for replication-based durability provider (sync replication).
#
# These tests validate the sync replication feature where writes are only
# considered committed once acknowledged by a configured number of sync
# replicas (ISR members).
#
# We use appendfsync=everysec so the AOF durability provider is DISABLED,
# isolating the replication provider behavior. The replication provider
# is enabled when min-sync-replicas > 0.

# Helper: wait until the primary reports at least N sync replicas in the ISR
# by polling INFO durability for durability_sync_replicas.
proc wait_for_isr_count {primary count} {
    wait_for_condition 100 100 {
        [getInfoProperty [$primary info durability] durability_sync_replicas] >= $count
    } else {
        fail "Expected $count sync replicas in ISR but got [getInfoProperty [$primary info durability] durability_sync_replicas]"
    }
}

# Helper: return the current write_blocked_count from INFO durability.
proc get_write_blocked_count {primary} {
    getInfoProperty [$primary info durability] durability_write_blocked_count
}

# ==========================================================================
# Test 1: If number of sync replicas < min-sync-replicas, primary rejects
#         writes with CLUSTERDOWN.
# ==========================================================================

start_server {tags {"repl durability external:skip"} overrides {appendonly yes appendfsync everysec min-sync-replicas 2}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    start_server {overrides {min-sync-replicas 1 sync-eligible yes}} {
        set replica1 [srv 0 client]
        set replica1_host [srv 0 host]
        set replica1_port [srv 0 port]

        start_server {overrides {min-sync-replicas 1 sync-eligible yes}} {
            set replica2 [srv 0 client]
            set replica2_host [srv 0 host]
            set replica2_port [srv 0 port]

            test "Sync replication: write rejected when ISR count < min-sync-replicas" {
                # Connect only replica1 — ISR will have 1 member
                $replica1 replicaof $primary_host $primary_port
                wait_replica_online $primary
                wait_for_isr_count $primary 1

                # Write must be rejected — only 1 of 2 required sync replicas
                catch {$primary set mykey myvalue} err
                assert_match "*CLUSTERDOWN*" $err

                # Connect replica2 so ISR reaches 2
                $replica2 replicaof $primary_host $primary_port
                wait_replica_online $primary
                wait_for_isr_count $primary 2

                # Write should succeed now
                assert_equal "OK" [$primary set mykey myvalue]
            }
        }
    }
}

# ==========================================================================
# Test 2: Primary connected to 2 sync replicas. Write is released to client
#         only when both sync replicas ack back.
# ==========================================================================

start_server {tags {"repl durability external:skip"} overrides {appendonly yes appendfsync everysec min-sync-replicas 2}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    start_server {overrides {min-sync-replicas 1 sync-eligible yes}} {
        set replica1 [srv 0 client]
        set replica1_host [srv 0 host]
        set replica1_port [srv 0 port]

        start_server {overrides {min-sync-replicas 1 sync-eligible yes}} {
            set replica2 [srv 0 client]
            set replica2_host [srv 0 host]
            set replica2_port [srv 0 port]

            test "Sync replication: write released only after both sync replicas ack" {
                # Connect both replicas and let them sync
                $replica1 replicaof $primary_host $primary_port
                $replica2 replicaof $primary_host $primary_port
                wait_replica_online $primary
                wait_for_isr_count $primary 2

                # Pause the replication provider so acks don't advance consensus
                $primary DEBUG durability-provider-pause replication

                set blocked_before [get_write_blocked_count $primary]

                # Issue a write via deferring client — reply should be held
                set rd [valkey_deferring_client -2]
                $rd set mykey myvalue

                # Wait for the write to be blocked
                wait_for_condition 50 100 {
                    [get_write_blocked_count $primary] > $blocked_before
                } else {
                    fail "Write was not blocked by durability provider"
                }

                # Resume the replication provider — replicas have already acked,
                # so consensus advances and the reply is released
                $primary DEBUG durability-provider-resume replication
                $primary ping ;# force a beforeSleep cycle

                assert_equal "OK" [$rd read]
                $rd close
            }
        }
    }
}

# ==========================================================================
# Test 3: Primary connected to 2 sync replicas. Write is blocked if one
#         replica lags behind (paused with SIGSTOP).
# ==========================================================================

start_server {tags {"repl durability external:skip"} overrides {appendonly yes appendfsync everysec min-sync-replicas 2}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    start_server {overrides {min-sync-replicas 1 sync-eligible yes}} {
        set replica1 [srv 0 client]
        set replica1_host [srv 0 host]
        set replica1_port [srv 0 port]

        start_server {overrides {min-sync-replicas 1 sync-eligible yes}} {
            set replica2 [srv 0 client]
            set replica2_host [srv 0 host]
            set replica2_port [srv 0 port]

            test "Sync replication: write blocked when one replica lags behind" {
                # Connect both replicas and let them sync
                $replica1 replicaof $primary_host $primary_port
                $replica2 replicaof $primary_host $primary_port
                wait_replica_online $primary
                wait_for_isr_count $primary 2

                # Verify writes work when both replicas are healthy
                assert_equal "OK" [$primary set healthy-key healthy-value]

                # Pause replica2 at the OS level (SIGSTOP) so it stops
                # sending ACKs. Its ack offset is frozen, preventing
                # consensus from advancing past new writes.
                set replica2_pid [srv 0 pid]
                pause_process $replica2_pid

                set blocked_before [get_write_blocked_count $primary]

                # Issue a write via deferring client — replica1 will ack
                # but replica2 cannot, so min_offset stays behind.
                set rd [valkey_deferring_client -2]
                $rd set blocked-key blocked-value

                # Verify the write was blocked
                wait_for_condition 50 100 {
                    [get_write_blocked_count $primary] > $blocked_before
                } else {
                    fail "First write was not blocked"
                }

                set blocked_before2 [get_write_blocked_count $primary]

                # Issue another write — also blocked
                $rd set blocked-key2 blocked-value2

                wait_for_condition 50 100 {
                    [get_write_blocked_count $primary] > $blocked_before2
                } else {
                    fail "Second write was not blocked"
                }

                # Resume replica2 — it catches up and acks, both writes unblock
                resume_process $replica2_pid

                assert_equal "OK" [$rd read]
                assert_equal "OK" [$rd read]
                $rd close
            }
        }
    }
}

# ==========================================================================
# Test 4: Replica killed — writes rejected, replica restarts — writes resume.
#
# Primary with min-sync-replicas=1 and a single sync replica. The replica
# is killed (shutdown nosave). After repl-timeout the primary disconnects
# the dead replica, ISR drops to 0, and writes are rejected with
# CLUSTERDOWN. The replica is then restarted, rejoins the ISR, and writes
# are accepted again.
# ==========================================================================

start_server {tags {"repl durability external:skip"} overrides {appendonly yes appendfsync everysec min-sync-replicas 1}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    start_server {overrides {min-sync-replicas 1 sync-eligible yes}} {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]

        test "Sync replication: replica killed — writes rejected then resume after restart" {
            # Use a short repl-timeout so the primary detects the dead
            # replica quickly.
            $primary config set repl-timeout 3

            $replica replicaof $primary_host $primary_port
            wait_replica_online $primary
            wait_for_isr_count $primary 1

            # Writes should succeed with a healthy replica
            assert_equal "OK" [$primary set key1 value1]

            # Kill the replica
            catch {$replica shutdown nosave}

            # Wait for the primary to disconnect the dead replica
            wait_for_condition 50 200 {
                [s -1 connected_slaves] == 0
            } else {
                fail "Primary did not disconnect the dead replica"
            }

            # Writes must now be rejected — no replicas in ISR
            catch {$primary set key2 value2} err
            assert_match "*CLUSTERDOWN*" $err

            # Restart the replica — it reconnects and rejoins the ISR
            restart_server 0 true false

            set replica [srv 0 client]
            $replica replicaof $primary_host $primary_port
            wait_replica_online $primary
            wait_for_isr_count $primary 1

            # Writes should succeed again
            assert_equal "OK" [$primary set key3 value3]
        }
    }
}

# ==========================================================================
# Test 5: Replica paused (SIGSTOP) — writes rejected after ISR timeout,
#         replica resumed — writes accepted again.
#
# Primary with min-sync-replicas=1 and a single sync replica. The replica
# is paused with SIGSTOP. It stops sending ACKs, and after the ISR timeout
# (REPLICA_ISR_TIMEOUT = 10 s) the primary removes it from the ISR.
# With 0 ISR members, writes are rejected. Resuming the replica (SIGCONT)
# lets it catch up, rejoin the ISR, and writes succeed again.
# ==========================================================================

start_server {tags {"repl durability external:skip"} overrides {appendonly yes appendfsync everysec min-sync-replicas 1}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    start_server {overrides {min-sync-replicas 1 sync-eligible yes}} {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]
        set replica_pid [srv 0 pid]

        test "Sync replication: replica paused — writes rejected then resume after SIGCONT" {
            # Use a repl-timeout longer than the ISR timeout so the
            # replica is removed from the ISR but NOT disconnected.
            # ISR timeout is 10 s, so set repl-timeout to 20 s.
            $primary config set repl-timeout 20

            $replica replicaof $primary_host $primary_port
            wait_replica_online $primary
            wait_for_isr_count $primary 1

            # Writes should succeed with a healthy replica
            assert_equal "OK" [$primary set key1 value1]

            # Pause the replica — it stays connected but stops ACKing
            pause_process $replica_pid

            # Wait for the ISR timeout to remove the replica from ISR.
            wait_for_condition 150 200 {
                [getInfoProperty [$primary info durability] durability_sync_replicas] == 0
            } else {
                fail "Replica was not removed from ISR after timeout"
            }

            # The replica is still connected (not timed out by
            # repl-timeout which is 20 s) but removed from ISR.
            assert_equal 1 [s -1 connected_slaves]

            # Writes must now be rejected — 0 ISR members
            catch {$primary set key2 value2} err
            assert_match "*CLUSTERDOWN*" $err

            # Resume the replica — it catches up and rejoins the ISR
            resume_process $replica_pid
            wait_for_isr_count $primary 1

            # Writes should succeed again
            set rd [valkey_deferring_client -1]
            $rd set key3 value3
            assert_equal "OK" [$rd read]
            $rd close
        }
    }
}

# ==========================================================================
# Test 6: Consensus offset advances based on sync replica only, not
#         regular (non-sync) replicas.
#
# Primary with min-sync-replicas=1, one sync replica, and one regular
# replica (sync-eligible=no). Writes succeed. The regular replica is
# paused (SIGSTOP). Writes continue to succeed and the committed offset
# advances, proving that consensus depends solely on the sync replica.
# ==========================================================================

start_server {tags {"repl durability external:skip"} overrides {appendonly yes appendfsync everysec min-sync-replicas 1}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    # Sync replica
    start_server {overrides {min-sync-replicas 1 sync-eligible yes}} {
        set sync_replica [srv 0 client]
        set sync_replica_host [srv 0 host]
        set sync_replica_port [srv 0 port]

        # Regular (non-sync) replica
        start_server {overrides {min-sync-replicas 1 sync-eligible no}} {
            set regular_replica [srv 0 client]
            set regular_replica_host [srv 0 host]
            set regular_replica_port [srv 0 port]
            set regular_replica_pid [srv 0 pid]

            test "Sync replication: committed offset advances based on sync replica, not regular replica" {
                # Connect both replicas
                $sync_replica replicaof $primary_host $primary_port
                $regular_replica replicaof $primary_host $primary_port

                # Wait for both replicas to come online
                wait_for_condition 50 100 {
                    [string match "*slave0:*state=online*" [$primary info replication]] &&
                    [string match "*slave1:*state=online*" [$primary info replication]]
                } else {
                    fail "Replicas did not come online"
                }

                # Wait for sync replica to join ISR
                wait_for_isr_count $primary 1

                # Verify writes succeed with both replicas healthy
                assert_equal "OK" [$primary set key1 value1]

                # Record the committed offset after the first write.
                set offset_before [getInfoProperty [$primary info durability] durability_committed_offset]
                assert {$offset_before > 0}

                # Pause the regular (non-sync) replica
                pause_process $regular_replica_pid

                # Issue more writes — they should succeed because the
                # sync replica is still healthy and ACKing.
                assert_equal "OK" [$primary set key2 value2]
                assert_equal "OK" [$primary set key3 value3]

                # Verify the committed offset has advanced, proving
                # consensus is driven by the sync replica alone.
                set offset_after [getInfoProperty [$primary info durability] durability_committed_offset]
                assert {$offset_after > $offset_before}

                # Verify the primary still sees 2 connected replicas
                # (the regular one is paused but TCP connection is alive)
                assert_equal 2 [status $primary connected_slaves]

                # Resume the regular replica
                resume_process $regular_replica_pid

                # One more write to confirm everything is still healthy
                assert_equal "OK" [$primary set key4 value4]

                set offset_final [getInfoProperty [$primary info durability] durability_committed_offset]
                assert {$offset_final > $offset_after}
            }
        }
    }
}

# ==========================================================================
# Test 7: [WBL] Replica blocks reads on uncommitted keys until REPLCONF COMMIT
#         arrives from the primary.
#
# The primary's replication provider is paused so the committed offset
# stops advancing. A write on the primary replicates to the replica but
# the committed offset doesn't move. A client connected to the replica
# reads the key — the read is blocked because the key is dirty
# (uncommitted). When the provider is resumed, the primary sends
# REPLCONF COMMIT with the new offset, the replica unblocks the read.
# ==========================================================================

start_server {tags {"repl durability external:skip"} overrides {appendonly yes appendfsync everysec min-sync-replicas 2}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    start_server {overrides {min-sync-replicas 1 sync-eligible yes}} {
        set replica1 [srv 0 client]
        set replica1_host [srv 0 host]
        set replica1_port [srv 0 port]

        start_server {overrides {min-sync-replicas 1 sync-eligible yes}} {
            set replica2 [srv 0 client]
            set replica2_host [srv 0 host]
            set replica2_port [srv 0 port]

            test "Sync replication: replica blocks read on uncommitted key until REPLCONF COMMIT" {
                # Connect both replicas and wait for ISR
                $replica1 replicaof $primary_host $primary_port
                $replica2 replicaof $primary_host $primary_port
                wait_replica_online $primary
                wait_for_isr_count $primary 2

                # Verify the system is healthy — a write succeeds end-to-end
                assert_equal "OK" [$primary set committed-key committed-value]

                # Verify the replica can read the committed key
                wait_for_condition 50 100 {
                    [$replica1 get committed-key] eq "committed-value"
                } else {
                    fail "Committed key did not replicate to replica"
                }

                # Pause the replication provider on the primary.
                # This freezes the committed offset — new writes will
                # replicate to replicas but REPLCONF COMMIT won't advance.
                $primary DEBUG durability-provider-pause replication

                # Write a key on the primary via a deferring client.
                # We don't read the reply — it will be blocked by the
                # paused provider, but we don't care about it.
                # The write replicates to replicas via the replication stream.
                set writer [valkey_deferring_client -2]
                $writer set uncommitted-key uncommitted-value

                # Wait for the write to replicate to the replica
                wait_for_condition 50 100 {
                    [getInfoProperty [$replica1 info durability] durability_uncommitted_keys] > 0
                } else {
                    fail "Key was not tracked as uncommitted on replica"
                }

                # Now a client connected to replica1 reads the key.
                # The key exists on the replica (it was replicated) but
                # is uncommitted (committed offset hasn't advanced).
                # The read should be blocked.
                set replica_blocked_before [getInfoProperty [$replica1 info durability] durability_clients_waiting_ack]

                set reader [valkey_deferring_client -1]
                $reader get uncommitted-key

                # Verify the read is blocked on the replica
                wait_for_condition 50 100 {
                    [getInfoProperty [$replica1 info durability] durability_clients_waiting_ack] > $replica_blocked_before
                } else {
                    fail "Read on uncommitted key was not blocked on replica"
                }

                # Resume the replication provider on the primary.
                # The committed offset advances, the primary sends
                # REPLCONF COMMIT, and the replica unblocks the read.
                $primary DEBUG durability-provider-resume replication
                $primary ping ;# force beforeSleep cycle

                # The reader should now get the value
                assert_equal "uncommitted-value" [$reader read]

                $reader close
                $writer close
            }
        }
    }
}

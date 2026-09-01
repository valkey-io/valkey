# Tests for CLEAN_STATE_FOR_DOLLY_SAVE.
#
# Scenario: a replica in a shard failed. Instead of provisioning a fresh
# replica (which forces a full BGSAVE - prohibitive on hundreds of GB of
# data), we CRIU-clone the primary to a new host. The clone runs
# CLEAN_STATE_FOR_DOLLY_SAVE and then issues REPLICAOF toward its source
# primary; the subsequent PSYNC lands a +CONTINUE (partial resync, zero
# RDB transfer) because the clone carries the primary's replid + offset.
#
# Cluster-mode cases are in tests/unit/cluster/dolly-save-clean-state.tcl.

start_server {tags {"dolly-save" "standalone"}} {
    test "CLEAN_STATE_FOR_DOLLY_SAVE regenerates run_id and preserves replid" {
        set old_run_id [status r run_id]
        set old_replid [status r master_replid]
        assert_equal [string length $old_run_id] 40
        assert_equal [string length $old_replid] 40

        # Small sleep so stat_starttime's new value differs observably.
        after 1100

        assert_equal "OK" [r CLEAN_STATE_FOR_DOLLY_SAVE]

        set new_run_id [status r run_id]
        set new_replid [status r master_replid]
        assert_equal [string length $new_run_id] 40
        assert_equal [string length $new_replid] 40
        assert {$new_run_id ne $old_run_id}

        # Replication identity is preserved so the clone can PSYNC with its
        # source primary after CRIU restore.
        assert_equal $old_replid $new_replid
    }

    test "CLEAN_STATE_FOR_DOLLY_SAVE preserves the keyspace" {
        r flushall
        r set mykey myval
        r sadd myset a b c

        r CLEAN_STATE_FOR_DOLLY_SAVE

        assert_equal myval [r get mykey]
        assert_equal 3 [r scard myset]
    }

    test "CLEAN_STATE_FOR_DOLLY_SAVE uptime restarts" {
        # Let uptime accumulate a little.
        after 1100
        set before [status r uptime_in_seconds]
        assert {$before >= 1}

        r CLEAN_STATE_FOR_DOLLY_SAVE

        set after_reset [status r uptime_in_seconds]
        assert {$after_reset < $before}
    }

    test "CLEAN_STATE_FOR_DOLLY_SAVE is idempotent" {
        set id1 [status r run_id]
        assert_equal "OK" [r CLEAN_STATE_FOR_DOLLY_SAVE]
        set id2 [status r run_id]
        assert_equal "OK" [r CLEAN_STATE_FOR_DOLLY_SAVE]
        set id3 [status r run_id]

        assert {$id1 ne $id2}
        assert {$id2 ne $id3}
        assert {$id1 ne $id3}

        assert_equal PONG [r ping]
    }

    test "CLEAN_STATE_FOR_DOLLY_SAVE rejects extra arguments" {
        catch {r CLEAN_STATE_FOR_DOLLY_SAVE extra} e
        assert_match "*wrong number of arguments*" $e
    }
}

# A CRIU-cloned replica is running REPLICAOF toward its source primary and
# partial-resyncs via the cached_primary synthesized by CLEAN_STATE_FOR_DOLLY_SAVE.
#
# Staging (to produce two servers that share a replid without CRIU):
#   1. A is primary, B is replica of A -> B inherits A's replid.
#   2. Disconnect B, promote B to primary (REPLICAOF NO ONE): B shifts
#      its own replid to R2 and remembers A's replid as replid2. B will
#      accept PSYNC for either R1 or R2 for offsets in its backlog.
#   3. On A: run CLEAN_STATE_FOR_DOLLY_SAVE. This synthesizes A's
#      cached_primary with {replid=R1, offset=O_A}. A's role stays
#      master, keyspace intact.
#   4. On A: REPLICAOF B. A connects to B and emits PSYNC R1 (O_A+1).
#      B matches via its replid2 and answers +CONTINUE.
#   5. Assert B's sync_partial_ok incremented and sync_full did not.
start_server {tags {"dolly-save" "standalone"}} {
    start_server {} {
        test "CLEAN_STATE_FOR_DOLLY_SAVE synthesizes cached_primary for PSYNC" {
            set A [srv -1 client]
            set A_host [srv -1 host]
            set A_port [srv -1 port]
            set B [srv 0 client]
            set B_host [srv 0 host]
            set B_port [srv 0 port]

            # Phase 1: B replicates from A so they share a replid.
            $A set k1 v1
            $A set k2 v2
            $B replicaof $A_host $A_port
            wait_for_condition 50 100 {
                [status $B master_link_status] eq "up"
            } else {
                fail "B did not connect to A"
            }
            wait_for_condition 50 100 {
                [$A debug digest] eq [$B debug digest]
            } else {
                fail "B did not sync data from A"
            }

            set shared_replid [status $A master_replid]
            assert_equal $shared_replid [status $B master_replid]

            # Phase 2: Promote B. B shifts its replid and remembers the
            # shared one as replid2. B's backlog will accept PSYNC against
            # replid2 for offsets within its window.
            $B replicaof no one
            wait_for_condition 50 100 {
                [status $B role] eq "master"
            } else {
                fail "B did not promote"
            }
            assert_equal $shared_replid [status $B master_replid2]

            # Reset B's sync counters so the assertion below is unambiguous.
            $B config resetstat

            # Phase 3: Run CLEAN_STATE_FOR_DOLLY_SAVE on A, the "source
            # primary" in our CRIU-clone analogy. A's role stays master,
            # replid preserved; cached_primary is synthesized.
            set A_replid_before [status $A master_replid]
            set A_offset_before [status $A master_repl_offset]
            assert_equal "OK" [$A CLEAN_STATE_FOR_DOLLY_SAVE]
            assert_equal master [status $A role]
            assert_equal $A_replid_before [status $A master_replid]
            # The offset may advance slightly if a replication no-op was
            # appended, but must not regress.
            assert {[status $A master_repl_offset] >= $A_offset_before}

            # Phase 4: A becomes a replica of B. Thanks to the synthesized
            # cached_primary, the handshake emits PSYNC <replid> <offset+1>
            # rather than PSYNC ? -1.
            $A replicaof $B_host $B_port
            wait_for_condition 100 100 {
                [status $A master_link_status] eq "up"
            } else {
                fail "A did not connect to B as replica"
            }

            # Phase 5: Assert partial resync, not full.
            assert_equal 1 [status $B sync_partial_ok]
            assert_equal 0 [status $B sync_full]
        }
    }
}

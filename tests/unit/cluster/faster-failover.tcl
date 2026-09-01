# Check the faster failover is working.

# Test 0: Single replica is always the best ranked replica.
# Minimal deployment: 3 primaries + 1 replica.
#
# We have the following nodes:
# Primary: R0  R1  R2
# Replica: R3 (replica of R0)
#
# R0 owns slots 0-5461, key key_977613 belongs to slot 0.
#
# With only one replica, auth_rank is always 0, failed_primary_rank is
# always 0, and clusterAllReplicasThinkPrimaryIsFail() only checks itself.
# So the sole replica R3 is always identified as the best ranked replica.
start_cluster 3 1 {tags {external:skip cluster} overrides {cluster-ping-interval 1000 cluster-node-timeout 5000}} {
    test "The sole replica is always the best ranked replica" {
        # Write some data to R0 so the replica has a non-zero offset.
        for {set i 0} {$i < 10} {incr i} {
            R 0 incr key_977613
        }
        wait_for_ofs_sync [srv 0 client] [srv -3 client]

        # Pause R0, R3 will do the automatic failover.
        pause_process [srv 0 pid]
        wait_for_condition 1000 50 {
            [s -3 role] == "master"
        } else {
            fail "No failover detected"
        }

        # The sole replica must be identified as best ranked replica.
        verify_log_message -3 "*This is the best ranked replica and can initiate the election immediately*" 0
        verify_log_message -3 "*Start of election delayed for 0 milliseconds (rank #0, primary rank #0, offset *)*" 0
    }
} ;# start_cluster

# Replica allocation for test 1: (3 primaries, 2 replicas, both replicas go to R0).
# R3 and R4 are both replicas of R0.
proc single_primary_replica_allocation {masters replicas} {
    set master0_id [R 0 CLUSTER MYID]
    R 3 CLUSTER REPLICATE $master0_id
    R 4 CLUSTER REPLICATE $master0_id
}

# Test 1: Single primary failure.
# Minimal deployment: 3 primaries + 2 replicas.
#
# We have the following nodes:
# Primary:  R0  R1  R2
# Replica1: R3 (replica of R0)
# Replica2: R4 (replica of R0)
#
# R0 owns slots 0-5461, key key_977613 belongs to slot 0.
#
# Only R0 goes down. With only one failed primary, failed_primary_rank is
# deterministically 0, so the best ranked replica (R4, which has a higher
# offset than R3), it must be identified as the best ranked replica either
# at initialization or during the delay update phase.
#
# However, in a bad case, R4 may not receive R3's MY_PRIMARY_FAIL flag
# via gossip before starting the election. We run this test multiple times
# and assert that the "best ranked replica" path is triggered at least once.
start_cluster 3 2 {tags {external:skip cluster} overrides {cluster-ping-interval 1000 cluster-node-timeout 5000}} {
    test "The best replica can initiate an election immediately in an automatic failover - single primary failure" {
        set best_ranked_triggered 0
        for {set attempt 1} {$attempt < 6} {incr attempt} {
            # Write some data to R0 so the replicas have a non-zero offset.
            for {set i 0} {$i < 10} {incr i} {
                R 0 incr key_977613
            }
            wait_for_ofs_sync [srv 0 client] [srv -3 client]
            wait_for_ofs_sync [srv 0 client] [srv -4 client]

            # Pause R3 so it has no chance to catch up with the offset.
            pause_process [srv -3 pid]

            # Kill the replica client of R0.
            R 0 client kill type replica
            R 0 incr key_977613

            # Wait for R4 to catch up with the offset so it will have
            # a better offset than R3.
            wait_for_ofs_sync [srv 0 client] [srv -4 client]

            # Pause R0 and resume R3, the replicas will do the automatic failover.
            pause_process [srv 0 pid]
            resume_process [srv -3 pid]
            wait_for_condition 1000 50 {
                ([s -4 role] == "master" || [s -3 role] == "master")
            } else {
                fail "No failover detected"
            }

            # With only one failed primary, failed_primary_rank is always 0.
            # R4 has the best offset (rank #0), so it must be identified as
            # the best ranked replica either at initialization or during the
            # delay update phase.
            if {[count_log_message -4 "This is the best ranked replica and can initiate the election immediately"]} {
                verify_log_message -4 "*Start of election delayed for 0 milliseconds*" 0
            }
            if {[count_log_message -4 "Myself become the best ranked replica, initiate the election immediately"]} {
                # Nothing to verify
            }

            set best_ranked_triggered [count_log_message -4 "best ranked replica"]
            if {$best_ranked_triggered > 0} break

            # "best ranked replica" was not triggered this round because R4
            # did not receive R3's MY_PRIMARY_FAIL gossip in time. Restore the
            # cluster and retry.

            # Resume R0
            resume_process [srv 0 pid]
            wait_for_condition 1000 50 {
                [s 0 role] == "slave"
            } else {
                fail "R0 did not become replica after resume"
            }

            # Manual failover R0 back to primary.
            R 0 cluster failover takeover
            wait_for_condition 1000 50 {
                [s 0 role] == "master" &&
                [s -3 role] == "slave" &&
                [s -4 role] == "slave"
            } else {
                fail "R0 did not become primary after manual failover"
            }

            # Wait for all replicas to sync before the next attempt.
            wait_for_ofs_sync [srv 0 client] [srv -3 client]
            wait_for_ofs_sync [srv 0 client] [srv -4 client]
            wait_for_cluster_propagation
            wait_for_cluster_state "ok"
        }

        if {$::verbose} {
            puts "The best replica can initiate an election immediately in an automatic failover attempts: $attempt"
        }
        assert_morethan $best_ranked_triggered 0
    }
} continuous_slot_allocation single_primary_replica_allocation ;# start_cluster

# Slot allocation for test 2 (5 primaries, 7 replicas).
# Allocate slot 0 to the first primary and allocate slot 1 to the second primary,
# and then evenly distribute the remaining slots to the remaining primaries.
proc two_primaries_slot_allocation {primaries replicas} {
    R 0 cluster addslots 0
    R 1 cluster addslots 1
    R 2 cluster addslotsrange 2 5463
    R 3 cluster addslotsrange 5464 10927
    R 4 cluster addslotsrange 10928 16383
}

# Test 2: Two primaries fail at the same time (5 primaries + 7 replicas).
#
# We have the following nodes:
# Primary:  R0  R1  R2  R3  R4
# Replica1: R5  R6  R7  R8  R9
# Replica2: R10 R11
#
# R0 own slot 0 and its replicas are R5 and R10, key key_977613 belong to slot 0.
# R1 own slot 1 and its replicas are R6 and R11, key key_991803 belong to slot 1.
#
# We will test the scenario where both R0 and R1 shards are down at the same time
# to test whether their faster failover is as expected.
#
# In the R0 shard, the offset of R10 will be greater than that of R5, so it is
# expected that R10 will start failover faster.
#
# In the R1 shard, the offsets of R6 and R11 are the same, we have replica rank
# to sure that there will not have failover timeout.
#
# Even if R0 and R1 down at the same time, we have failed primary rank to ensure
# that there will not have failover timeout.
#
# However, things won't go as expected due to timing issues, etc., we run this
# test multiple times and assert that the "best ranked replica" path is triggered
# at least once.
#
start_cluster 5 7 {tags {external:skip cluster tls:skip} overrides {cluster-ping-interval 1000 cluster-node-timeout 5000}} {
    test "The best replica can initiate an election immediately in an automatic failover - two primaries failure" {
        # We calculate their rankings so that we can make rank judgments later.
        set R0_failed_primary_rank [expr [expr [memcmp [R 0 cluster myshardid] [R 1 cluster myshardid]] < 0] ? 0 : 1]
        set R1_failed_primary_rank [expr [expr [memcmp [R 1 cluster myshardid] [R 0 cluster myshardid]] < 0] ? 0 : 1]

        set R6_replica_rank [expr [expr [memcmp [R 6 cluster myid] [R 11 cluster myid]] < 0] ? 0 : 1]
        set R11_replica_rank [expr [expr [memcmp [R 11 cluster myid] [R 6 cluster myid]] < 0] ? 0 : 1]

        set R0_nodeid [R 0 cluster myid]
        set R1_nodeid [R 1 cluster myid]

        set best_ranked_triggered 0
        for {set attempt 1} {$attempt < 11} {incr attempt} {
            # Write some data to the R0 and wait the sync.
            for {set i 0} {$i < 10} {incr i} {
                R 0 incr key_977613
            }
            wait_for_ofs_sync [srv 0 client] [srv -5 client]
            wait_for_ofs_sync [srv 0 client] [srv -10 client]

            # Write some data to the R1 and wait the sync.
            for {set i 0} {$i < 10} {incr i} {
                R 1 incr key_991803
            }
            wait_for_ofs_sync [srv -1 client] [srv -6 client]
            wait_for_ofs_sync [srv -1 client] [srv -11 client]

            # Pause R5 so it has no chance to catch up with the offset.
            pause_process [srv -5 pid]

            # Kill the replica client of R0.
            R 0 client kill type replica
            R 0 incr key_977613

            # Wait for R10 to catch up with the offset so it will have a better offset than R5.
            wait_for_ofs_sync [srv 0 client] [srv -10 client]

            # Pause R0 and R1, the replicas of each shard will do the automatic failover.
            pause_process [srv 0 pid]
            pause_process [srv -1 pid]

            # Resume R5.
            resume_process [srv -5 pid]

            wait_for_condition 1000 100 {
                ([s -10 role] == "master" || [s -5 role] == "master") &&
                ([s -6 role] == "master" || [s -11 role] == "master")
            } else {
                fail "No failover detected"
            }

            # case 1: R0 and R1 down in the same time, R0 have a better failed primary rank.
            # R10 has the best offset (rank #0, primary rank #0), so it must be identified as
            # the best ranked replica either at initialization or during the delay update phase.
            if {$R0_failed_primary_rank == 0} {
                if {[count_log_message -10 "This is the best ranked replica and can initiate the election immediately"] != 0} {
                    verify_log_message -10 "*Start of election delayed for 0 milliseconds*" 0
                }
                if {[count_log_message -10 "Myself become the best ranked replica, initiate the election immediately"]} {
                    # Nothing to verify
                }
            }

            # case 2: R0 and R1 down in the same time, R1 have a better failed primary rank.
            # R6 or R11 will be the best ranked replica.
            if {$R1_failed_primary_rank == 0 && $R6_replica_rank == 0} {
                if {[count_log_message -6 "This is the best ranked replica and can initiate the election immediately"]} {
                    verify_log_message -6 "*Start of election delayed for 0 milliseconds*" 0
                }
            }
            if {$R1_failed_primary_rank == 0 && $R11_replica_rank == 0} {
                if {[count_log_message -11 "This is the best ranked replica and can initiate the election immediately"]} {
                    verify_log_message -11 "*Start of election delayed for 0 milliseconds*" 0
                }
            }

            # In any case, we do not expect timeouts.
            verify_no_log_message -5 "*Failover attempt expired*" 0
            verify_no_log_message -10 "*Failover attempt expired*" 0
            verify_no_log_message -6 "*Failover attempt expired*" 0
            verify_no_log_message -11 "*Failover attempt expired*" 0

            # Check that at least one of R10, R6, or R11 is a best replica.
            incr best_ranked_triggered [count_log_message -10 "best ranked replica"]
            incr best_ranked_triggered [count_log_message -6 "best ranked replica"]
            incr best_ranked_triggered [count_log_message -11 "best ranked replica"]
            if {$best_ranked_triggered > 0} break

            # "best ranked replica" was not triggered this round because many
            # reasons. Restore the cluster and retry.

            # Resume R0 and R1
            resume_process [srv 0 pid]
            resume_process [srv -1 pid]
            wait_for_condition 1000 50 {
                [s 0 role] == "slave" &&
                [s -1 role] == "slave"
            } else {
                fail "R0 or R1 did not become replica after resume"
            }

            # Manual failover R0 back to primary.
            R 0 cluster failover takeover
            wait_for_condition 1000 50 {
                [s 0 role] == "master" &&
                [s -5 role] == "slave" &&
                [s -10 role] == "slave"
            } else {
                fail "R0 did not become primary after manual failover"
            }

            # Manual failover R1 back to primary.
            R 1 cluster failover takeover
            wait_for_condition 1000 50 {
                [s -1 role] == "master" &&
                [s -6 role] == "slave" $$
                [s -11 role] == "slave"
            } else {
                fail "R1 did not become primary after manual failover"
            }

            # Wait for all replicas to sync before the next attempt.
            wait_for_ofs_sync [srv 0 client] [srv -5 client]
            wait_for_ofs_sync [srv 0 client] [srv -10 client]
            wait_for_ofs_sync [srv -1 client] [srv -6 client]
            wait_for_ofs_sync [srv -1 client] [srv -11 client]
            wait_for_cluster_propagation
            wait_for_cluster_state "ok"
        } ;# end for

        if {$::verbose} {
            puts "The best replica can initiate an election immediately in an automatic failover - two primaries failure attempts: $attempt"
        }
        assert_morethan $best_ranked_triggered 0
    }
} two_primaries_slot_allocation cluster_allocate_replicas ;# start_cluster

# Regression test for a stale CLUSTER_NODE_MY_PRIMARY_FAIL flag.
#
# CLUSTER_NODE_MY_PRIMARY_FAIL means "I am a replica and my primary is FAIL in
# my view". It is set on myself and gossiped so that other replicas in the shard
# can tell whether everybody has already seen the failure (and therefore whether
# the exchanged replication offsets are fresh). clusterAllReplicasThinkPrimaryIsFail()
# uses it to let the best ranked replica skip the election delay entirely.
#
# If the flag is not cleared when the replica is reconfigured under a new primary,
# it keeps advertising "my primary is dead" forever. A later, unrelated failover
# then sees a bogus unanimous vote and takes the no-delay fast path even though
# the offsets were never re-exchanged.
#
# The deployment is 3 primaries + 2 replicas, both replicas under R0:
#   Primary:  R0  R1  R2
#   Replica:  R3 (replica of R0)   <- will win the first failover
#             R4 (replica of R0)   <- cluster-replica-no-failover, the flag carrier
#
# R0 owns slots 0-5461, key key_977613 belongs to slot 0.
proc stale_flag_replica_allocation {masters replicas} {
    set master0_id [R 0 CLUSTER MYID]
    R 3 CLUSTER REPLICATE $master0_id
    R 4 CLUSTER REPLICATE $master0_id
}

start_cluster 3 2 {tags {external:skip cluster} overrides {cluster-ping-interval 1000 cluster-node-timeout 5000}} {
    test "MY_PRIMARY_FAIL is cleared when a replica is reconfigured under a new primary" {
        set R0_id [R 0 CLUSTER MYID]
        set R3_id [R 3 CLUSTER MYID]
        set R4_id [R 4 CLUSTER MYID]

        # R4 must never run an election. It only exists to carry (or not carry)
        # the MY_PRIMARY_FAIL flag, which makes the first failover deterministic:
        # R3 is the only node that can win it.
        R 4 config set cluster-replica-no-failover yes

        # Give the replicas a non-zero replication offset.
        for {set i 0} {$i < 10} {incr i} {
            R 0 incr key_977613
        }
        wait_for_ofs_sync [srv 0 client] [srv -3 client]
        wait_for_ofs_sync [srv 0 client] [srv -4 client]

        ##########################################################
        # Phase 1: make R4 set MY_PRIMARY_FAIL, then move it to a
        #          brand new primary while the flag is still set.
        ##########################################################
        pause_process [srv 0 pid]

        # R4 marks R0 as FAIL, so it sets MY_PRIMARY_FAIL on itself.
        wait_node_marked_fail 4 $R0_id

        # Only R3 is allowed to fail over, so this is not a race.
        wait_for_condition 1000 50 {
            [s -3 role] == "master"
        } else {
            fail "R3 did not take over"
        }

        # R4 is reconfigured under R3 via clusterSetPrimary(). This is where the
        # stale flag is (or is not) cleared.
        wait_for_condition 1000 50 {
            [dict get [cluster_get_node_by_id 4 $R4_id] slaveof] eq $R3_id
        } else {
            fail "R4 did not become a replica of R3"
        }

        # Bring R0 back as the second replica of R3.
        resume_process [srv 0 pid]
        wait_for_condition 1000 50 {
            [s 0 role] == "slave" &&
            [dict get [cluster_get_node_by_id 0 $R0_id] slaveof] eq $R3_id
        } else {
            fail "R0 did not come back as a replica of R3"
        }

        # R0 must know that R4 is a sibling replica, otherwise R4 would not be
        # part of the clusterAllReplicasThinkPrimaryIsFail() scan at all.
        wait_for_condition 1000 50 {
            [dict get [cluster_get_node_by_id 0 $R4_id] slaveof] eq $R3_id
        } else {
            fail "R0 does not see R4 as a replica of R3"
        }
        wait_for_cluster_propagation
        wait_for_cluster_state "ok"

        ##########################################################
        # Phase 2: freeze R4 and fail R3. R0 is the only replica
        #          that can actually observe the failure.
        ##########################################################

        # Freezing R4 pins whatever flags R0 last learned about it, and stops its
        # replication offset, so R0 is unambiguously the better ranked replica.
        pause_process [srv -4 pid]

        for {set i 0} {$i < 10} {incr i} {
            R 3 incr key_977613
        }
        wait_for_ofs_sync [srv -3 client] [srv 0 client]

        set loglines [count_log_lines 0]
        pause_process [srv -3 pid]

        # R0 sees R3 fail and schedules its election.
        wait_for_log_messages 0 {"*Start of election delayed for*"} $loglines 1000 50

        # R4 is frozen and never saw R3 die, so not every replica thinks the
        # primary is failing: R0 must take the regular ranked delay.
        #
        # With the stale flag bug, R4 still advertises MY_PRIMARY_FAIL from the
        # first failover, the vote looks unanimous and R0 elects immediately.
        verify_no_log_message 0 "*This is the best ranked replica and can initiate the election immediately*" $loglines
        verify_no_log_message 0 "*Myself become the best ranked replica, initiate the election immediately*" $loglines
        verify_no_log_message 0 "*Start of election delayed for 0 milliseconds*" $loglines

        resume_process [srv -4 pid]
        resume_process [srv -3 pid]
    }
} continuous_slot_allocation stale_flag_replica_allocation ;# start_cluster

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

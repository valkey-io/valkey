# Check that 'cluster-replica-no-failover no-data' prevents a replica whose
# replication offset is still 0 (it never completed a sync with its primary)
# from starting an automatic failover, which would promote an empty node and
# lose all the data of the shard.

# Run the zero-offset-replica failover test for the given sync type. sync_type
# is either "diskless" (the primary never starts the snapshot) or "disk-based"
# (the primary stalls while generating the RDB file), both leaving the new
# replica stuck at replication offset 0.
proc test_zero_offset_replica {sync_type} {
    test "Zero-offset replica cannot fail over ($sync_type)" {
        set R0_nodeid [R 0 CLUSTER MYID]
        set R3_nodeid [R 3 CLUSTER MYID]

        # Fill primary 0 with some data so that later, after the failover promotes
        # an empty replica, we can detect that the shard data was lost.
        for {set i 0} {$i < 1000} {incr i} {
            R 0 set "{key_991803}:$i" x
        }

        # Make the new replica stall at replication offset 0, depending on the sync
        # type under test.
        if {$sync_type eq "diskless"} {
            # Diskless replication with a large sync delay: the primary won't even
            # start transferring the snapshot, so the new replica stays at offset 0.
            R 0 CONFIG SET repl-diskless-sync yes
            R 0 CONFIG SET repl-diskless-sync-delay 1000
        } else {
            # Disk-based replication with a huge per-key save delay: rdb-key-save-delay
            # is applied per key by the RDB child process, so the parent can still run
            # its event loop (handling the replica's CLUSTER REPLICATE and propagating
            # the new role) while the snapshot is never finished. The replica therefore
            # stays at offset 0.
            R 0 CONFIG SET repl-diskless-sync no
            R 0 CONFIG SET rdb-key-save-delay 100000
        }

        # Turn on the guard on the replica: a zero-offset replica must not start an
        # automatic failover. Validity factor 0 lets it become a candidate immediately
        # once the primary is down.
        R 3 CONFIG SET cluster-replica-no-failover no-data
        R 3 CONFIG SET cluster-replica-validity-factor 0

        # Add the empty node as a replica of R0 and wait until the role change is
        # agreed across the cluster (the replica and each primary agree that R3 is a
        # replica of R0).
        R 3 cluster replicate [R 0 CLUSTER MYID]
        wait_for_condition 1000 50 {
            [cluster_node_is_replica_of 0 $R3_nodeid $R0_nodeid] &&
            [cluster_node_is_replica_of 1 $R3_nodeid $R0_nodeid] &&
            [cluster_node_is_replica_of 2 $R3_nodeid $R0_nodeid] &&
            [cluster_node_is_replica_of 3 $R3_nodeid $R0_nodeid]
        } else {
            for {set j 0} {$j < [llength $::servers]} {incr j} {
                puts "R $j cluster nodes output: [R $j cluster nodes]"
            }
            fail "R3 was not consistently recognized as a replica of R0 across the cluster"
        }

        # Take the primary down so the replica should attempt a failover.
        pause_process [srv 0 pid]

        # The replica must refuse to fail over and log the reason. Waiting for the
        # log is a stable signal that it already tried and was blocked.
        wait_for_log_messages -3 {"*Currently unable to failover*Replication offset is 0*"} 0 1000 50

        # Disabling the guard lets the zero-offset replica win the election.
        R 3 CONFIG SET cluster-replica-no-failover no
        wait_for_condition 1000 50 {
            [s -3 role] eq {master}
        } else {
            fail "Replica did not fail over after disabling the guard"
        }

        # The new primary has an empty dataset: the shard data is lost.
        assert_equal 0 [R 3 dbsize]

        # Bring the original primary back. After the failover R3 is the new primary,
        # so R0 should rejoin the cluster as its replica. Both now have an empty
        # dataset, because the failover promoted an empty node.
        if {$sync_type eq "diskless"} {
            R 3 CONFIG SET repl-diskless-sync yes
            R 3 CONFIG SET repl-diskless-sync-delay 0
            resume_process [srv 0 pid]
        } else {
            resume_process [srv 0 pid]
            R 0 CONFIG SET rdb-key-save-delay 0
        }
        wait_for_condition 1000 50 {
            [s 0 role] eq {slave} &&
            [R 0 dbsize] eq 0
        } else {
            fail "Original primary did not rejoin the cluster as a replica of the new primary"
        }
    }
}

start_cluster 3 1 {tags {external:skip cluster}} {
    test_zero_offset_replica diskless
} continuous_slot_allocation no_replica_allocation

start_cluster 3 1 {tags {external:skip cluster}} {
    test_zero_offset_replica disk-based
} continuous_slot_allocation no_replica_allocation

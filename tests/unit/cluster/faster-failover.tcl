# Check the faster failover is working.

# Allocate slot 0 to the first primary and allocate slot 1 to the second primary,
# and then evenly distribute the remaining slots to the remaining primaries.
proc my_slot_allocation {primaries replicas} {
    R 0 cluster addslots 0
    R 1 cluster addslots 1
    R 2 cluster addslotsrange 2 5463
    R 3 cluster addslotsrange 5464 10927
    R 4 cluster addslotsrange 10928 16383
}

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
# Needs to run in the body of
# start_cluster 5 7 {tags {external:skip cluster} overrides {cluster-ping-interval 1000 cluster-node-timeout 5000}} {
proc test_best_ranked_replica {} {
    test "The best replica can initiate an election immediately in an automatic failover" {
        # We calculate their rankings so that we can make rank judgments later.
        set R0_failed_primary_rank [expr [expr [memcmp [R 0 cluster myshardid] [R 1 cluster myshardid]] < 0] ? 0 : 1]
        set R1_failed_primary_rank [expr [expr [memcmp [R 1 cluster myshardid] [R 0 cluster myshardid]] < 0] ? 0 : 1]

        set R6_replica_rank [expr [expr [memcmp [R 6 cluster myid] [R 11 cluster myid]] < 0] ? 0 : 1]
        set R11_replica_rank [expr [expr [memcmp [R 11 cluster myid] [R 6 cluster myid]] < 0] ? 0 : 1]

        set R0_nodeid [R 0 cluster myid]
        set R1_nodeid [R 1 cluster myid]

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

        # Make sure both primaries R0 and R1 are FAIL from the replica's view.
        wait_for_condition 1000 10 {
            [cluster_has_flag [cluster_get_node_by_id 5 $R0_nodeid] fail] eq 1 &&
            [cluster_has_flag [cluster_get_node_by_id 5 $R1_nodeid] fail] eq 1 &&
            [cluster_has_flag [cluster_get_node_by_id 10 $R0_nodeid] fail] eq 1 &&
            [cluster_has_flag [cluster_get_node_by_id 10 $R1_nodeid] fail] eq 1 &&

            [cluster_has_flag [cluster_get_node_by_id 6 $R0_nodeid] fail] eq 1 &&
            [cluster_has_flag [cluster_get_node_by_id 6 $R1_nodeid] fail] eq 1 &&
            [cluster_has_flag [cluster_get_node_by_id 11 $R0_nodeid] fail] eq 1 &&
            [cluster_has_flag [cluster_get_node_by_id 11 $R1_nodeid] fail] eq 1
        } else {
            fail "The node is not marked with the correct flag"
        }

        wait_for_condition 1000 100 {
            [s -10 role] == "master" &&
            ([s -6 role] == "master" || [s -11 role] == "master")
        } else {
            fail "No failover detected"
        }

        # case 1: R0 and R1 down in the same time, R0 have a better failed primary rank, R10 is
        # the best ranked replica in the first place, if so, there is no delay.
        if {$R0_failed_primary_rank == 0} {
            if {[count_log_message -10 "This is the best ranked replica and can initiate the election immediately"] != 0} {
                verify_log_message -10 "*Start of election delayed for 0 milliseconds*" 0
            }
        }
        # No matter what, R10 will be the best ranked replica in R0.
        # This is the best ranked replica and can initiate the election immediately
        # Myself become the best ranked replica, initiate the election immediately
        verify_log_message -10 "*best ranked replica*" 0
        wait_for_log_messages -5 {"*Successful partial resynchronization with primary*"} 0 1000 50

        # case 2: R0 and R1 down in the same time, R1 have a better failed primary rank, R6 or R11
        # will be the best ranked replica in the first place, if so, there is no delay.
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
        # No matter what, there will be a best ranked replica in R1.
        # This is the best ranked replica and can initiate the election immediately
        # Myself become the best ranked replica, initiate the election immediately
        if {$R6_replica_rank == 0} {
            verify_log_message -6 "*best ranked replica*" 0
            wait_for_log_messages -11 {"*Successful partial resynchronization with primary*"} 0 1000 50
        } else {
            verify_log_message -11 "*best ranked replica*" 0
            wait_for_log_messages -6 {"*Successful partial resynchronization with primary*"} 0 1000 50
        }

        # In any case, we do not expect timeouts.
        verify_no_log_message -5 "*Failover attempt expired*" 0
        verify_no_log_message -10 "*Failover attempt expired*" 0
        verify_no_log_message -6 "*Failover attempt expired*" 0
        verify_no_log_message -11 "*Failover attempt expired*" 0
    }
}

# This change and test is quite important, so we want to run it a few more times.
for {set i 0} {$i < 5} {incr i} {
    start_cluster 5 7 {tags {external:skip cluster} overrides {cluster-ping-interval 1000 cluster-node-timeout 5000}} {
        test_best_ranked_replica
    } my_slot_allocation cluster_allocate_replicas ;# start_cluster
}

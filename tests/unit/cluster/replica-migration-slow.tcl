# Check that the status of primary that can be targeted by replica migration
# is acquired again, after being getting slots again, in a cluster where the
# other primaries have replicas.
tags {"slow valgrind:skip"} {
run_solo {cluster-replica-migration-slow} {
start_cluster 5 15 {tags {external:skip cluster} overrides {cluster-allow-replica-migration yes}} {
    test "Primary #0 should re-acquire one or more replicas" {
        # Resharding all the primary #0 slots away from it
        set primary0_id [dict get [cluster_get_myself 0] id]
        set output [exec \
            $::VALKEY_CLI_BIN --cluster rebalance \
            127.0.0.1:[srv 0 port] \
            {*}[valkeycli_tls_config "./tests"] \
            --cluster-weight ${primary0_id}=0 >@ stdout]

        # Primary #0 who lost all slots should turn into a replica without replicas
        wait_for_condition 1000 50 {
            [s 0 role] eq "slave" && [s 0 connected_slaves] == 0
        } else {
            puts [R 0 info replication]
            fail "Primary #0 didn't turn itself into a replica"
        }

        # Resharding back some slot to primary #0
        # Wait for the cluster config to propagate before attempting a
        # new resharding.
        wait_for_cluster_propagation
        set output [exec \
            $::VALKEY_CLI_BIN --cluster rebalance \
            127.0.0.1:[srv 0 port] \
            {*}[valkeycli_tls_config "./tests"] \
            --cluster-weight ${primary0_id}=.01 \
            --cluster-use-empty-masters >@ stdout]

        wait_for_condition 1000 50 {
            [llength [lindex [R 0 role] 2]] >= 1
        } else {
            fail "Primary #0 has no has replicas"
        }
    }
} ;# start_cluster
} ;# run_solo
} ;# tag

# Check that if 'cluster-allow-replica-migration' is set to 'no', replicas do not
# migrate when primary becomes empty.
tags {"slow valgrind:skip"} {
run_solo {cluster-replica-migration-slow} {
start_cluster 5 15 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no}} {
    test "Each primary should have at least two replicas attached" {
        # Resharding all the primary #0 slots away from it
        set primary0_id [dict get [cluster_get_myself 0] id]
        set output [exec \
            $::VALKEY_CLI_BIN --cluster rebalance \
            127.0.0.1:[srv 0 port] \
            {*}[valkeycli_tls_config "./tests"] \
            --cluster-weight ${primary0_id}=0 >@ stdout]

        wait_for_cluster_propagation
        wait_for_cluster_state "ok"

        # Primary #0 still should have its replicas
        assert { [llength [lindex [R 0 role] 2]] >= 2 }

        # Each primary should have at least two replicas attached
        for {set id 0} {$id < 5} {incr id} {
            wait_for_condition 1000 50 {
                [llength [lindex [R $id role] 2]] >= 2
            } else {
                fail "Primary #$id does not have 2 replicas as expected"
            }
        }
    }
} ;# start_cluster
} ;# run_solo
} ;# tag

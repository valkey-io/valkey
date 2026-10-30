# Tests for cluster-voter, which decides whether a node votes in failure
# detection and failover elections independently of its role.

source tests/support/cluster.tcl

proc wait_for_cluster_voters {voters} {
    wait_for_condition 1000 50 {
        [CI 0 cluster_voters] == $voters &&
        [CI 1 cluster_voters] == $voters &&
        [CI 2 cluster_voters] == $voters
    } else {
        fail "cluster_voters didn't converge to $voters"
    }
}

# Returns 1 if instance_id reports node_id as a voter in CLUSTER SHARDS.
proc cluster_node_is_voter {instance_id node_id} {
    foreach shard [R $instance_id CLUSTER SHARDS] {
        foreach node [dict get $shard nodes] {
            if {[dict get $node id] eq $node_id} {
                return [expr {[dict get $node voter] eq {yes}}]
            }
        }
    }
    return 0
}

proc wait_for_voter_flag {node_index has_flag} {
    set node_id [R $node_index CLUSTER MYID]
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        wait_for_condition 1000 50 {
            [cluster_node_is_voter $j $node_id] == $has_flag
        } else {
            fail "Instance $j doesn't see the expected voter flag of instance $node_index"
        }
    }
}

start_cluster 1 2 {tags {external:skip cluster}} {
    test "By default only the primary with slots is a voter" {
        wait_for_cluster_voters 1
        assert_equal 1 [CI 0 cluster_size]
        assert_equal auto [lindex [R 0 CONFIG GET cluster-voter] 1]
        for {set j 0} {$j < 3} {incr j} {
            assert_equal 1 [cluster_node_is_voter $j [R 0 CLUSTER MYID]]
            assert_equal 0 [cluster_node_is_voter $j [R 1 CLUSTER MYID]]
        }
    }

    test "cluster-voter yes makes replicas voters" {
        R 1 CONFIG SET cluster-voter yes
        R 2 CONFIG SET cluster-voter yes
        wait_for_voter_flag 1 1
        wait_for_voter_flag 2 1
        wait_for_cluster_voters 3
        # cluster_size keeps counting primaries with slots only.
        assert_equal 1 [CI 0 cluster_size]
    }

    test "cluster-voter no excludes a primary that owns slots" {
        R 0 CONFIG SET cluster-voter no
        wait_for_voter_flag 0 0
        wait_for_cluster_voters 2
        assert_equal 1 [CI 1 cluster_size]

        set primary_id [R 0 CLUSTER MYID]
        set nodes_conf [cluster_nodes_conf_path 1]
        wait_for_condition 1000 50 {
            [file_has_pattern $nodes_conf "$primary_id\[^\n\]*,voter=no"]
        } else {
            fail "The voter policy of the primary is not saved in nodes.conf"
        }

        R 0 CONFIG SET cluster-voter auto
        wait_for_voter_flag 0 1
        wait_for_cluster_voters 3
        assert_equal 0 [file_has_pattern [cluster_nodes_conf_path 0] "$primary_id\[^\n\]*,voter="]
    }

    test "CLUSTER NODES doesn't expose the voter policy" {
        for {set j 0} {$j < 3} {incr j} {
            foreach node [get_cluster_nodes $j] {
                assert_equal 0 [cluster_has_flag $node voter]
            }
        }
    }

    test "The voter policy is persisted in nodes.conf" {
        set replica_id [R 1 CLUSTER MYID]
        set nodes_conf [cluster_nodes_conf_path 0]
        wait_for_condition 1000 50 {
            [file_has_pattern $nodes_conf "$replica_id\[^\n\]*,voter=yes"]
        } else {
            fail "The voter policy of the replica is not saved in nodes.conf"
        }
    }

    test "The voter policy survives a restart when set in the config file" {
        R 2 CONFIG REWRITE
        restart_server -2 true false
        assert_equal yes [lindex [R 2 CONFIG GET cluster-voter] 1]
        wait_for_voter_flag 2 1
        wait_for_cluster_voters 3
    }

    test "A replica set back to auto stops voting" {
        R 2 CONFIG SET cluster-voter auto
        wait_for_voter_flag 2 0
        wait_for_cluster_voters 2
    }
}

# A single shard where the primary and both replicas are voters: three voters,
# quorum two.
start_cluster 1 2 {tags {external:skip cluster} overrides {cluster-voter yes}} {
    test "A single shard with voter replicas has three voters" {
        wait_for_cluster_voters 3
        for {set j 1} {$j < 3} {incr j} {
            wait_for_sync [srv -$j client]
        }
    }

    test "A primary that can't reach a majority of the voters stops serving" {
        pause_process [srv -1 pid]
        pause_process [srv -2 pid]
        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {fail}
        } else {
            fail "The isolated primary didn't change its state to fail"
        }
        assert_error "*CLUSTERDOWN*" {R 0 SET foo bar}
        resume_process [srv -1 pid]
        resume_process [srv -2 pid]
        wait_for_cluster_state ok
    }

    test "The replicas fail over the primary without other shards" {
        set primary_id [R 0 CLUSTER MYID]
        pause_process [srv 0 pid]
        wait_for_condition 1000 50 {
            [s -1 role] eq {master} || [s -2 role] eq {master}
        } else {
            fail "No replica was promoted"
        }
        if {[s -1 role] eq {master}} {
            set new_primary 1
            set other_replica 2
        } else {
            set new_primary 2
            set other_replica 1
        }
        wait_for_condition 1000 50 {
            [CI $new_primary cluster_state] eq {ok} &&
            [CI $other_replica cluster_state] eq {ok}
        } else {
            fail "The cluster didn't recover after the failover"
        }
        # The voter set doesn't change on failover.
        assert_equal 3 [CI $new_primary cluster_voters]
        assert_equal OK [R $new_primary SET foo bar]

        # The old primary rejoins as a replica of the new one.
        resume_process [srv 0 pid]
        wait_for_condition 1000 50 {
            [s 0 role] eq {slave} &&
            [dict get [cluster_get_myself_primary 0] id] eq [R $new_primary CLUSTER MYID]
        } else {
            fail "The old primary didn't rejoin as a replica"
        }
        wait_for_cluster_state ok
    }
}

start_cluster 1 2 {tags {external:skip cluster} overrides {cluster-voter yes}} {
    test "A failed voter still counts in the quorum" {
        wait_for_cluster_voters 3
        wait_for_sync [srv -1 client]

        set replica2_id [R 2 CLUSTER MYID]
        pause_process [srv -2 pid]
        wait_node_marked_fail 0 $replica2_id
        assert_equal 3 [CI 0 cluster_voters]
        # Two out of three voters are still reachable.
        assert_equal ok [CI 0 cluster_state]

        # With the primary down too, the remaining replica is the only
        # reachable voter, it can't win an election alone.
        pause_process [srv 0 pid]
        wait_for_condition 1000 50 {
            [CI 1 cluster_state] eq {fail}
        } else {
            fail "The remaining replica didn't change its state to fail"
        }
        after [expr {[lindex [R 1 CONFIG GET cluster-node-timeout] 1] * 2}]
        assert_equal slave [s -1 role]

        resume_process [srv 0 pid]
        resume_process [srv -2 pid]
        wait_for_cluster_state ok
        assert_equal master [s 0 role]
    }
}

start_cluster 3 3 {tags {external:skip cluster}} {
    test "A primary set to no doesn't vote" {
        R 2 CONFIG SET cluster-voter no
        wait_for_condition 1000 50 {
            [CI 0 cluster_voters] == 2 &&
            [CI 1 cluster_voters] == 2 &&
            [CI 3 cluster_voters] == 2
        } else {
            fail "The primary set to no is still counted as a voter"
        }
        assert_equal 3 [CI 1 cluster_size]

        # Primary #1 is the only reachable voter left, it can't mark #0 as
        # failed nor authorize a failover on its own.
        set primary0_id [R 0 CLUSTER MYID]
        pause_process [srv 0 pid]
        wait_node_marked_pfail 1 $primary0_id
        after [expr {[lindex [R 1 CONFIG GET cluster-node-timeout] 1] * 2}]
        assert_equal 0 [check_cluster_node_mark fail 1 $primary0_id]
        assert_equal slave [s -3 role]
    }

    test "The failover happens once the primary votes again" {
        R 2 CONFIG SET cluster-voter auto
        wait_for_condition 1000 50 {
            [s -3 role] eq {master}
        } else {
            fail "The replica of the paused primary wasn't promoted"
        }
        resume_process [srv 0 pid]
        wait_for_cluster_state ok
    }
}

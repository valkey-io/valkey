tags {tls:skip external:skip cluster singledb} {
    set base_conf [list cluster-enabled yes save ""] 
    start_multiple_servers 2 [list overrides $base_conf] {
        test "Cluster nodes are reachable" {
            for {set id 0} {$id < [llength $::servers]} {incr id} {
                # Every node should be reachable.
                wait_for_condition 1000 50 {
                    ([catch {R $id ping} ping_reply] == 0) &&
                    ($ping_reply eq {PONG})
                } else {
                    catch {R $id ping} err
                    fail "Node #$id keeps replying '$err' to PING."
                }
            }
        }

        test "Before slots allocation, all nodes report cluster failure" {
            wait_for_cluster_state fail
        }

        test "Cluster nodes haven't met each other" {
            assert {[llength [get_cluster_nodes 1]] == 1}
            assert {[llength [get_cluster_nodes 0]] == 1}
        }

        test "Allocate slots" {
            cluster_allocate_slots 1 1
        }

        test "Restart of node in cluster mode doesn't cause nodes.conf corruption due to shard id mismatch" {
            set primary_id [R 0 CLUSTER MYID]
            R 1 CLUSTER MEET 127.0.0.1 [srv 0 port]
            set pattern *$primary_id*
            wait_for_condition 100 50 {
                [string match $pattern [R 1 CLUSTER NODES]]
            } else {
                fail "Meet took longer"
            }
            R 1 CLUSTER REPLICATE $primary_id
            wait_replica_online [srv 0 client]
            set result [catch {R 0 DEBUG RESTART 0} err]

            if {$result != 0 && $err ne "I/O error reading reply"} {
                fail "Unexpected error restarting server: $err"
            }

             wait_for_condition 100 100 {
                [check_server_response 0] eq 1
            } else {
                fail "Server didn't come back online in time"
            }
        }
    }
}

start_cluster 3 1 {tags {external:skip cluster}} {
    test "The replica will have a new shard_id after cluster reset soft" {
        assert_equal [R 0 cluster myshardid] [R 3 cluster myshardid]

        R 3 cluster reset
        assert_not_equal [R 0 cluster myshardid] [R 3 cluster myshardid]

        wait_for_condition 1000 50 {
            [llength [R 0 cluster shards]] == 4 &&
            [llength [R 1 cluster shards]] == 4 &&
            [llength [R 2 cluster shards]] == 4
        } else {
            fail "R 3 does not become a new shard"
        }
    }
}

proc partition_node {id} {
    R $id DEBUG DROP-CLUSTER-PACKET-FILTER -2
    R $id DEBUG PAUSE-CRON 1
}

proc heal_partition {id} {
    R $id DEBUG DROP-CLUSTER-PACKET-FILTER -1
    R $id DEBUG PAUSE-CRON 0
}

start_cluster 3 6 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no loglevel debug}} {
    test "Don't process slot ownership updates until shard_id is initialized" {
        # Remove one of the replicas from the cluster to be added back again later
        cluster_forget_node 6 9
        assert_equal [R 6 CLUSTER RESET HARD] {OK}

        partition_node 0
        wait_for_condition 1000 50 {
            [s -3 role] eq {master}
        } else {
            fail "Failover to node 3 didn't happen in time"
        }
        assert_equal [R 6 CLUSTER MEET 127.0.0.1 [srv -3 port]] {OK}
        for {set i 0} {$i < 9} {incr i} {
            if {$i == 0} continue
            wait_for_condition 1000 50 {
                [llength [get_cluster_nodes $i connected]] == 8
            } else {
                return 0
            }
        }

        assert_equal [R 6 CLUSTER REPLICATE [R 3 cluster myid]] {OK}
        wait_replica_online [srv -3 client]

        partition_node 3
        wait_for_condition 1000 50 {
            [s -6 role] eq {master}
        } else {
            fail "Failover to node 6 hasn't happened yet"
        }

        heal_partition 0
        wait_for_condition 1000 50 {
            [s 0 role] eq {slave}
        } else {
            fail "node 0 hasn't learnt about the new master"
        }
    }
}
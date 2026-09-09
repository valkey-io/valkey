proc find_slots_for_node {node_idx node_id_to_find} {
    set shards_cfg [R $node_idx CLUSTER SHARDS]
    foreach shard_cfg $shards_cfg {
        set nodes [dict get $shard_cfg nodes]
        foreach node $nodes {
            if {[dict get $node id] eq $node_id_to_find} {
                return [dict get $shard_cfg slots]
            }
        }
    }
    return [list]
}

proc find_primary_node {number_of_shards shard_id} {
    # primary indexes: [0..$number_of_shards-1]
    for {set i 0}  {$i < $number_of_shards} {incr i} {
        if {[R $i CLUSTER MYSHARDID] eq $shard_id} {
            return $i
        }
    }
    fail "Can't find shard $shard_id across $number_of_shards primaries."
}

set number_of_shards 3
start_cluster $number_of_shards $number_of_shards {tags {external:skip cluster}} {
    test "Cluster should start ok" {
        wait_for_cluster_state ok
    }

    test "CLUSTER REPLICATE NO ONE should turn node into empty primary" {
        upvar number_of_shards number_of_shards
        # primary indexes: [0..$number_of_shards-1]
        # replica indexes: [$number_of_shards..2*$number_of_shards-1]
        # We are just taking first replica index for testing.
        set replica_node $number_of_shards
        set replica_node_id [R $replica_node CLUSTER MYID]
        set shard_id [R $replica_node CLUSTER MYSHARDID]
        # make sure node is replica
        wait_for_condition 100 100 {
            [string first "slave" [R $replica_node ROLE]] >= 0
        } else {
            puts "R $replica_node ROLE: [R $replica_node ROLE]"
            fail "R $replica_node didn't assume replica role in time"
        }
        # Adding some data to replica's shard
        set primary_node [find_primary_node $number_of_shards $shard_id]
        wait_for_condition 100 100 {
            ([catch {R $primary_node SET [randomKey] "some-value"} resp] == 0) && ($resp eq {OK})
        } else {
            puts "R $primary_node SCAN 0: [R $primary_node SCAN 0]"
            fail "Failed to add some key to primary node $primary_node in shard $shard_id."
        }
        # and wait for data is replicated to replica
        wait_for_condition 100 100 {
            [R $replica_node DBSIZE] > 0
        } else {
            puts "R $replica_node DBSIZE: [R $replica_node DBSIZE]"
            fail "Replica $replica_node didn't replicate data from primary."
        }

        assert_equal "OK" [R $replica_node CLUSTER REPLICATE NO ONE]

        # make sure node is turned into primary
        wait_for_condition 100 100 {
            [string first "master" [R $replica_node ROLE]] >= 0
        } else {
            puts "R $replica_node ROLE: [R $replica_node ROLE]"
            fail "R $replica_node didn't assume primary role in time"
        }

        # make sure shard id is changed
        assert_not_equal $shard_id [R $replica_node CLUSTER MYSHARDID]
        assert_equal 0 [R $replica_node DBSIZE]

        # checking that new primary has no slots across all nodes
        foreach node_idx {0 1 2 3 4 5} {
            wait_for_condition 100 100 {
                [llength [find_slots_for_node $node_idx $replica_node_id]] == 0
            } else {
                puts "R $node_idx still returns node $replica_node_id owning slots: [find_slots_for_node $node_idx $replica_node_id]"
                fail "R $node_idx didn't refresh topology after detaching replica $replica_node/$replica_node_id"
            }
        }
    }
}
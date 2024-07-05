start_cluster 4 4 {tags {external:skip cluster}} {
    test "Deterministic order of CLUSTER SHARDS response" {
        set node_ids {}
        for {set j 0} {$j < 8} {incr j} {
            set shards_cfg [R $j CLUSTER SHARDS]
            set i 0
            foreach shard_cfg $shards_cfg {
                set nodes [dict get $shard_cfg nodes]
                foreach node $nodes {
                    if {$j == 0} {
                        # Save the node ids from the first node response
                        dict set node_ids $i [dict get $node id]
                    } else {
                        # Verify the order of the node ids is the same as the first node response
                        assert_equal [dict get $node id] [dict get $node_ids $i]
                    }
                    incr i
                }
            }
        }
    }
}

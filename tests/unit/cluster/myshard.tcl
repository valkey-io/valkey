tags {external:skip cluster} {
    start_server [list overrides [list cluster-enabled yes cluster-databases 16]] {
        test {CLUSTER MYSHARD describes a shard with no slots} {
            set shard [r CLUSTER MYSHARD]
            assert_equal [r CLUSTER MYSHARDID] [dict get $shard id]
            assert_equal {} [dict get $shard slots]
            set nodes [dict get $shard nodes]
            assert_equal 1 [llength $nodes]
            assert_equal [r CLUSTER MYID] [dict get [lindex $nodes 0] id]
        }

        test {CLUSTER MYSHARD rejects extra arguments} {
            assert_error {*wrong number of arguments*} {r CLUSTER MYSHARD extra}
        }

        foreach resp {2 3} {
            test "CLUSTER MYSHARD returns fragmented slot ranges with RESP$resp" {
                r HELLO $resp
                r CLUSTER FLUSHSLOTS
                r CLUSTER ADDSLOTSRANGE 0 0 2 4 16383 16383
                # Repeated calls must release temporary slot data, including
                # when MYSHARD and SHARDS are interleaved.
                for {set i 0} {$i < 3} {incr i} {
                    set shard [r CLUSTER MYSHARD]
                    assert_equal {0 0 2 4 16383 16383} [dict get $shard slots]
                    assert_equal [r CLUSTER MYSHARDID] [dict get $shard id]
                    set member [lindex [dict get $shard nodes] 0]
                    assert {[string is wideinteger -strict [dict get $member replication-offset]]}
                    assert_equal [lindex [r CLUSTER SHARDS] 0] $shard
                }
                r HELLO 2
            }
        }

        test {CLUSTER HELP documents MYSHARD} {
            assert {[lsearch -exact [r CLUSTER HELP] MYSHARD] != -1}
        }
    }
}

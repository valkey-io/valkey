tags {external:skip cluster singledb} {
    # Start a cluster with a divergent shard ID configuration
    test "divergent cluster shardid conflict" {
        for {set i 1} {$i <= 4} {incr i} {
            if {$::verbose} { puts "Testing for tests/assets/divergent-shard-$i.conf"; flush stdout;}
            exec cp -f tests/assets/divergent-shard-$i.conf tests/tmp/nodes.conf.divergent
            start_server {overrides {"cluster-enabled" "yes" "cluster-config-file" "../nodes.conf.divergent"}} {
                set shardid [r CLUSTER MYSHARDID]
                set count [exec grep -c $shardid tests/tmp/nodes.conf.divergent];
                assert_equal $count 2 "Expect shard ID to be present twice in the configuration file"
            }
        }
    }
}
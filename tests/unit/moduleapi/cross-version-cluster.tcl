# Test cross version compatibility of cluster.
#
# Use minimal.conf to make sure we don't use any configs not supported on the old version.

# Check that cluster nodes agree about "state", or raise an error.
proc wait_for_cluster_stat {server stat value} {
    wait_for_condition 50 100 {
        [string match "*cluster_stats_$stat:$value*" [R $server CLUSTER INFO]]
    } else {
        fail "Cluster node $server cluster_stats_$stat does not match $value"
    }
}

tags {external:skip needs:other-server cluster modules singledb} {
    # To run this test use the `--other-server-path` parameter and pass in a compatible server path supporting
    # SendClusterMessage module API.
    #
    # ./runtest-moduleapi --single unit/moduleapi/cross-version-cluster --other-server-path tests/tmp/valkey-8-0/valkey-server
    #
    # Test cross version compatibility of cluster module send message API.
    start_server {config "minimal-cluster.conf" start-other-server 1} {
        set testmodule [file normalize tests/modules/cluster.so]
        r MODULE LOAD $testmodule
        set other_node_name [r CLUSTER MYID]

        start_server {config "minimal-cluster.conf"} {
            r MODULE LOAD $testmodule

            test "set up cluster" {
                r CLUSTER MEET [srv -1 host] [srv -1 port]

                # Link establishment requires few PING-PONG between two nodes
                wait_for_condition 50 100 {
                    [string match {*handshake*} [r CLUSTER NODES]] eq 0 &&
                    [string match {*handshake*} [r -1 CLUSTER NODES]] eq 0
                } else {
                    puts [r CLUSTER NODES]
                    puts [r -1 CLUSTER NODES]
                    fail "Cluster meet stuck in handshake state"
                }
            }

            test "Send cluster message via module from latest to other server" {
                assert_equal OK [r test.pingall]
                wait_for_cluster_stat 0 messages_module_sent 1
                wait_for_cluster_stat 1 messages_module_received 1
                wait_for_cluster_stat 1 messages_module_sent 1
                wait_for_cluster_stat 0 messages_module_received 1
            }

            test "Send cluster message via module from other to latest server" {
                assert_equal OK [r -1 test.pingall]
                wait_for_cluster_stat 1 messages_module_sent 2
                wait_for_cluster_stat 0 messages_module_received 2
                wait_for_cluster_stat 0 messages_module_sent 2
                wait_for_cluster_stat 1 messages_module_received 2
            }
        }
    }
}

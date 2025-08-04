# Test cross version compatibility of cluster.
#
# Use minimal.conf to make sure we don't use any configs not supported on the old version.

# To run this test use the `--other-server-path` parameter and pass in a compatible server path.
#
# ./runtest --single unit/cluster/cross-version-cluster --other-server-path tests/tmp/valkey-7-2/valkey-server

tags {external:skip needs:other-server cluster singledb} {
    test "Cross version cluster - failover" {

        # Test cluster failover on shutdown is prevented when old replicas exist
        start_cluster 3 1 {tags {external:skip cluster} overrides {cluster-ping-interval 1000}} {
            set primary [srv 0 client]
            set primary_host [srv 0 host]
            set primary_port [srv 0 port]
            set primary_id [$primary cluster myid]

            start_server {config "minimal-cluster.conf" start-other-server 1 overrides {cluster-ping-interval 1000}} {
                # Add a replica of the old version to the cluster
                r cluster meet $primary_host $primary_port
                wait_for_cluster_propagation
                r cluster replicate $primary_id
                wait_for_cluster_state "ok"

                # Make sure the primary won't do the auto-failover.
                catch {$primary shutdown nosave failover}
                verify_log_message -1 "*Unable to perform auto failover on shutdown since there are legacy replicas*" 0
            }
        }
    }
}

tags {external:skip needs:other-server cluster singledb compatible-redis} {
    test "Cross version cluster - PING/PONG" {
        start_server {config "minimal-cluster-legacy.conf" start-other-server 1} {
            set other_node_name [r CLUSTER MYID]

            start_server {config "minimal-cluster.conf"} {
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
        }
    }
}
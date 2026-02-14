start_server {tags {"multi-master external:skip"} overrides {save {} active-replica yes multi-master yes replica-read-only no}} {
start_server {overrides {save {} active-replica yes multi-master yes replica-read-only no}} {
start_server {overrides {save {} active-replica yes multi-master yes replica-read-only no}} {
start_server {overrides {save {} active-replica yes multi-master yes replica-read-only no}} {
    set replica [srv -3 client]
    set p1 [srv -2 client]
    set p1_host [srv -2 host]
    set p1_port [srv -2 port]
    set p2 [srv -1 client]
    set p2_host [srv -1 host]
    set p2_port [srv -1 port]
    set p3 [srv 0 client]
    set p3_host [srv 0 host]
    set p3_port [srv 0 port]

    test {Connect to first upstream and replicate} {
        $replica replicaof add $p1_host $p1_port
        wait_for_condition 100 100 {
            [s -3 master_link_status] eq {up} &&
            [s -3 master_host] eq $p1_host
        } else {
            fail "replica did not connect to upstream #1"
        }

        $p1 set mmc:key1 v1
        wait_for_condition 100 100 {
            [$replica get mmc:key1] eq {v1}
        } else {
            fail "replica did not receive data from upstream #1"
        }
    }

    test {Connect to additional upstreams} {
        $replica replicaof add $p2_host $p2_port
        $replica replicaof add $p3_host $p3_port
        assert_equal 3 [s -3 configured_upstreams]
    }

    test {Switch to second upstream and replicate} {
        $replica replicaof remove $p1_host $p1_port
        wait_for_condition 150 100 {
            [s -3 master_host] eq $p2_host &&
            [s -3 master_link_status] eq {up}
        } else {
            fail "replica did not fail over to upstream #2"
        }

        $p2 set mmc:key2 v2
        wait_for_condition 100 100 {
            [$replica get mmc:key2] eq {v2}
        } else {
            fail "replica did not receive data from upstream #2"
        }
    }

    test {Switch to third upstream and replicate} {
        $replica replicaof remove $p2_host $p2_port
        wait_for_condition 150 100 {
            [s -3 master_host] eq $p3_host &&
            [s -3 master_link_status] eq {up}
        } else {
            fail "replica did not fail over to upstream #3"
        }

        $p3 set mmc:key3 v3
        wait_for_condition 100 100 {
            [$replica get mmc:key3] eq {v3}
        } else {
            fail "replica did not receive data from upstream #3"
        }
    }
}}}}

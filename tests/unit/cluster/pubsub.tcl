# Test PUBLISH propagation across the cluster.

start_cluster 5 5 {tags {external:skip cluster}} {

# Helper procedure to check if all subscriber nodes have received the cluster publish message.
proc all_nodes_received_publish {instance instances initial_received_var} {
    upvar 1 $initial_received_var initial_received
    for {set j 0} {$j < $instances} {incr j} {
        if {$j != $instance} {
            set cur [CI $j cluster_stats_messages_publish_received]
            if {$cur eq ""} { set cur 0 }
            if {$cur <= $initial_received($j)} {
                return 0
            }
        }
    }
    return 1
}

proc test_cluster_publish {instance instances} {
    # Ensure the publishing instance has established connections to all other cluster nodes.
    wait_for_condition 50 100 {
        [llength [lsearch -all -inline [split [R $instance cluster nodes] "\n"] "*connected*"]] >= $instances
    } else {
        fail "Node $instance did not connect to all cluster nodes"
    }

    # Subscribe on dedicated clients for all instances except the publishing node.
    set sub_clients {}
    for {set j 0} {$j < $instances} {incr j} {
        if {$j != $instance} {
            set sc [valkey_deferring_client -$j]
            $sc deferred 1
            $sc subscribe testchannel
            $sc read; # Read the subscribe reply
            dict set sub_clients $j $sc

            set initial_received($j) [CI $j cluster_stats_messages_publish_received]
            if {$initial_received($j) eq ""} { set initial_received($j) 0 }
        }
    }

    set data [randomValue]
    R $instance PUBLISH testchannel $data

    # Wait for the publish message to propagate across the cluster bus to all nodes.
    wait_for_condition 100 100 {
        [all_nodes_received_publish $instance $instances initial_received]
    } else {
        fail "Publish message did not propagate to all cluster nodes"
    }

    # Read the message back from all the subscriber nodes.
    for {set j 0} {$j < $instances} {incr j} {
        if {$j != $instance} {
            set sc [dict get $sub_clients $j]
            set msg [$sc read]
            assert {$data eq [lindex $msg 2]}
            $sc unsubscribe testchannel
            $sc read; # Read the unsubscribe reply
            $sc deferred 0
            $sc close
        }
    }
}

test "Test publishing to master" {
    test_cluster_publish 0 10
}

test "Test publishing to slave" {
    test_cluster_publish 5 10
}
} ;# start_cluster

start_cluster 3 0 {tags {external:skip cluster}} {
    test "Test cluster info stats for publish" {
        R 0 CONFIG RESETSTAT
        R 1 CONFIG RESETSTAT
        R 2 CONFIG RESETSTAT

        R 0 PUBLISH hello world
        assert_equal 2 [CI 0 cluster_stats_messages_publish_sent]
        wait_for_condition 50 100 {
            [CI 1 cluster_stats_messages_publish_received] eq 1 &&
            [CI 2 cluster_stats_messages_publish_received] eq 1 &&
            [CI 1 cluster_stats_pubsub_bytes_received] > 0 &&
            [CI 2 cluster_stats_pubsub_bytes_received] > 0
        } else {
            fail "node 2 or node 3 didn't receive clusterbus publish packet"
        }

        set sender_pubsub_bytes [CI 0 cluster_stats_pubsub_bytes_sent]
        set receiver_pubsub_bytes [expr {[CI 1 cluster_stats_pubsub_bytes_received] + [CI 2 cluster_stats_pubsub_bytes_received]}]
        assert_morethan $sender_pubsub_bytes 0
        assert_equal $sender_pubsub_bytes $receiver_pubsub_bytes
    }
}

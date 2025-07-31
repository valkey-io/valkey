# Test cluster bus traffic statistics

start_cluster 3 0 {tags {external:skip cluster "modules"}} {

test "Cluster should start ok" {
    wait_for_cluster_state ok
}

set primary1 [srv 0 "client"]
set primary2 [srv -1 "client"] 
set primary3 [srv -2 "client"]

test "Initial cluster bus stats should be zero or positive" {
    set info1 [$primary1 cluster info]
    assert {[CI 0 cluster_bus_admin_bytes_sent] >= 0}
    assert {[CI 0 cluster_bus_admin_bytes_received] >= 0}
    assert {[CI 0 cluster_bus_pubsub_bytes_sent] >= 0}
    assert {[CI 0 cluster_bus_pubsub_bytes_received] >= 0}
}

test "Admin traffic increases with cluster operations" {
    set admin_sent_before [CI 0 cluster_bus_admin_bytes_sent]
    set admin_recv_before [CI 0 cluster_bus_admin_bytes_received]
    
    # Force cluster communication with CLUSTER NODES
    $primary1 cluster nodes
    $primary2 cluster nodes
    
    # Wait a bit for cluster bus activity
    after 100
    
    set admin_sent_after [CI 0 cluster_bus_admin_bytes_sent]
    set admin_recv_after [CI 0 cluster_bus_admin_bytes_received]
    
    assert {$admin_sent_after > $admin_sent_before}
    assert {$admin_recv_after > $admin_recv_before}
}

test "Pub/sub traffic increases with publish operations" {
    set pubsub_sent_before [CI 0 cluster_bus_pubsub_bytes_sent]
    set pubsub_recv_before [CI 1 cluster_bus_pubsub_bytes_received]
    
    # Subscribe on one node
    set sub_client [valkey_client -1]
    $sub_client subscribe testchan
    
    # Publish from another node
    $primary1 publish testchan "test message"
    
    # Wait for propagation
    after 100
    
    set pubsub_sent_after [CI 0 cluster_bus_pubsub_bytes_sent]
    set pubsub_recv_after [CI 1 cluster_bus_pubsub_bytes_received]
    
    assert {$pubsub_sent_after > $pubsub_sent_before}
    assert {$pubsub_recv_after > $pubsub_recv_before}
    
    $sub_client close
}

test "Module cluster message traffic tracking" {
    # Load the hellocluster module on all nodes
    set testmodule [file normalize src/modules/hellocluster.so]
    r module load $testmodule
    
    # Send module cluster message to generate traffic
    $primary1 HELLOCLUSTER.PINGALL
    
    # Wait for message processing
    after 100
    
    # Check sent bytes on sender (primary1)
    set cluster_info_sender [$primary1 cluster info]
    set sent_found 0
    foreach line [split $cluster_info_sender "\r\n"] {
        if {[string match "cluster_bus_module_sent_bytes_*" $line]} {
            set sent_found 1
            set bytes [lindex [split $line ":"] 1]
            assert {$bytes >= 0}
        }
        if {[string match "cluster_bus_module_received_bytes_*" $line]} {
            set recv_found 1
            set bytes [lindex [split $line ":"] 1]
            assert {$bytes >= 0}
        }
    }

    # Verify that both sent and received module traffic were tracked
    assert {$sent_found == 1}
    assert {$recv_found == 1}
}

}
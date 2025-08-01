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
    # Load the cluster module on all nodes
    set testmodule [file normalize tests/modules/cluster.so]
    r module load $testmodule
    
    # Send module cluster message to generate traffic
    r test.pingall
    
    # Wait for message processing
    after 100
    
    # Check sent bytes on sender (primary1)
    set cluster_info_sender [r cluster info]
    set sent_found 0
    set recv_found 0
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

    r module unload cluster

    # Verify module was unloaded successfully
    set modules [r module list]
    set cluster_found 0
    foreach module $modules {
        if {[dict get $module name] eq "cluster"} {
            set cluster_found 1
        }
    }
    assert {$cluster_found == 0}

    # Check that module traffic entries are cleaned up after unload
    set cluster_info_after [r cluster info]
    set module_traffic_found_after 0
    foreach line [split $cluster_info_after "\r\n"] {
        if {[string match "cluster_bus_module_*_bytes_*" $line]} {
            set module_traffic_found_after 1
            break
        }
    }

    # Verify module traffic was present before and absent after unload
    assert {$module_traffic_found_after == 0}
}

}
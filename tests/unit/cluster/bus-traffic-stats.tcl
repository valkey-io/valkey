# Test cluster bus traffic statistics

start_cluster 3 0 {tags {external:skip cluster modules}} {

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

proc check_module_traffic_exists {} {
    set cluster_info [r cluster info]
    set sent_found 0
    set recv_found 0
    foreach line [split $cluster_info "\r\n"] {
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
    return [expr {$sent_found && $recv_found}]
}

proc check_module_traffic_absent {} {
    set cluster_info [r cluster info]
    foreach line [split $cluster_info "\r\n"] {
        if {[string match "cluster_bus_module_*_bytes_*" $line]} {
            return 0
        }
    }
    return 1
}

proc retry_until {condition {max_retries 10} {delay 100}} {
    for {set retry 0} {$retry < $max_retries} {incr retry} {
        if {[uplevel 1 $condition]} {
            return 1
        }
        after $delay
    }
    return 0
}

test "Module cluster message traffic tracking" {
    # Build the cluster module if it doesn't exist
    set testmodule [file normalize tests/modules/cluster.so]
    if {![file exists $testmodule]} {
        exec make -C tests/modules cluster.so
    }

    # Load the cluster module on all nodes
    r module load $testmodule

    # Wait longer for module load and cluster stabilization
    after 1000

    # Send module cluster message to generate traffic
    r test.pingall
    
    # Wait longer for message processing in sanitizer builds
    after 1000
    
    # Check that module traffic entries exist with retries
    assert {[retry_until check_module_traffic_exists]}

    # Unload module from all nodes
    r module unload cluster

    # Wait longer for cleanup to propagate after module unload
    after 1000

    # Verify module was unloaded successfully
    set modules [r module list]
    set cluster_found 0
    foreach module $modules {
        if {[dict get $module name] eq "cluster"} {
            set cluster_found 1
        }
    }
    assert {$cluster_found == 0}

    # Check that module traffic entries are cleaned up after unload with retries
    assert {[retry_until check_module_traffic_absent]}
}

}
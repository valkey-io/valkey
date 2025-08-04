# # Test cluster bus module message traffic tracking for both light and regular messages

# # Helper function to get cluster info with retries
# proc get_cluster_info_with_retries {client {max_retries 5} {retry_delay 200}} {
#     set cluster_info {}
#     set retry_count 0
#     while {$cluster_info eq {} && $retry_count < $max_retries} {
#         if {[catch {$client cluster info} cluster_info]} {
#             incr retry_count
#             after $retry_delay
#             continue
#         }
#         break
#     }
#     if {$cluster_info eq {}} {
#         error "Failed to get cluster info after $max_retries retries"
#     }
#     return $cluster_info
# }

# # Helper function to extract module traffic stats from cluster info
# proc get_module_traffic_stats {cluster_info} {
#     set stats {}
#     foreach line [split $cluster_info "\r\n"] {
#         if {[string match "cluster_bus_module_sent_bytes_*" $line] || 
#             [string match "cluster_bus_module_received_bytes_*" $line]} {
#             set parts [split $line ":"]
#             if {[llength $parts] == 2} {
#                 set key [lindex $parts 0]
#                 set value [lindex $parts 1]
#                 dict set stats $key $value
#             }
#         }
#     }
#     return $stats
# }

# start_cluster 3 0 {tags {external:skip cluster modules}} {

# test "Cluster should start ok" {
#     wait_for_cluster_state ok
# }

# set primary1 [srv 0 "client"]
# set primary2 [srv -1 "client"] 
# set primary3 [srv -2 "client"]

# test "Initial cluster bus module traffic should not be present" {
#     set info1 [$primary1 cluster info]
#     set stats [get_module_traffic_stats $info1]
    
#     # Initially there should be no module traffic stats
#     assert {[dict size $stats] == 0}
# }

# test "Module cluster message traffic tracking with both light and regular headers" {
#     # Build the cluster module if it doesn't exist
#     set testmodule [file normalize tests/modules/cluster.so]
#     if {![file exists $testmodule]} {
#         exec make -C tests/modules cluster.so
#     }

#     # Load the cluster module on all nodes
#     $primary1 module load $testmodule
#     $primary2 module load $testmodule  
#     $primary3 module load $testmodule

#     # Wait for module load and cluster stabilization
#     after 1000

#     # Get baseline traffic stats (should be zero)
#     set baseline_info [get_cluster_info_with_retries $primary1]
#     set baseline_stats [get_module_traffic_stats $baseline_info]
    
#     # Send multiple pingall commands to generate module traffic
#     # This should generate both light and regular module messages depending on node capabilities
#     for {set i 0} {$i < 3} {incr i} {
#         $primary1 test.pingall
#         after 200
#         $primary2 test.pingall  
#         after 200
#         $primary3 test.pingall
#         after 200
#     }
    
#     # Wait for message processing
#     after 2000

#     # Check cluster info for module traffic stats
#     set cluster_info [get_cluster_info_with_retries $primary1]
#     set stats [get_module_traffic_stats $cluster_info]
    
#     # Verify that module traffic entries exist
#     set sent_found 0
#     set recv_found 0
#     set total_sent_bytes 0
#     set total_recv_bytes 0
    
#     dict for {key value} $stats {
#         if {[string match "cluster_bus_module_sent_bytes_*" $key]} {
#             set sent_found 1
#             incr total_sent_bytes $value
#             assert {$value >= 0}
#         }
#         if {[string match "cluster_bus_module_received_bytes_*" $key]} {
#             set recv_found 1  
#             incr total_recv_bytes $value
#             assert {$value >= 0}
#         }
#     }

#     # Verify that both sent and received module traffic were tracked
#     assert {$sent_found == 1}
#     assert {$recv_found == 1}
#     assert {$total_sent_bytes >= 0}
#     assert {$total_recv_bytes >= 0}
    
#     # Verify traffic increased from baseline
#     if {[dict size $baseline_stats] > 0} {
#         set baseline_sent 0
#         set baseline_recv 0
#         dict for {key value} $baseline_stats {
#             if {[string match "cluster_bus_module_sent_bytes_*" $key]} {
#                 incr baseline_sent $value
#             }
#             if {[string match "cluster_bus_module_received_bytes_*" $key]} {
#                 incr baseline_recv $value  
#             }
#         }
#         assert {$total_sent_bytes > $baseline_sent}
#         assert {$total_recv_bytes > $baseline_recv}
#     }
# }

# test "Module traffic tracking persists across multiple message exchanges" {
#     # Get current traffic stats
#     set info_before [get_cluster_info_with_retries $primary2]
#     set stats_before [get_module_traffic_stats $info_before]
    
#     set sent_before 0
#     set recv_before 0
#     dict for {key value} $stats_before {
#         if {[string match "cluster_bus_module_sent_bytes_*" $key]} {
#             incr sent_before $value
#         }
#         if {[string match "cluster_bus_module_received_bytes_*" $key]} {
#             incr recv_before $value
#         }
#     }
    
#     # Send more messages to generate additional traffic
#     for {set i 0} {$i < 5} {incr i} {
#         $primary2 test.pingall
#         after 150
#     }
    
#     # Wait for processing
#     after 1500
    
#     # Check that traffic stats increased
#     set info_after [get_cluster_info_with_retries $primary2]  
#     set stats_after [get_module_traffic_stats $info_after]
    
#     set sent_after 0
#     set recv_after 0
#     dict for {key value} $stats_after {
#         if {[string match "cluster_bus_module_sent_bytes_*" $key]} {
#             incr sent_after $value
#         }
#         if {[string match "cluster_bus_module_received_bytes_*" $key]} {
#             incr recv_after $value
#         }
#     }
    
#     # Traffic should have increased
#     assert {$sent_after > $sent_before}
#     # recv might be same if no responses were received
#     assert {$recv_after >= $recv_before}
# }

# test "Module traffic stats are correctly associated with module name" {
#     set cluster_info [get_cluster_info_with_retries $primary3]
#     set stats [get_module_traffic_stats $cluster_info]
    
#     # The cluster module should appear in the traffic stats
#     set cluster_module_found 0
#     dict for {key value} $stats {
#         if {[string match "*cluster*" $key]} {
#             set cluster_module_found 1
#             assert {$value > 0}
#         }
#     }
    
#     assert {$cluster_module_found == 1}
# }

# test "Module unload cleans up traffic stats" {
#     # Unload module from all nodes
#     $primary1 module unload cluster
#     $primary2 module unload cluster
#     $primary3 module unload cluster

#     # Wait for cleanup to propagate
#     after 1000

#     # Verify modules were unloaded successfully
#     set modules [$primary1 module list]
#     set cluster_found 0
#     foreach module $modules {
#         if {[dict get $module name] eq "cluster"} {
#             set cluster_found 1
#         }
#     }
#     assert {$cluster_found == 0}
    
#     # Verify no new module traffic is generated after unload
#     set info_before [get_cluster_info_with_retries $primary1]
#     set stats_before [get_module_traffic_stats $info_before]
    
#     # Try to send a command that would generate module traffic (should fail)
#     assert_error "*unknown command*" {$primary1 test.pingall}
    
#     # Wait a bit and verify stats didn't change
#     after 500
#     set info_after [get_cluster_info_with_retries $primary1]
#     set stats_after [get_module_traffic_stats $info_after]
    
#     # Stats should be identical since no new traffic was generated
#     assert {[dict size $stats_before] == [dict size $stats_after]}
#     dict for {key value} $stats_before {
#         assert {[dict get $stats_after $key] == $value}
#     }
# }

# }

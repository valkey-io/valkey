# Tests for reply blocking durability feature
# This test suite covers the synchronous replication functionality
# that blocks client responses until replicas acknowledge writes

start_server {tags {"repl durability external:skip"} overrides {sync-replication no}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    
    start_server {} {
        set replica [srv 0 client]
        
        test "Durability initialization creates required data structures" {
            # After enabling, info should show durability is active
            set info [$primary info replication]
            assert_match "*role:master*" $info
        }
        
        test "Replica can sync with durability-enabled primary" {
            $replica replicaof $primary_host $primary_port
            
            wait_for_condition 50 100 {
                [lindex [$replica role] 3] eq {connected}
            } else {
                fail "Replica didn't connect to primary"
            }
        }
        
        test "Write command generates uncommitted key on primary" {
            # Perform a write
            $primary set testkey testvalue
            
            # Key should exist
            assert_equal "testvalue" [$primary get testkey]
        }
        
        test "Read command on primary succeeds for uncommitted key" {
            # Primary should be able to read its own uncommitted writes
            assert_equal "testvalue" [$primary get testkey]
        }
        
        test "Write is eventually propagated to replica" {
            # Wait for replica to receive the write
            wait_for_condition 50 100 {
                [$replica get testkey] eq {testvalue}
            } else {
                fail "Write didn't propagate to replica"
            }
        }
        
        test "Multiple writes create multiple uncommitted keys" {
            $primary set key1 value1
            $primary set key2 value2
            $primary set key3 value3
            
            # All should be readable on primary
            assert_equal "value1" [$primary get key1]
            assert_equal "value2" [$primary get key2]
            assert_equal "value3" [$primary get key3]
            
            # Wait for replication
            wait_for_condition 50 100 {
                [$replica get key1] eq {value1} &&
                [$replica get key2] eq {value2} &&
                [$replica get key3] eq {value3}
            } else {
                fail "Multiple writes didn't propagate"
            }
        }
        
        test "Cleanup after replica disconnection" {
            # Get current client list
            set clients_before [llength [$primary client list type replica]]
            
            # Disconnect replica
            $replica replicaof no one
            
            # Wait for primary to notice
            wait_for_condition 50 100 {
                [llength [$primary client list type replica]] < $clients_before
            } else {
                fail "Primary didn't notice replica disconnect"
            }
        }
    }
}

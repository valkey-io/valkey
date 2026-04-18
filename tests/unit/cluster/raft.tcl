# Test Raft handshake and basic commands

proc cluster_has_node_handshaking_and_connected {myself_id target_id} {
    set target_name [R $target_id CLUSTER MYID]
    set nodes [R $myself_id CLUSTER NODES]
    foreach line [split $nodes "\n"] {
        if {$line eq ""} continue
        set fields [split $line " "]
        set id [lindex $fields 0]
        set flags [lindex $fields 2]
        set status [lindex $fields 7]
        if {$id eq $target_name} {
            if {[string match "*handshake*" $flags] && $status eq "connected"} {
                return 1
            }
        }
    }
    return 0
}

tags {tls:skip external:skip cluster singledb} {

set base_conf [list cluster-enabled yes cluster-raft-enabled yes]
start_multiple_servers 2 [list overrides $base_conf] {

test "Raft nodes are reachable" {
    for {set id 0} {$id < [llength $::servers]} {incr id} {
        wait_for_condition 1000 50 {
            ([catch {R $id ping} ping_reply] == 0) &&
            ($ping_reply eq {PONG})
        } else {
            fail "Node #$id keeps replying to PING."
        }
    }
}

test "Raft Handshake test" {
    set port1 [srv -1 port]
    set host1 [srv -1 host]
    
    # Node 0 meets Node 1
    R 0 CLUSTER MEET $host1 $port1
    
    # Wait for handshake to complete (links connected, but stays in handshake state)
    wait_for_condition 5000 100 {
        [cluster_has_node_handshaking_and_connected 0 1]
    } else {
        fail "Handshake did not complete or links not connected"
    }
}

test "CLUSTER INFO shows Raft info" {
    set info [R 0 CLUSTER INFO]
    assert_match "*raft_role:*" $info
    assert_match "*raft_node_count:2*" $info
}

test "CLUSTER NODES shows connected nodes" {
    set port0 [srv 0 port]
    set host0 [srv 0 host]
    set port1 [srv -1 port]
    set host1 [srv -1 host]
    set nodes [R 0 CLUSTER NODES]
    assert_match "*$host1:$port1*" $nodes
    set id0 [R 0 CLUSTER MYID]
    set id1 [R 1 CLUSTER MYID]
    set found0 0
    set found1 0
    foreach line [split $nodes "\n"] {
        if {$line eq ""} continue
        set fields [split $line " "]
        set id [lindex $fields 0]
        if {$id eq $id1} {
            set found1 1
            set addr [lindex $fields 1]
            set flags [lindex $fields 2]
            set status [lindex $fields 7]
            
            # Verify address contains host and port
            assert_match "*$host1:$port1*" $addr
            
            # Verify flags contain handshake
            assert_match "*handshake*" $flags
            
            # Verify status is connected
            assert_equal "connected" $status
        }
        if {$id eq $id0} {
            set found0 1
            set addr [lindex $fields 1]
            set flags [lindex $fields 2]
            set status [lindex $fields 7]
            
            # Verify address contains host and port
            assert_match "*$host0:$port0*" $addr
            
            # Verify flags contain myself flag
            assert_match "*myself*" $flags
            
            # Verify status is connected
            assert_equal "connected" $status
        }
    }
    assert {$found0 == 1}
    assert {$found1 == 1}
}

} ;# stop servers

test "Raft HANDSHAKING Node Timeout test" {
    start_server {overrides {cluster-enabled yes cluster-raft-enabled yes cluster-node-timeout 1000}} {
        set port 9999 ;# Random port likely not in use
        
        # Meet a non-existent node
        r CLUSTER MEET 127.0.0.1 $port
        
        # Verify node is added and in handshake state
        set nodes [r CLUSTER NODES]
        assert_match "*handshake*" $nodes
        
        # Wait for timeout (more than 1000ms)
        after 2000
        
        # Verify node is removed
        set nodes [r CLUSTER NODES]
        assert_no_match "*127.0.0.1:$port*" $nodes
    }
}

test "Raft EXTERNAL Node Timeout test" {
    set base_conf [list cluster-enabled yes cluster-raft-enabled yes]
    start_multiple_servers 2 [list overrides $base_conf] {
        set port1 [srv -1 port]
        set host1 [srv -1 host]
        
        # Node 0 meets Node 1
        R 0 CLUSTER MEET $host1 $port1
        
        # Wait for handshake to complete (Node 1 becomes EXTERNAL in Node 0)
        wait_for_condition 5000 100 {
            [cluster_has_node_handshaking_and_connected 0 1]
        } else {
            fail "Handshake did not complete or links not connected"
        }
        
        # Now set short timeout to trigger it
        R 0 CONFIG SET cluster-node-timeout 1000
        
        # Wait for timeout (more than 1000ms)
        after 2000
        
        # Verify Node 1 is removed from Node 0's view
        set nodes [R 0 CLUSTER NODES]
        assert_no_match "*$host1:$port1*" $nodes
    }
}

} ;# tags

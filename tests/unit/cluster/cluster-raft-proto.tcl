# Test the raft cluster bus protocol by simulating cluster nodes from Tcl.
# A single valkey-server is started and we connect to its cluster bus port,
# speaking the raft wire protocol directly.

# Build a raft wire message: "RAFT" + 4-byte big-endian length + payload.
proc raft_msg {payload} {
    set len [expr {8 + [string length $payload]}]
    set hdr "RAFT"
    append hdr [binary format I $len]
    append hdr $payload
    return $hdr
}

# Connect to a node's cluster bus port and return the socket.
proc raft_connect {host port} {
    set fd [socket $host $port]
    fconfigure $fd -translation binary -buffering full
    return $fd
}

# Send a raft message on a cluster bus connection.
proc raft_send {fd payload} {
    puts -nonewline $fd [raft_msg $payload]
    flush $fd
}

# Read a raft message from a cluster bus connection. Returns the payload.
proc raft_recv {fd {timeout 5000}} {
    # Read 8-byte header
    fconfigure $fd -blocking 0
    set deadline [expr {[clock milliseconds] + $timeout}]
    set hdr ""
    while {[string length $hdr] < 8} {
        append hdr [read $fd [expr {8 - [string length $hdr]}]]
        if {[string length $hdr] < 8} {
            if {[clock milliseconds] > $deadline} {
                error "timeout reading raft header"
            }
            after 10
        }
    }
    if {[string range $hdr 0 3] ne "RAFT"} {
        error "bad raft header: [string range $hdr 0 3]"
    }
    binary scan [string range $hdr 4 7] I totlen
    set paylen [expr {$totlen - 8}]
    set payload ""
    while {[string length $payload] < $paylen} {
        append payload [read $fd [expr {$paylen - [string length $payload]}]]
        if {[string length $payload] < $paylen} {
            if {[clock milliseconds] > $deadline} {
                error "timeout reading raft payload (got [string length $payload]/$paylen)"
            }
            after 10
        }
    }
    return $payload
}

tags {tls:skip external:skip cluster singledb} {

# Listen on a random port, accept one connection, close the listener.
# Returns the accepted client fd. Sets the listen port in the upvar port_var.
proc raft_listen_and_accept {port_var {timeout 5000}} {
    upvar $port_var listen_port
    set ::_raft_accepted ""
    proc _raft_on_accept {fd addr port} {
        fconfigure $fd -translation binary -buffering full
        set ::_raft_accepted $fd
    }
    set listen_fd [socket -server _raft_on_accept -myaddr 127.0.0.1 0]
    set listen_port [lindex [fconfigure $listen_fd -sockname] 2]
    after $timeout {set ::_raft_accepted timeout}
    vwait ::_raft_accepted
    close $listen_fd
    if {$::_raft_accepted eq "timeout"} {
        error "timeout waiting for connection"
    }
    return $::_raft_accepted
}

# Parse an AE message and reply with AE_ACK.
# Returns the last log index after applying the entries.
proc raft_reply_ae_ack {fd ae_msg repl_offset} {
    set lines [split $ae_msg "\n"]
    set fields [split [lindex $lines 0] " "]
    # AE <leader-id> <term> <prev-log-idx> <prev-log-term> <commit> <count>
    set term [lindex $fields 2]
    set prev_idx [lindex $fields 3]
    set count [lindex $fields 6]
    set last_index [expr {$prev_idx + $count}]
    raft_send $fd "AE_ACK $term 1 $last_index $repl_offset"
    return $last_index
}

test "Raft proto: connect to cluster bus and exchange HELLO" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft}} {
        set port [srv 0 port]
        set cport [expr {$port + 10000}]
        set node_id [R 0 CLUSTER MYID]

        # Generate a fake node ID for our simulated node.
        set fake_id [string repeat "a" 40]
        set fake_addr "127.0.0.1:9999@19999,,tls-port=0,shard-id=[string repeat b 40]"

        # Connect to the cluster bus.
        set fd [raft_connect 127.0.0.1 $cport]

        # Send HELLO.
        raft_send $fd "HELLO $fake_id $fake_addr 1 3 1"

        # Expect HI back (response to our HELLO).
        set reply [raft_recv $fd]
        assert_match "HI *" $reply

        close $fd
    }
}

test "Raft proto: singleton clears leader on step-down from HELLO" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft}} {
        set port [srv 0 port]
        set cport [expr {$port + 10000}]

        # Singleton is its own leader.
        assert_equal [R 0 CLUSTER MYID] [CI 0 cluster_raft_leader]

        # Send HELLO from a fake node (simulating a MEET target receiving HELLO).
        set fake_id [string repeat "c" 40]
        set fake_addr "127.0.0.1:9998@19998,,tls-port=0,shard-id=[string repeat d 40]"
        set fd [raft_connect 127.0.0.1 $cport]
        raft_send $fd "HELLO $fake_id $fake_addr 1 3 1"
        set reply [raft_recv $fd]
        assert_match "HI *" $reply
        close $fd

        # After step-down, leader should be empty (unknown).
        assert_equal "" [CI 0 cluster_raft_leader]
        assert_equal "learner" [CI 0 cluster_raft_role]
    }
}

test "Raft proto: singleton clears leader on step-down from HI" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft}} {
        set port [srv 0 port]
        set cport [expr {$port + 10000}]

        # Singleton is its own leader.
        assert_equal [R 0 CLUSTER MYID] [CI 0 cluster_raft_leader]

        # Send HI from a fake non-singleton (cluster_size=3).
        set fake_id [string repeat "e" 40]
        set fake_addr "127.0.0.1:9997@19997,,tls-port=0,shard-id=[string repeat f 40]"
        set fd [raft_connect 127.0.0.1 $cport]
        raft_send $fd "HI $fake_id $fake_addr 1 1 3"

        # After step-down, leader should be empty (unknown).
        after 100
        assert_equal "" [CI 0 cluster_raft_leader]
        assert_equal "learner" [CI 0 cluster_raft_role]

        close $fd
    }
}


test "Raft proto: REPL_OFFSETS updates node replication offset" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft}} {
        set port [srv 0 port]
        set cport [expr {$port + 10000}]
        set node_id [R 0 CLUSTER MYID]

        # Fake node IDs: one for us (the "leader") and one for a "replica".
        set leader_id [string repeat "a" 40]
        set replica_id [string repeat "c" 40]
        set leader_shard [string repeat "b" 40]
        set leader_addr "127.0.0.1:9999@19999,,tls-port=0,shard-id=$leader_shard"
        set replica_addr "127.0.0.1:9998@19998,,tls-port=0,shard-id=$leader_shard"

        # Connect and do HELLO/HI handshake.
        set fd [raft_connect 127.0.0.1 $cport]
        raft_send $fd "HELLO $leader_id $leader_addr 1 3 2"
        set reply [raft_recv $fd]
        assert_match "HI *" $reply

        # Send AE to establish ourselves as leader and add the replica
        # via NODE_JOIN in the log.
        # AE <leader-id> <term> <prev-log-idx> <prev-log-term> <commit> <count>
        # Entry: <term> <type> <data>
        set ae "AE $leader_id 1 0 0 2 2\n"
        append ae "1 NODE_JOIN $replica_id $replica_addr\n"
        append ae "1 SET_REPLICA_OF $replica_id $leader_id $leader_shard"
        raft_send $fd $ae

        # Read AE_ACK.
        set reply [raft_recv $fd]
        assert_match "AE_ACK *" $reply

        # Verify the replica exists in CLUSTER SHARDS.
        set shards [R 0 CLUSTER SHARDS]
        set found 0
        foreach shard $shards {
            foreach node [dict get $shard nodes] {
                if {[dict get $node id] eq $replica_id} {
                    set found 1
                }
            }
        }
        assert_equal 1 $found "replica should appear in CLUSTER SHARDS"

        # Send REPL_OFFSETS to update the replica's offset.
        raft_send $fd "REPL_OFFSETS $replica_id 42000"

        # Give it a moment to process.
        after 100

        # Verify the offset was updated.
        set shards [R 0 CLUSTER SHARDS]
        foreach shard $shards {
            foreach node [dict get $shard nodes] {
                if {[dict get $node id] eq $replica_id} {
                    assert_equal 42000 [dict get $node replication-offset]
                }
            }
        }

        close $fd
    }
}

test "Raft proto: leader sends REPL_OFFSETS after follower offset changes" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
        set port [srv 0 port]
        set node_id [R 0 CLUSTER MYID]

        set fake_id [string repeat "a" 40]
        set fake_shard [string repeat "b" 40]

        # Start listening before MEET so the leader can connect.
        set cport 0
        set ::_raft_accepted ""
        proc _raft_on_accept {fd addr port} {
            fconfigure $fd -translation binary -buffering full
            set ::_raft_accepted $fd
        }
        set listen_fd [socket -server _raft_on_accept -myaddr 127.0.0.1 0]
        set cport [lindex [fconfigure $listen_fd -sockname] 2]
        set fake_port [expr {$cport - 10000}]
        set fake_addr "127.0.0.1:${fake_port}@${cport},,tls-port=0,shard-id=$fake_shard"

        # Send CLUSTER MEET without waiting for reply (it blocks until
        # NODE_JOIN commits, which needs our AE_ACK).
        set meet_client [valkey_deferring_client]
        $meet_client CLUSTER MEET 127.0.0.1 $fake_port

        # Wait for the leader to connect.
        after 5000 {set ::_raft_accepted timeout}
        vwait ::_raft_accepted
        close $listen_fd
        assert {$::_raft_accepted ne "timeout"}
        set fd $::_raft_accepted

        # Leader sends HELLO.
        set reply [raft_recv $fd 5000]
        assert_match "HELLO *" $reply

        # Reply with HI.
        raft_send $fd "HI $fake_id $fake_addr 1 1 1"

        # Leader commits NODE_JOIN (quorum=1 before we join) and sends WELCOME.
        set reply [raft_recv $fd 10000]
        assert_match "WELCOME *" $reply

        # Leader sends AE with committed entries. Reply with offset=0.
        set reply [raft_recv $fd 5000]
        assert_match "AE *" $reply
        raft_reply_ae_ack $fd $reply 0

        # MEET should now complete. Consume the deferred reply.
        assert_match {OK} [$meet_client read]
        $meet_client close

        # Wait for next AE heartbeat.
        set reply [raft_recv $fd 5000]
        assert_match "AE *" $reply

        # Reply with repl_offset=5000 (0 -> non-zero transition).
        raft_reply_ae_ack $fd $reply 5000

        # Read messages until we find REPL_OFFSETS with non-zero offset.
        set found 0
        for {set i 0} {$i < 30} {incr i} {
            if {[catch {set reply [raft_recv $fd 2000]} err]} break
            if {[string match "REPL_OFFSETS *$fake_id 5000*" $reply]} {
                set found 1
                break
            }
            # Reply to AE heartbeats to keep the connection alive.
            if {[string match "AE *" $reply]} {
                raft_reply_ae_ack $fd $reply 5000
            }
        }
        assert_equal 1 $found "leader should send REPL_OFFSETS with offset 5000"

        close $fd
    }
}

} ;# tags

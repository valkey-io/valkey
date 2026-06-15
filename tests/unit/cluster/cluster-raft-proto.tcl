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

# Listen on a random port, run optional setup code, accept one connection, close
# the listener. Returns the accepted client fd. Sets the listen port in the
# upvar port_var before running the setup code.
proc raft_listen_and_accept {port_var {timeout 5000} {before_accept {}}} {
    upvar $port_var listen_port
    set ::_raft_accepted ""
    proc _raft_on_accept {fd addr port} {
        fconfigure $fd -translation binary -buffering full
        set ::_raft_accepted $fd
    }
    set listen_fd [socket -server _raft_on_accept -myaddr 127.0.0.1 0]
    set listen_port [lindex [fconfigure $listen_fd -sockname] 2]
    if {$before_accept ne {}} {
        uplevel 1 $before_accept
    }
    set accept_after [after $timeout {set ::_raft_accepted timeout}]
    vwait ::_raft_accepted
    after cancel $accept_after
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
        raft_send $fd "HELLO $fake_id $fake_addr singleton"

        # Expect HI back (response to our HELLO).
        set reply [raft_recv $fd]
        assert_match "HI *" $reply

        close $fd
    }
}

test "Raft proto: singleton steps down on MEET from singleton" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft}} {
        set port [srv 0 port]
        set cport [expr {$port + 10000}]

        # Singleton is its own leader.
        assert_equal [R 0 CLUSTER MYID] [CI 0 cluster_raft_leader]

        # Send HELLO + MEET(singleton) from a fake singleton.
        set fake_id [string repeat "c" 40]
        set fake_addr "127.0.0.1:9998@19998,,tls-port=0,shard-id=[string repeat d 40]"
        set fd [raft_connect 127.0.0.1 $cport]
        raft_send $fd "HELLO $fake_id $fake_addr"
        raft_send $fd "MEET singleton"
        set reply [raft_recv $fd]
        assert_match "HI *" $reply
        set reply [raft_recv $fd]
        assert_match "ADD_ME" $reply

        # After step-down, leader should be empty (unknown).
        assert_equal "" [CI 0 cluster_raft_leader]
        assert_equal "joiner" [CI 0 cluster_raft_role]

        close $fd
    }
}

test "Raft proto: singleton steps down on MEET from cluster" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft}} {
        set port [srv 0 port]
        set cport [expr {$port + 10000}]

        # Singleton is its own leader.
        assert_equal [R 0 CLUSTER MYID] [CI 0 cluster_raft_leader]

        # Simulate a cluster member connecting and sending HELLO + MEET(cluster).
        set fake_id [string repeat "e" 40]
        set fake_addr "127.0.0.1:9997@19997,,tls-port=0,shard-id=[string repeat f 40]"
        set fd [raft_connect 127.0.0.1 $cport]
        raft_send $fd "HELLO $fake_id $fake_addr"
        raft_send $fd "MEET cluster"
        set reply [raft_recv $fd]
        assert_match "HI *" $reply
        set reply [raft_recv $fd]
        assert_match "ADD_ME" $reply

        # After step-down, leader should be empty (unknown).
        assert_equal "" [CI 0 cluster_raft_leader]
        assert_equal "joiner" [CI 0 cluster_raft_role]

        close $fd
    }
}

test "Raft proto: joiner reverts to leader after timeout" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 500}} {
        set port [srv 0 port]
        set cport [expr {$port + 10000}]

        set fake_id [string repeat "f" 40]
        set fake_addr "127.0.0.1:9996@19996,,tls-port=0,shard-id=[string repeat a 40]"
        set fd [raft_connect 127.0.0.1 $cport]
        raft_send $fd "HELLO $fake_id $fake_addr"
        raft_send $fd "MEET singleton"
        raft_recv $fd; raft_recv $fd
        assert_equal "joiner" [CI 0 cluster_raft_role]
        close $fd

        # Timeout is 3x500ms = 1.5s. Wait up to 5s for slow CI.
        wait_for_condition 50 100 {
            [CI 0 cluster_raft_role] eq "leader"
        } else {
            fail "Joiner did not revert to leader"
        }
    }
}


test "Raft proto: PRE_VOTE denied while leader lease is active" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
        set cport [expr {[srv 0 port] + 10000}]
        set fake_id [string repeat "d" 40]
        set fake_addr "127.0.0.1:9995@19995,,tls-port=0,shard-id=[string repeat e 40]"

        set fd [raft_connect 127.0.0.1 $cport]
        raft_send $fd "HELLO $fake_id $fake_addr"
        set reply [raft_recv $fd]
        assert_match "HI *" $reply

        set term [CI 0 cluster_raft_current_term]
        set next_term [expr {$term + 1}]
        raft_send $fd "PRE_VOTE_REQ $fake_id $next_term 0 0"
        set reply [raft_recv $fd]
        assert_match "PRE_VOTE $term 0" $reply
        assert_equal $term [CI 0 cluster_raft_current_term]

        close $fd
    }
}

test "Raft proto: PRE_VOTE granted when no leader lease is active" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
        set cport [expr {[srv 0 port] + 10000}]
        set candidate1 [string repeat "g" 40]
        set addr1 "127.0.0.1:9994@19994,,tls-port=0,shard-id=[string repeat h 40]"
        set candidate2 [string repeat "i" 40]
        set addr2 "127.0.0.1:9993@19993,,tls-port=0,shard-id=[string repeat j 40]"

        # Link 1: move node from singleton leader -> follower by higher-term VOTE_REQ.
        set fd1 [raft_connect 127.0.0.1 $cport]
        raft_send $fd1 "HELLO $candidate1 $addr1"
        set reply [raft_recv $fd1]
        assert_match "HI *" $reply

        set term [CI 0 cluster_raft_current_term]
        set vote_term [expr {$term + 1}]
        raft_send $fd1 "VOTE_REQ $candidate1 $vote_term 0 0"
        set reply [raft_recv $fd1]
        assert_match "VOTE $vote_term 1" $reply
        assert_equal "follower" [CI 0 cluster_raft_role]
        assert_equal "" [CI 0 cluster_raft_leader]

        # Link 2: with leader unknown, PRE_VOTE should be grantable.
        set fd2 [raft_connect 127.0.0.1 $cport]
        raft_send $fd2 "HELLO $candidate2 $addr2"
        set reply [raft_recv $fd2]
        assert_match "HI *" $reply

        set prevote_term [expr {$vote_term + 1}]
        raft_send $fd2 "PRE_VOTE_REQ $candidate2 $prevote_term 0 0"
        set reply [raft_recv $fd2]
        assert_match "PRE_VOTE $prevote_term 1" $reply
        assert_equal $vote_term [CI 0 cluster_raft_current_term]

        close $fd1
        close $fd2
    }
}

test "Raft proto: PRE_VOTE denied when candidate log is stale" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
        set cport [expr {[srv 0 port] + 10000}]

        # 1) Seed one committed log entry so receiver's last_log is not empty.
        set seed_id [string repeat "k" 40]
        set seed_addr "127.0.0.1:9992@19992,,tls-port=0,shard-id=[string repeat l 40]"
        set joined_id [string repeat "m" 40]
        set joined_addr "127.0.0.1:9991@19991,,tls-port=0,shard-id=[string repeat l 40]"
        set fd_seed [raft_connect 127.0.0.1 $cport]
        raft_send $fd_seed "HELLO $seed_id $seed_addr"
        set reply [raft_recv $fd_seed]
        assert_match "HI *" $reply

        set ae "AE $seed_id 1 0 0 1 1\n"
        append ae "1 NODE_JOIN $joined_id $joined_addr"
        raft_send $fd_seed $ae
        set reply [raft_recv $fd_seed]
        assert_match "AE_ACK 1 1 1 *" $reply
        assert {[CI 0 cluster_raft_log_entries] >= 1}

        # 2) Step down with higher-term VOTE_REQ to clear known leader lease gate.
        set voter_id [string repeat "n" 40]
        set voter_addr "127.0.0.1:9990@19990,,tls-port=0,shard-id=[string repeat o 40]"
        set fd_vote [raft_connect 127.0.0.1 $cport]
        raft_send $fd_vote "HELLO $voter_id $voter_addr"
        set reply [raft_recv $fd_vote]
        assert_match "HI *" $reply

        set current_term [CI 0 cluster_raft_current_term]
        set vote_term [expr {$current_term + 1}]
        raft_send $fd_vote "VOTE_REQ $voter_id $vote_term 1 1"
        set reply [raft_recv $fd_vote]
        assert_match "VOTE $vote_term 1" $reply
        assert_equal "follower" [CI 0 cluster_raft_role]
        assert_equal "" [CI 0 cluster_raft_leader]

        # 3) Candidate with stale log (0/0) must be denied.
        set stale_id [string repeat "p" 40]
        set stale_addr "127.0.0.1:9989@19989,,tls-port=0,shard-id=[string repeat q 40]"
        set fd_prevote [raft_connect 127.0.0.1 $cport]
        raft_send $fd_prevote "HELLO $stale_id $stale_addr"
        set reply [raft_recv $fd_prevote]
        assert_match "HI *" $reply

        set prevote_term [expr {$vote_term + 1}]
        raft_send $fd_prevote "PRE_VOTE_REQ $stale_id $prevote_term 0 0"
        set reply [raft_recv $fd_prevote]
        assert_match "PRE_VOTE $vote_term 0" $reply
        assert_equal $vote_term [CI 0 cluster_raft_current_term]

        close $fd_seed
        close $fd_vote
        close $fd_prevote
    }
}

test "Raft proto: pre-vote timeout does not inflate term without quorum" {
    start_server {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 500}} {
        set cport [expr {[srv 0 port] + 10000}]
        set node_id [R 0 CLUSTER MYID]
        set fake_id [string repeat "r" 40]

        # Pre-vote and RequestVote broadcasts use node->link, not the inbound
        # leader link, so listen for the server's outbound link to the fake peer.
        set fake_cport 0
        set fd [raft_listen_and_accept fake_cport 5000 {
            set fake_port [expr {$fake_cport - 10000}]
            set fake_addr "127.0.0.1:${fake_port}@${fake_cport},,tls-port=0,shard-id=[string repeat s 40]"

            set leader_fd [raft_connect 127.0.0.1 $cport]
            raft_send $leader_fd "HELLO $fake_id $fake_addr"
            set reply [raft_recv $leader_fd]
            assert_match "HI *" $reply

            # Commit both nodes into the fake leader's log. The real node must
            # see the fake peer as joined, otherwise pre-vote requests are skipped.
            set ae "AE $fake_id 2 0 0 2 2\n"
            append ae "2 NODE_JOIN $node_id 127.0.0.1:[srv 0 port]@[expr {[srv 0 port] + 10000}],,tls-port=0,shard-id=[string repeat t 40]\n"
            append ae "2 NODE_JOIN $fake_id $fake_addr"
            raft_send $leader_fd $ae
            set reply [raft_recv $leader_fd]
            assert_match "AE_ACK 2 1 2 *" $reply
            assert_equal "follower" [CI 0 cluster_raft_role]
            assert_equal 2 [CI 0 cluster_raft_current_term]
            assert_equal 2 [CI 0 cluster_size]
        }]

        set reply [raft_recv $fd]
        assert_match "HELLO *" $reply
        raft_send $fd "HI $fake_id $fake_addr"

        # Stop heartbeats. The follower should pre-vote in term+1, but without
        # a grant it must not increment its persisted current term.
        set reply [raft_recv $fd 5000]
        assert_match "PRE_VOTE_REQ $node_id 3 2 2" $reply
        assert_equal 2 [CI 0 cluster_raft_current_term]

        # Do not respond to the first pre-vote. A later timeout starts a new
        # pre-vote without changing the persisted term.
        assert_equal 2 [CI 0 cluster_raft_current_term]

        # Granting the second pre-vote should move the
        # node into a real election and broadcast VOTE_REQ with the bumped term.
        set reply [raft_recv $fd 5000]
        assert_match "PRE_VOTE_REQ $node_id 3 2 2" $reply
        raft_send $fd "PRE_VOTE 3 1"

        set found_vote_req 0
        for {set i 0} {$i < 20} {incr i} {
            set reply [raft_recv $fd 5000]
            if {[string match "VOTE_REQ $node_id 3 2 2" $reply]} {
                set found_vote_req 1
                break
            }
        }
        assert_equal 1 $found_vote_req "pre-vote grant should start a real election"
        assert_equal 3 [CI 0 cluster_raft_current_term]

        close $fd
        close $leader_fd
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
        set fd [raft_listen_and_accept cport 5000 {
            set fake_port [expr {$cport - 10000}]
            set fake_addr "127.0.0.1:${fake_port}@${cport},,tls-port=0,shard-id=$fake_shard"

            # Send CLUSTER MEET without waiting for reply (it blocks until
            # NODE_JOIN commits, which needs our AE_ACK).
            set meet_client [valkey_deferring_client]
            $meet_client CLUSTER MEET 127.0.0.1 $fake_port
        }]

        # Leader sends HELLO + MEET(singleton).
        set reply [raft_recv $fd 5000]
        assert_match "HELLO *" $reply
        set reply [raft_recv $fd 5000]
        assert_match "MEET *" $reply

        # Reply with HI + ADD_ME (fake node steps down, asks to be added).
        raft_send $fd "HI $fake_id $fake_addr"
        raft_send $fd "ADD_ME"

        # Leader receives ADD_ME, invites us, commits NODE_JOIN, sends AE.
        # Reply with offset=0.
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

# Shared helpers for Raft cluster integration tests.

proc raft_add_voter {leader node_id} {
    set leader_client [Rn $leader]
    # Wait until NODE_JOIN finished: learner, or already a voting member.
    wait_for_condition 100 100 {
        [expr {[llength [cluster_get_node_by_id $leader $node_id]] > 0 &&
               ([cluster_has_flag [cluster_get_node_by_id $leader $node_id] learner] ||
                ![cluster_has_flag [cluster_get_node_by_id $leader $node_id] handshake])}]
    } else {
        fail "Node $node_id did not finish joining on leader $leader"
    }
    set err ""
    # The auto-promoter may have already promoted this learner, making a
    # subsequent ADDVOTER fail with "already a voter". Treat either
    # "promoted" or "ADDVOTER succeeded" as success (avoids a TOCTOU race).
    wait_for_condition 200 100 {
        [expr {![cluster_has_flag [cluster_get_node_by_id $leader $node_id] learner] ||
               ![catch {$leader_client CLUSTER ADDVOTER $node_id} err]}]
    } else {
        fail "Could not promote $node_id to voter: $err"
    }
}

# ---------------------------------------------------------------------------
# Raft wire protocol helpers for simulating cluster nodes over the cluster bus.
# ---------------------------------------------------------------------------

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

# Listen on a random port, run optional setup code, accept one connection, then
# close the listener. Returns the accepted client fd and sets the listen port in
# the upvar port_var.
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

# Connect a fake node to a cluster bus port and complete the HELLO/HI handshake.
proc raft_connect_fake_node {host cport fake_id fake_addr} {
    set fd [raft_connect $host $cport]
    raft_send $fd "HELLO $fake_id $fake_addr"
    set reply [raft_recv $fd]
    assert_match "HI *" $reply
    return $fd
}

# Reply AE_ACK to an AE message. last_index defaults to the AE's prev-log-idx
# plus its entry count; pass it explicitly to simulate a lagging follower.
proc raft_reply_ae_ack {fd ae_msg repl_offset {last_index ""}} {
    set lines [split $ae_msg "\n"]
    set fields [split [lindex $lines 0] " "]
    # AE <leader-id> <term> <prev-log-idx> <prev-log-term> <commit> <count>
    set term [lindex $fields 2]
    if {$last_index eq ""} {
        set last_index [expr {[lindex $fields 3] + [lindex $fields 6]}]
    }
    raft_send $fd "AE_ACK $term 1 $last_index $repl_offset"
    return $last_index
}

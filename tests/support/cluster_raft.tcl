# Shared helpers for Raft cluster integration tests.

proc raft_set_voter {node policy} {
    R $node CONFIG SET cluster-voter $policy
}

proc raft_add_voter {leader node} {
    set node_id [R $node CLUSTER MYID]
    # Wait until NODE_JOIN finished: learner, or already a voting member.
    wait_for_condition 100 100 {
        [expr {[llength [cluster_get_node_by_id $leader $node_id]] > 0 &&
               ([cluster_has_flag [cluster_get_node_by_id $leader $node_id] learner] ||
                ![cluster_has_flag [cluster_get_node_by_id $leader $node_id] handshake])}]
    } else {
        fail "Node $node did not finish joining on leader $leader"
    }
    if {[cluster_has_flag [cluster_get_node_by_id $leader $node_id] learner]} {
        set leader_id [R $leader CLUSTER MYID]
        wait_for_condition 100 50 {
            [CI $node cluster_raft_leader] eq $leader_id
        } else {
            fail "Node $node did not follow leader $leader"
        }
        raft_set_voter $node yes
    }
    wait_for_condition 200 100 {
        ![cluster_has_flag [cluster_get_node_by_id $leader $node_id] learner]
    } else {
        fail "Could not promote node $node to voter"
    }
}

# Unapplied log entries of `type` in nodes.conf.
proc raft_inflight_count {client type} {
    $client CLUSTER SAVECONFIG
    set dir [lindex [$client CONFIG GET dir] 1]
    set path "$dir/nodes.conf"
    if {![file exists $path]} {
        return 0
    }
    set fp [open $path r]
    set text [read $fp]
    close $fp
    set n 0
    foreach line [split $text "\n"] {
        set fields [split [string trim $line "\r"] " "]
        # Format: log <crc64hex> <index> <term> <type> <data...>
        if {[lindex $fields 0] eq "log" && [lindex $fields 4] eq $type} {
            incr n
        }
    }
    return $n
}

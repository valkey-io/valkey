# Shared helpers for Raft cluster integration tests.

proc get_cluster_info_field {client field} {
    set info [$client CLUSTER INFO]
    foreach line [split $info "\n"] {
        set line [string trim $line "\r"]
        if {[string match "${field}:*" $line]} {
            return [lindex [split $line ":"] 1]
        }
    }
    return ""
}

proc raft_cluster_nodes_line {client node_id} {
    foreach line [split [$client CLUSTER NODES] "\n"] {
        set line [string trim $line "\r"]
        if {[string match "$node_id *" $line]} {
            return $line
        }
    }
    return ""
}

proc raft_node_visible {client node_id} {
    return [expr {[raft_cluster_nodes_line $client $node_id] ne ""}]
}

proc raft_node_has_flag {client node_id flag} {
    set line [raft_cluster_nodes_line $client $node_id]
    if {$line eq ""} {
        return 0
    }
    return [string match "*${flag}*" [lindex [split $line] 2]]
}

proc raft_add_voter {leader node_id} {
    # Wait until NODE_JOIN finished: learner, or already a voting member.
    wait_for_condition 100 100 {
        [expr {[raft_node_visible $leader $node_id] && (
            [raft_node_has_flag $leader $node_id learner] ||
            ![raft_node_has_flag $leader $node_id handshake]
        )}]
    } else {
        fail "Node $node_id did not finish joining on $leader"
    }
    if {![raft_node_has_flag $leader $node_id learner]} {
        return
    }
    set err ""
    wait_for_condition 200 100 {
        [expr {![catch {$leader CLUSTER ADDVOTER $node_id} err]}]
    } else {
        fail "Could not promote $node_id to voter: $err"
    }
}

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

# TEST HARNESS ONLY — not production behavior.
#
# On the wire, CLUSTER MEET still proposes NODE_JOIN ... learner.
# start_cluster historically assumed every joined node could vote
# (pre-learner). Generic cluster tests still rely on that topology,
# so after MEET we explicitly ADDVOTER each peer.
#
# Do NOT copy this into product code. Tests that need learners must
# use start_multiple_servers (or equivalent) and promote themselves.
proc raft_promote_start_cluster_voters {node_count} {
    for {set i 1} {$i < $node_count} {incr i} {
        raft_add_voter [srv 0 client] [R $i CLUSTER MYID]
    }
}

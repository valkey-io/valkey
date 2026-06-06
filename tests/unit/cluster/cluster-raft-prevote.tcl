# Test pre-vote behavior in raft cluster bus.

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

proc raft_find_role_idx {role} {
    foreach idx {0 1 2} {
        if {[CI $idx cluster_raft_role] eq $role} {
            return $idx
        }
    }
    return -1
}

proc raft_form_3node_cluster {} {
    R 0 CLUSTER MEET [srv -2 host] [srv -2 port]
    R 0 CLUSTER MEET [srv -1 host] [srv -1 port]

    wait_for_condition 100 100 {
        [CI 0 cluster_size] == 3 &&
        [CI 1 cluster_size] == 3 &&
        [CI 2 cluster_size] == 3
    } else {
        fail "cluster did not form"
    }

    wait_for_condition 100 100 {
        [raft_find_role_idx leader] >= 0
    } else {
        fail "no raft leader after cluster formed"
    }
}

proc raft_isolate_node {idx} {
    set isolated [Rn $idx]
    $isolated DEBUG DISABLE-CLUSTER-RECONNECTION 1
    foreach other {0 1 2} {
        if {$other == $idx} continue
        set other_c [Rn $other]
        $isolated DEBUG CLUSTERLINK KILL all [$other_c CLUSTER MYID]
        $other_c DEBUG CLUSTERLINK KILL all [$isolated CLUSTER MYID]
    }
}

proc raft_heal_isolation {idx} {
    [Rn $idx] DEBUG DISABLE-CLUSTER-RECONNECTION 0
}

tags {external:skip cluster singledb} {

start_multiple_servers 3 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 500}} {
    test "Raft pre-vote: isolated follower does not inflate term" {
        raft_form_3node_cluster

        set leader_idx [raft_find_role_idx leader]
        set leader_term [CI $leader_idx cluster_raft_current_term]
        set leader_id [CI $leader_idx cluster_raft_leader]

        set follower_idx [raft_find_role_idx follower]
        assert {$follower_idx >= 0}

        raft_isolate_node $follower_idx

        # Wait long enough to trigger several election timeouts on the isolated node.
        after 3500

        # Isolated follower should not have inflated term while partitioned.
        assert_equal $leader_term [CI $follower_idx cluster_raft_current_term]

        # Heal partition and ensure leader remains stable on the majority side.
        raft_heal_isolation $follower_idx
        wait_for_condition 100 100 {
            [CI 0 cluster_raft_leader] eq $leader_id &&
            [CI 1 cluster_raft_leader] eq $leader_id &&
            [CI 2 cluster_raft_leader] eq $leader_id
        } else {
            fail "leader changed unexpectedly after isolated follower rejoined"
        }
    }
}

start_multiple_servers 3 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 500}} {
    test "Raft pre-vote: leader failure still allows election" {
        raft_form_3node_cluster

        set leader_idx [raft_find_role_idx leader]
        set old_term [CI $leader_idx cluster_raft_current_term]
        set old_leader_id [CI $leader_idx cluster_raft_leader]

        set survivors [list]
        foreach idx {0 1 2} {
            if {$idx != $leader_idx} {
                lappend survivors $idx
            }
        }

        catch {R $leader_idx shutdown nosave}

        set s0 [lindex $survivors 0]
        set s1 [lindex $survivors 1]
        wait_for_condition 200 100 {
            [CI $s0 cluster_raft_current_term] > $old_term &&
            [CI $s1 cluster_raft_current_term] > $old_term &&
            ([CI $s0 cluster_raft_role] eq "leader" ||
             [CI $s1 cluster_raft_role] eq "leader")
        } else {
            fail "no new leader elected after leader shutdown (term0=[CI $s0 cluster_raft_current_term] term1=[CI $s1 cluster_raft_current_term] role0=[CI $s0 cluster_raft_role] role1=[CI $s1 cluster_raft_role])"
        }

        if {[CI $s0 cluster_raft_role] eq "leader"} {
            set new_leader_idx $s0
        } else {
            set new_leader_idx $s1
        }
        set new_leader_id [CI $new_leader_idx cluster_raft_leader]
        assert {$new_leader_id ne $old_leader_id}
        assert {[CI $new_leader_idx cluster_raft_current_term] > $old_term}
    }
}

} ;# tags

# Test automatic learner promotion for the Raft cluster protocol: the leader
# promotes caught-up learners to voters up to RAFT_TARGET_VOTERS (5), reusing
# the ADD_VOTER path, with replacement-first DELVOTER pre-validation and an
# "at most one membership transition in flight" guard.

source tests/support/cluster_raft.tcl

# Have node 0 meet peers 1..n-1 (star formation) and return their node ids.
proc raft_meet_peers {n} {
    set r0 [srv 0 client]
    for {set i 1} {$i < $n} {incr i} {
        $r0 CLUSTER MEET [srv -$i host] [srv -$i port]
    }
    set peer_ids [list]
    for {set i 1} {$i < $n} {incr i} {
        lappend peer_ids [R $i CLUSTER MYID]
    }
    return $peer_ids
}

# Return the first learner id among the given peer ids, or "" if none.
proc raft_find_learner {leader ids} {
    foreach id $ids {
        if {[cluster_has_flag [cluster_get_node_by_id $leader $id] learner]} {
            return $id
        }
    }
    return ""
}

# Return the first voting (non-learner) id among the given peer ids, skipping
# an optional excluded id, or "" if none.
proc raft_find_voter {leader ids {exclude ""}} {
    foreach id $ids {
        if {$id eq $exclude} continue
        if {![cluster_has_flag [cluster_get_node_by_id $leader $id] learner]} {
            return $id
        }
    }
    return ""
}

# Return the unique ADD_VOTER targets among the unapplied (in-flight) log
# entries persisted in nodes.conf. A full rewrite (SAVECONFIG) writes only
# entries with index > last_applied as "log" lines.
proc raft_inflight_add_voter_targets {client} {
    $client CLUSTER SAVECONFIG
    set dir [lindex [$client CONFIG GET dir] 1]
    set path "$dir/nodes.conf"
    if {![file exists $path]} {
        return [list]
    }
    set fp [open $path r]
    set text [read $fp]
    close $fp
    set targets [list]
    foreach line [split $text "\n"] {
        set fields [split [string trim $line "\r"] " "]
        # Format: log <crc64hex> <index> <term> <type> <data...>
        if {[lindex $fields 0] eq "log" && [lindex $fields 4] eq "ADD_VOTER"} {
            lappend targets [lindex $fields 5]
        }
    }
    return [lsort -unique $targets]
}

tags {external:skip cluster singledb} {

start_multiple_servers 5 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: 1 voter + 4 learners converge to 5 voters" {
        raft_meet_peers 5
        # No manual ADDVOTER — the controller must promote all four learners.
        wait_for_condition 200 100 {
            [CI 0 cluster_size] == 5 &&
            [CI 1 cluster_size] == 5 &&
            [CI 4 cluster_size] == 5
        } else {
            fail "Sizes: [CI 0 cluster_size] [CI 1 cluster_size] [CI 4 cluster_size]"
        }
    }
}

start_multiple_servers 3 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: fewer than target nodes converge without hanging" {
        raft_meet_peers 3
        wait_for_condition 100 100 {
            [CI 0 cluster_size] == 3 &&
            [CI 1 cluster_size] == 3 &&
            [CI 2 cluster_size] == 3
        } else {
            fail "Sizes: [CI 0 cluster_size] [CI 1 cluster_size] [CI 2 cluster_size]"
        }
    }
}

start_multiple_servers 5 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: stops at exactly the target number of voters" {
        set r0 [srv 0 client]
        raft_meet_peers 5
        wait_for_condition 200 100 {
            [CI 0 cluster_size] == 5
        } else {
            fail "size=[CI 0 cluster_size]"
        }
        # Give the controller a chance to overshoot; it must not.
        after 2000
        assert_equal 5 [CI 0 cluster_size]
        assert_equal 0 [llength [raft_inflight_add_voter_targets $r0]]
    }
}

start_multiple_servers 6 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: manual ADDVOTER above target is not demoted" {
        set peer_ids [raft_meet_peers 6]
        wait_for_condition 200 100 {
            [CI 0 cluster_size] == 5
        } else {
            fail "size=[CI 0 cluster_size]"
        }
        # Promote the remaining learner by hand, pushing size to 6.
        set remaining [raft_find_learner 0 $peer_ids]
        assert {$remaining ne ""}
        raft_add_voter 0 $remaining
        wait_for_condition 100 100 {
            [CI 0 cluster_size] == 6
        } else {
            fail "size=[CI 0 cluster_size]"
        }
        # The one-way controller must not demote it back to 5.
        after 2000
        assert_equal 6 [CI 0 cluster_size]
    }
}

start_multiple_servers 5 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: DELVOTER below target is rejected (replacement-first)" {
        set r0 [srv 0 client]
        set peer_ids [raft_meet_peers 5]
        wait_for_condition 200 100 {
            [CI 0 cluster_size] == 5
        } else {
            fail "size=[CI 0 cluster_size]"
        }
        # Demoting a voter would leave 4 < target.
        set target [raft_find_voter 0 $peer_ids]
        assert {$target ne ""}
        catch {$r0 CLUSTER DELVOTER $target} err
        assert_match "*rejected*" $err
        assert_equal 5 [CI 0 cluster_size]
    }
}

start_multiple_servers 6 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: DELVOTER at/above target is allowed" {
        set r0 [srv 0 client]
        set peer_ids [raft_meet_peers 6]
        wait_for_condition 200 100 {
            [CI 0 cluster_size] == 5
        } else {
            fail "size=[CI 0 cluster_size]"
        }
        set remaining [raft_find_learner 0 $peer_ids]
        assert {$remaining ne ""}
        raft_add_voter 0 $remaining
        wait_for_condition 100 100 {
            [CI 0 cluster_size] == 6
        } else {
            fail "size=[CI 0 cluster_size]"
        }
        # Now demoting a non-leader voter (6 -> 5) is allowed.
        set target [raft_find_voter 0 $peer_ids $remaining]
        assert {$target ne ""}
        $r0 CLUSTER DELVOTER $target
        wait_for_condition 100 100 {
            [CI 0 cluster_size] == 5
        } else {
            fail "size=[CI 0 cluster_size]"
        }
    }
}

start_multiple_servers 6 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: paused voter does not cause promotion beyond target" {
        set peer_ids [raft_meet_peers 6]
        wait_for_condition 200 100 {
            [CI 0 cluster_size] == 5
        } else {
            fail "size=[CI 0 cluster_size]"
        }
        set remaining [raft_find_learner 0 $peer_ids]
        assert {$remaining ne ""}
        # Pause a non-leader voter. The controller must not promote the 6th
        # learner to replace it (target is 5, already reached).
        set pause_i 0
        for {set i 0} {$i < [llength $peer_ids]} {incr i} {
            if {![cluster_has_flag [cluster_get_node_by_id 0 [lindex $peer_ids $i]] learner]} {
                set pause_i [expr {$i + 1}]
                break
            }
        }
        assert {$pause_i > 0}
        pause_process [srv -$pause_i pid]
        after 2000
        assert_equal 5 [CI 0 cluster_size]
        assert {[cluster_has_flag [cluster_get_node_by_id 0 $remaining] learner]}
        resume_process [srv -$pause_i pid]
    }
}

start_server {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 5000}} {
    test "Raft autopromote: learner that has not caught up is not promoted" {
        set fake_id [string repeat "9" 40]
        set fake_shard [string repeat "8" 40]

        # Listen for the leader's outbound connection so AE flows to us. MEET
        # triggers NODE_JOIN(learner) which commits under quorum=1 (we don't
        # count as a voter).
        set fake_cport 0
        set fd [raft_listen_and_accept fake_cport 5000 {
            set fake_port [expr {$fake_cport - 10000}]
            set fake_addr "127.0.0.1:${fake_port}@${fake_cport},,tls-port=0,shard-id=$fake_shard"
            set meet_client [valkey_deferring_client]
            $meet_client CLUSTER MEET 127.0.0.1 $fake_port
        }]

        # Leader connects outbound: HELLO + MEET(singleton).
        set reply [raft_recv $fd 5000]
        assert_match "HELLO *" $reply
        set reply [raft_recv $fd 5000]
        assert_match "MEET *" $reply
        raft_send $fd "HI $fake_id $fake_addr"
        raft_send $fd "ADD_ME"

        # Leader commits NODE_JOIN(learner) and sends AE carrying it.
        set reply [raft_recv $fd 5000]
        assert_match "AE *" $reply

        # Reply with a lagging last-log-index so the leader never sees us as
        # caught up (match_index stays 0).
        raft_reply_ae_ack $fd $reply 0 0

        wait_for_condition 50 100 {
            [cluster_has_flag [cluster_get_node_by_id 0 $fake_id] learner]
        } else {
            fail "learner did not join"
        }

        # The catch-up gate must keep us a learner.
        after 2000
        assert {[cluster_has_flag [cluster_get_node_by_id 0 $fake_id] learner]}

        close $fd
        $meet_client close
    }
}

start_multiple_servers 3 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 15000}} {
    test "Raft autopromote: at most one distinct membership transition in flight" {
        raft_meet_peers 3
        wait_for_condition 100 100 {
            [CI 0 cluster_size] == 3
        } else {
            fail "size=[CI 0 cluster_size]"
        }

        # Pause both follower voters so the leader can append but not commit a
        # membership transition (quorum = 2 of 3). node-timeout is large enough
        # that the leader won't step down within the observation window.
        pause_process [srv -1 pid]
        pause_process [srv -2 pid]

        set cport [expr {[srv 0 port] + 10000}]
        set f1 [string repeat "a" 40]
        set f2 [string repeat "b" 40]
        set a1 "127.0.0.1:9001@19001,,tls-port=0,shard-id=[string repeat "c" 40]"
        set a2 "127.0.0.1:9002@19002,,tls-port=0,shard-id=[string repeat "d" 40]"

        # First membership transition (NODE_JOIN voter) is accepted and stays
        # in flight because quorum is unavailable.
        set fd1 [raft_connect_fake_node 127.0.0.1 $cport $f1 $a1]
        raft_send $fd1 "PROPOSE NODE_JOIN $f1 $a1 voter"
        after 500

        # A second, different membership transition must be rejected by the
        # "at most one in flight" gate.
        set fd2 [raft_connect_fake_node 127.0.0.1 $cport $f2 $a2]
        raft_send $fd2 "PROPOSE NODE_JOIN $f2 $a2 voter"
        set reply [raft_recv $fd2 2000]
        assert_match "REJECT *" $reply

        close $fd1
        close $fd2
        resume_process [srv -1 pid]
        resume_process [srv -2 pid]
    }
}

start_multiple_servers 6 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 500}} {
    test "Raft autopromote: leader change preserves the voter set and ADDVOTER works" {
        set peer_ids [raft_meet_peers 6]
        wait_for_condition 200 100 {
            [CI 0 cluster_size] == 5
        } else {
            fail "size=[CI 0 cluster_size]"
        }
        set remaining [raft_find_learner 0 $peer_ids]
        assert {$remaining ne ""}

        set old_term [CI 0 cluster_raft_current_term]
        pause_process [srv 0 pid]
        wait_for_condition 200 100 {
            [CI 1 cluster_raft_current_term] > $old_term &&
            ([CI 1 cluster_raft_role] eq "leader" ||
             [CI 2 cluster_raft_role] eq "leader" ||
             [CI 3 cluster_raft_role] eq "leader" ||
             [CI 4 cluster_raft_role] eq "leader" ||
             [CI 5 cluster_raft_role] eq "leader")
        } else {
            resume_process [srv 0 pid]
            fail "no new leader elected after pause"
        }

        set new_leader_idx 0
        for {set i 1} {$i < 6} {incr i} {
            if {[CI $i cluster_raft_role] eq "leader"} {
                set new_leader_idx $i
                break
            }
        }
        assert {$new_leader_idx > 0}

        # The new leader must still promote the remaining learner.
        raft_add_voter $new_leader_idx $remaining
        resume_process [srv 0 pid]
        wait_for_condition 200 100 {
            [CI 0 cluster_size] == 6 && [CI 1 cluster_size] == 6
        } else {
            fail "Sizes after leader change: [CI 0 cluster_size] [CI 1 cluster_size]"
        }
    }
}

start_multiple_servers 6 {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: concurrent DELVOTER does not drop below target" {
        set peer_ids [raft_meet_peers 6]
        wait_for_condition 200 100 {
            [CI 0 cluster_size] == 5
        } else {
            fail "size=[CI 0 cluster_size]"
        }
        set remaining [raft_find_learner 0 $peer_ids]
        raft_add_voter 0 $remaining
        wait_for_condition 100 100 {
            [CI 0 cluster_size] == 6
        } else {
            fail "size=[CI 0 cluster_size]"
        }

        set a [raft_find_voter 0 $peer_ids]
        set b [raft_find_voter 0 $peer_ids $a]
        assert {$a ne "" && $b ne ""}

        # Fire two DELVOTERs concurrently. Exactly one may win (6 -> 5); the
        # other is rejected (either by the in-flight gate or because it would
        # go 5 -> 4, below target).
        set c1 [valkey_deferring_client]
        set c2 [valkey_deferring_client]
        $c1 CLUSTER DELVOTER $a
        $c2 CLUSTER DELVOTER $b
        set s1 [catch {$c1 read} r1]
        set s2 [catch {$c2 read} r2]
        $c1 close
        $c2 close

        assert {($s1 == 0) != ($s2 == 0)}
        wait_for_condition 100 100 {
            [CI 0 cluster_size] == 5
        } else {
            fail "size=[CI 0 cluster_size]"
        }
        after 1000
        assert_equal 5 [CI 0 cluster_size]
    }
}

} ;# tags

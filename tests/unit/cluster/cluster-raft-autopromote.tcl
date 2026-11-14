# Automatic learner promotion up to RAFT_TARGET_VOTERS (5).

source tests/support/cluster_raft.tcl

# Servers 1..n-1. Skip the bootstrap leader so DELVOTER/FORGET do not pick it.
proc raft_peer_ids {n} {
    set ids [list]
    for {set i 1} {$i < $n} {incr i} {
        lappend ids [R $i CLUSTER MYID]
    }
    return $ids
}

proc raft_find_learner {leader ids} {
    foreach id $ids {
        if {[cluster_has_flag [cluster_get_node_by_id $leader $id] learner]} {
            return $id
        }
    }
    return ""
}

proc raft_find_voter {leader ids {exclude ""}} {
    foreach id $ids {
        if {$id eq $exclude} continue
        if {![cluster_has_flag [cluster_get_node_by_id $leader $id] learner]} {
            return $id
        }
    }
    return ""
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

tags {external:skip cluster singledb} {

start_cluster 5 0 {overrides {cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: 1 voter + 4 learners converge to 5 and do not overshoot" {
        after 2000
        assert_equal 5 [CI 0 cluster_size]
        assert_equal 0 [raft_inflight_count [srv 0 client] ADD_VOTER]
    }
}

start_cluster 6 0 {overrides {cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: manual ADDVOTER above target is not demoted" {
        set remaining [raft_find_learner 0 [raft_peer_ids 6]]
        assert {$remaining ne ""}
        raft_add_voter 0 $remaining
        wait_for_raft_cluster_size 6
        after 2000
        assert_equal 6 [CI 0 cluster_size]
    }
}

start_cluster 5 0 {overrides {cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: DELVOTER below target is applied, then the learner is promoted" {
        set target [raft_find_voter 0 [raft_peer_ids 5]]
        assert {$target ne ""}
        assert_equal OK [R 0 CLUSTER DELVOTER $target]
        wait_for_condition 200 100 {
            [CI 0 cluster_size] == 5 &&
            ![cluster_has_flag [cluster_get_node_by_id 0 $target] learner]
        } else {
            fail "size=[CI 0 cluster_size]"
        }
    }
}

start_cluster 6 0 {overrides {cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: DELVOTER at/above target is allowed" {
        set peer_ids [raft_peer_ids 6]
        set remaining [raft_find_learner 0 $peer_ids]
        assert {$remaining ne ""}
        raft_add_voter 0 $remaining
        wait_for_raft_cluster_size 6
        set target [raft_find_voter 0 $peer_ids $remaining]
        assert {$target ne ""}
        R 0 CLUSTER DELVOTER $target
        wait_for_raft_cluster_size 5
    }
}

start_cluster 6 0 {overrides {cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: paused voter does not cause promotion beyond target" {
        set peer_ids [raft_peer_ids 6]
        set remaining [raft_find_learner 0 $peer_ids]
        assert {$remaining ne ""}
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

start_server {overrides {cluster-enabled yes cluster-protocol raft cluster-node-timeout 5000} tags {tls:skip}} {
    test "Raft autopromote: learner that has not caught up is not promoted" {
        set fake_id [string repeat "9" 40]
        set fake_shard [string repeat "8" 40]

        # MEET commits NODE_JOIN(learner) under quorum=1. Lag the AE ACK so
        # match_index stays 0 and auto-promote must not pick us.
        set fake_cport 0
        set fd [raft_listen_and_accept fake_cport 5000 {
            set fake_port [expr {$fake_cport - 10000}]
            set fake_addr "127.0.0.1:${fake_port}@${fake_cport},,tls-port=0,shard-id=$fake_shard"
            set meet_client [valkey_deferring_client]
            $meet_client CLUSTER MEET 127.0.0.1 $fake_port
        }]

        set reply [raft_recv $fd 5000]
        assert_match "HELLO *" $reply
        set reply [raft_recv $fd 5000]
        assert_match "MEET *" $reply
        raft_send $fd "HI $fake_id $fake_addr"
        raft_send $fd "ADD_ME"

        set reply [raft_recv $fd 5000]
        assert_match "AE *" $reply
        raft_reply_ae_ack $fd $reply 0 0

        wait_for_condition 50 100 {
            [cluster_has_flag [cluster_get_node_by_id 0 $fake_id] learner]
        } else {
            fail "learner did not join"
        }

        after 2000
        assert {[cluster_has_flag [cluster_get_node_by_id 0 $fake_id] learner]}

        close $fd
        $meet_client close
    }
}

start_cluster 3 0 {overrides {cluster-protocol raft cluster-node-timeout 15000} tags {tls:skip}} {
    test "Raft autopromote: at most one distinct membership transition in flight" {
        # Pause followers so the leader can append but not commit (quorum 2/3).
        pause_process [srv -1 pid]
        pause_process [srv -2 pid]

        set cport [expr {[srv 0 port] + 10000}]
        set f1 [string repeat "a" 40]
        set f2 [string repeat "b" 40]
        set a1 "127.0.0.1:9001@19001,,tls-port=0,shard-id=[string repeat "c" 40]"
        set a2 "127.0.0.1:9002@19002,,tls-port=0,shard-id=[string repeat "d" 40]"

        set fd1 [raft_connect_fake_node 127.0.0.1 $cport $f1 $a1]
        raft_send $fd1 "PROPOSE NODE_JOIN $f1 $a1 voter"
        after 500

        set fd2 [raft_connect_fake_node 127.0.0.1 $cport $f2 $a2]
        raft_send $fd2 "PROPOSE NODE_JOIN $f2 $a2 voter"
        set reply [raft_recv $fd2 2000]
        assert_match "REJECT *" $reply

        # Re-proposing the in-flight NODE_JOIN is SAME, not a second entry.
        raft_send $fd1 "PROPOSE NODE_JOIN $f1 $a1 voter"
        set got [catch {raft_recv $fd1 400} reply]
        if {!$got} {
            assert_no_match "REJECT *" $reply
        }
        assert_equal 1 [raft_inflight_count [srv 0 client] NODE_JOIN]

        close $fd1
        close $fd2
        resume_process [srv -1 pid]
        resume_process [srv -2 pid]
    }
}

start_cluster 3 0 {overrides {cluster-protocol raft cluster-node-timeout 15000}} {
    test "Raft autopromote: second DELVOTER waits on the in-flight entry" {
        set id [R 1 CLUSTER MYID]
        pause_process [srv -1 pid]
        pause_process [srv -2 pid]
        set c1 [valkey_deferring_client]
        set c2 [valkey_deferring_client]
        $c1 CLUSTER DELVOTER $id
        after 300
        $c2 CLUSTER DELVOTER $id
        after 300
        assert_equal 1 [raft_inflight_count [srv 0 client] DEL_VOTER]

        resume_process [srv -1 pid]
        resume_process [srv -2 pid]
        assert_equal OK [$c1 read]
        assert_equal OK [$c2 read]
        $c1 close
        $c2 close
    }
}

start_cluster 6 0 {overrides {cluster-protocol raft cluster-node-timeout 500}} {
    test "Raft autopromote: leader change preserves the voter set and ADDVOTER works" {
        set remaining [raft_find_learner 0 [raft_peer_ids 6]]
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

        raft_add_voter $new_leader_idx $remaining
        resume_process [srv 0 pid]
        wait_for_raft_cluster_size 6
    }
}

start_cluster 6 0 {overrides {cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: forgetting a voter promotes another learner" {
        set peer_ids [raft_peer_ids 6]
        set learner [raft_find_learner 0 $peer_ids]
        set target [raft_find_voter 0 $peer_ids]
        assert {$learner ne "" && $target ne ""}
        set target_idx [expr {[lsearch -exact $peer_ids $target] + 1}]
        assert_equal OK [R 0 CLUSTER FORGET $target]
        pause_process [srv -$target_idx pid]
        wait_for_condition 200 100 {
            [CI 0 cluster_size] == 5 &&
            [cluster_get_node_by_id 0 $target] eq {} &&
            ![cluster_has_flag [cluster_get_node_by_id 0 $learner] learner]
        } else {
            resume_process [srv -$target_idx pid]
            fail "size=[CI 0 cluster_size]"
        }
        resume_process [srv -$target_idx pid]
    }
}

start_cluster 3 0 {overrides {cluster-protocol raft cluster-node-timeout 1000}} {
    test "Raft autopromote: FORGET can shrink the voter set to one node" {
        assert_equal OK [R 0 CLUSTER FORGET [R 2 CLUSTER MYID]]
        pause_process [srv -2 pid]
        wait_for_condition 100 100 {
            [CI 0 cluster_size] == 2
        } else {
            resume_process [srv -2 pid]
            fail "size=[CI 0 cluster_size]"
        }
        assert_equal OK [R 0 CLUSTER FORGET [R 1 CLUSTER MYID]]
        pause_process [srv -1 pid]
        wait_for_condition 100 100 {
            [CI 0 cluster_size] == 1
        } else {
            resume_process [srv -1 pid]
            resume_process [srv -2 pid]
            fail "size=[CI 0 cluster_size]"
        }
        after 1000
        assert_equal 1 [CI 0 cluster_size]
        resume_process [srv -1 pid]
        resume_process [srv -2 pid]
    }
}

} ;# tags

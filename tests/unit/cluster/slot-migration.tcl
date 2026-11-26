proc get_cluster_role {srv_idx} {
    set flags [dict get [cluster_get_myself $srv_idx] flags]
    set role [lindex $flags 1]
    return $role
}

proc get_myself_primary_flags {srv_idx} {
    set flags [dict get [cluster_get_myself_primary $srv_idx] flags]
    return $flags
}

proc get_myself_primary_linkstate {srv_idx} {
    set linkstate [dict get [cluster_get_myself_primary $srv_idx] linkstate]
    return $linkstate
}

proc get_port {instance_id} {
    if {$::tls} {
        set port [lindex [R $instance_id CONFIG GET tls-port] 1]
    } else {
        set port [lindex [R $instance_id CONFIG GET port] 1]
    }
}

proc wait_for_role {srv_idx role} {
    # Wait for the role, make sure the replication role matches.
    wait_for_condition 100 100 {
        [lindex [split [R $srv_idx ROLE] " "] 0] eq $role
    } else {
        puts "R $srv_idx ROLE: [R $srv_idx ROLE]"
        fail "R $srv_idx didn't assume the replication $role in time"
    }

    if {$role eq "slave"} {
        # Wait for the replication link, make sure the replication link is normal.
        wait_for_condition 100 100 {
            [s -$srv_idx master_link_status] eq "up"
        } else {
            puts "R $srv_idx INFO REPLICATION: [R $srv_idx INFO REPLICATION]"
            fail "R $srv_idx didn't assume the replication link in time"
        }
    }

    # Wait for the cluster role, make sure the cluster role matches.
    wait_for_condition 100 100 {
        [get_cluster_role $srv_idx] eq $role
    } else {
        puts "R $srv_idx CLUSTER NODES: [R $srv_idx CLUSTER NODES]"
        fail "R $srv_idx didn't assume the cluster $role in time"
    }

    if {$role eq "slave"} {
        # Wait for the flags, make sure the primary node is not failed.
        wait_for_condition 100 100 {
            [get_myself_primary_flags $srv_idx] eq "master"
        } else {
            puts "R $srv_idx CLUSTER NODES: [R $srv_idx CLUSTER NODES]"
            fail "R $srv_idx didn't assume the primary state in time"
        }

        # Wait for the cluster link, make sure that the cluster connection is normal.
        wait_for_condition 100 100 {
            [get_myself_primary_linkstate $srv_idx] eq "connected"
        } else {
            puts "R $srv_idx CLUSTER NODES: [R $srv_idx CLUSTER NODES]"
            fail "R $srv_idx didn't assume the cluster primary link in time"
        }
    }

    wait_for_cluster_propagation
}

# Freeze $server_id until $peer has taken its shard over.
proc fail_server {server_id peer} {
    set node_timeout [lindex [R 0 CONFIG GET cluster-node-timeout] 1]
    # timeout is reduced to make elections happen faster
    config_set_all_nodes cluster-node-timeout 1000
    pause_process [srv [expr -1*$server_id] pid]
    set wait_error [catch {
        wait_for_condition 200 100 {
            [get_cluster_role $peer] eq "master"
        } else {
            fail "R $peer didn't take over from R $server_id in time"
        }
    } wait_result wait_options]
    resume_process [srv [expr -1*$server_id] pid]
    config_set_all_nodes cluster-node-timeout $node_timeout
    if {$wait_error} {
        return -options $wait_options $wait_result
    }
    wait_for_role $server_id slave
}

proc migrate_slot {from to slot} {
    set from_id [R $from CLUSTER MYID]
    set to_id [R $to CLUSTER MYID]
    assert_equal {OK} [R $from CLUSTER SETSLOT $slot MIGRATING $to_id]
    assert_equal {OK} [R $to CLUSTER SETSLOT $slot IMPORTING $from_id]
}

# Returns 1 when no node in the cluster flags $node_id as pfail or fail.
proc node_seen_healthy_by_all {node_id} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        set node [cluster_get_node_by_id $j $node_id]
        # A node turned into an empty node no longer knows the whole cluster.
        if {$node eq {}} continue
        if {[cluster_has_flag $node fail?] || [cluster_has_flag $node fail]} {
            return 0
        }
    }
    return 1
}

# Returns 1 when every node in the cluster sees $node_id as a replica.
proc node_seen_as_replica_by_all {node_id} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        set node [cluster_get_node_by_id $j $node_id]
        # A node turned into an empty node no longer knows the whole cluster.
        if {$node eq {}} continue
        if {![cluster_has_flag $node slave]} {
            return 0
        }
    }
    return 1
}

# Give the shard back to $primary with a manual failover.
proc failback {primary} {
    set primary_id [R $primary CLUSTER MYID]
    wait_for_condition 100 100 {
        [node_seen_healthy_by_all $primary_id] == 1
    } else {
        fail "node $primary_id is still flagged as failing by some node"
    }
    # A node can process a packet that $primary sent as a primary after the
    # one that announced its replica role, and then sees it as a primary until
    # its next packet. Such a node ignores its MFSTART or denies it the vote.
    wait_for_condition 100 100 {
        [node_seen_as_replica_by_all $primary_id] == 1
    } else {
        fail "node $primary_id is still seen as a primary by some node"
    }
    # A manual failover is refused while $primary has no link to the node it
    # replicates or sees it as failing, for example right after $primary was
    # resumed.
    wait_for_condition 100 100 {
        [set n [cluster_get_node_by_id $primary [dict get [cluster_get_myself $primary] slaveof]]] ne {} &&
        [dict get $n linkstate] eq "connected" &&
        ![cluster_has_flag $n fail?] && ![cluster_has_flag $n fail]
    } else {
        fail "node $primary_id has no healthy link to the node it replicates"
    }
    assert_equal {OK} [R $primary cluster failover]
    wait_for_role $primary master
}

# Restore the layout the tests assume.
proc normalize_cluster {pairs} {
    # A failed failover_and_back_unseen_by can leave a node dropping cluster
    # bus packets, which would break every later test in the block.
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        R $j DEBUG DROP-CLUSTER-PACKET-FILTER -1
    }
    foreach {primary replica} $pairs {
        if {[get_cluster_role $primary] eq "slave"} {
            # $replica took the shard over and never gave it back.
            failback $primary
        } elseif {[get_cluster_role $replica] ne "slave"} {
            # $replica was turned into an empty node and never re-attached.
            assert_equal {OK} [R $replica CLUSTER MEET [srv [expr {-1*$primary}] host] [srv [expr {-1*$primary}] port]]
            wait_for_role $primary master
            assert_equal {OK} [R $replica CLUSTER REPLICATE [R $primary CLUSTER MYID]]
            wait_for_role $replica slave
        }
    }
}

start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no cluster-node-timeout 5000} } {

    set shard_pairs {0 3 1 4 2 5}
    set R0_id [R 0 CLUSTER MYID]
    set R1_id [R 1 CLUSTER MYID]
    set R2_id [R 2 CLUSTER MYID]
    set R3_id [R 3 CLUSTER MYID]
    set R4_id [R 4 CLUSTER MYID]
    set R5_id [R 5 CLUSTER MYID]
    R 0 SET "{aga}2" banana

    test "Slot migration states are replicated" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # Validate initial states
        assert_not_equal [get_open_slots 0] "\[609->-$R1_id\]"
        assert_not_equal [get_open_slots 1] "\[609-<-$R0_id\]"
        assert_not_equal [get_open_slots 3] "\[609->-$R1_id\]"
        assert_not_equal [get_open_slots 4] "\[609-<-$R0_id\]"
        # Kick off the migration of slot 609 from R0 to R1
        assert_equal {OK} [R 0 CLUSTER SETSLOT 609 MIGRATING $R1_id]
        assert_equal {OK} [R 1 CLUSTER SETSLOT 609 IMPORTING $R0_id]
        # Validate that R0 is migrating slot 609 to R1
        assert_equal [get_open_slots 0] "\[609->-$R1_id\]"
        # Validate that R1 is importing slot 609 from R0 
        assert_equal [get_open_slots 1] "\[609-<-$R0_id\]"
        # Validate final states
        wait_for_slot_state 0 "\[609->-$R1_id\]"
        wait_for_slot_state 1 "\[609-<-$R0_id\]"
        wait_for_slot_state 3 "\[609->-$R1_id\]"
        wait_for_slot_state 4 "\[609-<-$R0_id\]"
    }

    test "Migration target is auto-updated after failover in target shard" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # Trigger an auto-failover from R1 to R4
        fail_server 1 4
        # Validate final states
        wait_for_slot_state 0 "\[609->-$R4_id\]"
        wait_for_slot_state 1 "\[609-<-$R0_id\]"
        wait_for_slot_state 3 "\[609->-$R4_id\]"
        wait_for_slot_state 4 "\[609-<-$R0_id\]"
        # Restore R1's primaryship
        failback 1
        # Validate initial states
        wait_for_slot_state 0 "\[609->-$R1_id\]"
        wait_for_slot_state 1 "\[609-<-$R0_id\]"
        wait_for_slot_state 3 "\[609->-$R1_id\]"
        wait_for_slot_state 4 "\[609-<-$R0_id\]"
    }

    test "Migration source is auto-updated after failover in source shard" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # Trigger an auto-failover from R0 to R3
        fail_server 0 3
        # Validate final states
        wait_for_slot_state 0 "\[609->-$R1_id\]"
        wait_for_slot_state 1 "\[609-<-$R3_id\]"
        wait_for_slot_state 3 "\[609->-$R1_id\]"
        wait_for_slot_state 4 "\[609-<-$R3_id\]"
        # Restore R0's primaryship
        failback 0
        # Validate final states
        wait_for_slot_state 0 "\[609->-$R1_id\]"
        wait_for_slot_state 1 "\[609-<-$R0_id\]"
        wait_for_slot_state 3 "\[609->-$R1_id\]"
        wait_for_slot_state 4 "\[609-<-$R0_id\]"
    }

    test "Replica redirects key access in migrating slots" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # Validate initial states. Use wait_for_slot_state since the replicas
        # (R3/R4) learn the migration state via gossip, which may still be
        # propagating when this test starts.
        wait_for_slot_state 0 "\[609->-$R1_id\]"
        wait_for_slot_state 1 "\[609-<-$R0_id\]"
        wait_for_slot_state 3 "\[609->-$R1_id\]"
        wait_for_slot_state 4 "\[609-<-$R0_id\]"
        catch {[R 3 get aga]} e
        set port0 [srv 0 port]
        assert_equal "MOVED 609 127.0.0.1:$port0" $e
    }

    test "Replica of migrating node returns ASK redirect after READONLY" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # Validate initial states. Use wait_for_slot_state since the replicas
        # (R3/R4) learn the migration state via gossip, which may still be
        # propagating when this test starts.
        wait_for_slot_state 0 "\[609->-$R1_id\]"
        wait_for_slot_state 1 "\[609-<-$R0_id\]"
        wait_for_slot_state 3 "\[609->-$R1_id\]"
        wait_for_slot_state 4 "\[609-<-$R0_id\]"
        # Read missing key in readonly replica in migrating state.
        assert_equal OK [R 3 READONLY]
        set port1 [srv -1 port]
        catch {[R 3 get aga]} e
        assert_equal "ASK 609 127.0.0.1:$port1" $e
        assert_equal OK [R 3 READWRITE]
    }

    test "Replica of migrating node returns TRYAGAIN after READONLY" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # Validate initial states. Use wait_for_slot_state since the replicas
        # (R3/R4) learn the migration state via gossip, which may still be
        # propagating when this test starts.
        wait_for_slot_state 0 "\[609->-$R1_id\]"
        wait_for_slot_state 1 "\[609-<-$R0_id\]"
        wait_for_slot_state 3 "\[609->-$R1_id\]"
        wait_for_slot_state 4 "\[609-<-$R0_id\]"
        # Read some existing and some missing keys in readonly replica in
        # migrating state results in TRYAGAIN, just like its primary would do.
        assert_equal OK [R 3 READONLY]
        catch {[R 3 mget "{aga}1" "{aga}2"]} e
        assert_match "TRYAGAIN *" $e
        assert_equal OK [R 3 READWRITE]
    }

    test "Replica of importing node returns TRYAGAIN after READONLY and ASKING" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # Validate initial states. Use wait_for_slot_state since the replicas
        # (R3/R4) learn the migration state via gossip, which may still be
        # propagating when this test starts.
        wait_for_slot_state 0 "\[609->-$R1_id\]"
        wait_for_slot_state 1 "\[609-<-$R0_id\]"
        wait_for_slot_state 3 "\[609->-$R1_id\]"
        wait_for_slot_state 4 "\[609-<-$R0_id\]"
        # A client follows an ASK redirect to a primary, but wants to read from a replica.
        # The replica returns TRYAGAIN just like a primary would do for two missing keys.
        assert_equal OK [R 4 READONLY]
        assert_equal OK [R 4 ASKING]
        catch {R 4 MGET "{aga}1" "{aga}2"} e
        assert_match "TRYAGAIN *" $e
        assert_equal OK [R 4 READWRITE]
    }

    test "New replica inherits migrating slot" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # Reset R3 to turn it into an empty node
        assert_equal [get_open_slots 3] "\[609->-$R1_id\]"
        assert_equal {OK} [R 3 CLUSTER RESET]
        assert_not_equal [get_open_slots 3] "\[609->-$R1_id\]"
        # Add R3 back as a replica of R0
        assert_equal {OK} [R 3 CLUSTER MEET [srv 0 "host"] [srv 0 "port"]]
        wait_for_role 0 master
        assert_equal {OK} [R 3 CLUSTER REPLICATE $R0_id]
        wait_for_role 3 slave
        # Validate that R3 now sees slot 609 open
        wait_for_slot_state 3 "\[609->-$R1_id\]"
    }

    test "New replica inherits importing slot" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # Reset R4 to turn it into an empty node
        assert_equal [get_open_slots 4] "\[609-<-$R0_id\]"
        assert_equal {OK} [R 4 CLUSTER RESET]
        assert_not_equal [get_open_slots 4] "\[609-<-$R0_id\]"
        # Add R4 back as a replica of R1
        assert_equal {OK} [R 4 CLUSTER MEET [srv -1 "host"] [srv -1 "port"]]
        wait_for_role 1 master
        assert_equal {OK} [R 4 CLUSTER REPLICATE $R1_id]
        wait_for_role 4 slave
        # Validate that R4 now sees slot 609 open
        wait_for_slot_state 4 "\[609-<-$R0_id\]"
    }
}

proc create_empty_shard {p r} {
    assert_equal {OK} [R $p CLUSTER RESET]
    assert_equal {OK} [R $r CLUSTER RESET]
    assert_equal {OK} [R $p CLUSTER MEET [srv 0 "host"] [srv 0 "port"]]
    assert_equal {OK} [R $r CLUSTER MEET [srv 0 "host"] [srv 0 "port"]]
    wait_for_role $p master
    assert_equal {OK} [R $r CLUSTER REPLICATE [R $p CLUSTER MYID]]
    wait_for_role $r slave
    wait_for_role $p master
}

# Returns 1 when R<from>'s outbound link to the node was created after the last
# PONG R<from> got from it. Until that PONG, R<from> sends full-header PUBLISH
# messages on the link, which carry its config epoch.
proc link_unsettled {from to_id} {
    set link {}
    foreach l [R $from CLUSTER LINKS] {
        if {[dict get $l node] eq $to_id && [dict get $l direction] eq "to"} { set link $l }
    }
    if {$link eq {}} { return 0 }
    set pong [dict get [cluster_get_node_by_id $from $to_id] pong_recv]
    return [expr {[dict get $link create-time] > $pong}]
}

# Returns the shard id that R<idx> records for the node, from CLUSTER SHARDS.
proc node_shard_id {idx node_id} {
    foreach shard [R $idx CLUSTER SHARDS] {
        foreach n [dict get $shard nodes] {
            if {[dict get $n id] eq $node_id} { return [dict get $shard id] }
        }
    }
    return {}
}

# Fail over from $primary to $replica and back, while $observer drops every
# cluster bus packet from the moment $primary is about to turn into a replica
# until it is a primary again. $observer never sees $primary as a replica, so
# the newer config epoch is the only sign it gets that $primary is the primary
# of the shard again.
#
# $first_seen chooses the packet that first carries that config epoch to
# $observer:
# - ping: a regular PING or PONG.
# - handshake: the PONG that answers a MEET. $observer knows $primary already,
#   so it discards the packet.
# - publish: a full-header PUBLISH, sent before $primary's first PONG on a new
#   link to $observer. Only PINGs and PONGs reach the slot checks.
proc failover_and_back_unseen_by {primary replica observer {first_seen ping}} {
    set CLUSTER_PACKET_TYPE_PING 0
    set CLUSTER_PACKET_TYPE_NONE -1
    set CLUSTER_PACKET_TYPE_ALL -2
    set primary_id [R $primary CLUSTER MYID]
    set replica_id [R $replica CLUSTER MYID]

    # Freeze $primary until $observer has seen $replica take its shard over.
    # A takeover needs no vote, so the nodes don't have to detect a failure.
    pause_process [srv [expr -1*$primary] pid]
    set wait_error [catch {
        assert_equal {OK} [R $replica CLUSTER FAILOVER TAKEOVER]
        wait_for_condition 100 100 {
            [cluster_has_flag [cluster_get_node_by_id $observer $replica_id] master]
        } else {
            fail "R $observer didn't see R $replica take over from R $primary in time"
        }
        R $observer DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_ALL
    } wait_result wait_options]
    resume_process [srv [expr -1*$primary] pid]
    if {$wait_error} {
        return -options $wait_options $wait_result
    }

    wait_for_condition 100 100 {
        [get_cluster_role $primary] eq "slave"
    } else {
        fail "R $primary didn't become a replica of R $replica in time"
    }
    assert_equal {OK} [R $primary CLUSTER FAILOVER TAKEOVER]
    wait_for_condition 100 100 {
        [get_cluster_role $primary] eq "master"
    } else {
        fail "R $primary didn't take over from R $replica in time"
    }

    # Once the other nodes have seen $primary take over, $observer has dropped
    # what $primary sent it as a replica.
    set primary_epoch [dict get [cluster_get_myself $primary] config_epoch]
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {$j == $observer} continue
        wait_for_condition 100 100 {
            [cluster_has_flag [cluster_get_node_by_id $j $primary_id] master] &&
            [dict get [cluster_get_node_by_id $j $primary_id] config_epoch] == $primary_epoch
        } else {
            fail "R $j didn't see R $primary take over from R $replica in time"
        }
    }
    assert {[cluster_has_flag [cluster_get_node_by_id $observer $primary_id] master]}

    if {$first_seen eq "handshake"} {
        # The ping of $observer to $primary must be pending, so that the PONG
        # of the handshake link is the only one $primary sends it.
        wait_for_condition 100 100 {
            [dict get [cluster_get_node_by_id $observer $primary_id] ping_sent] != 0
        } else {
            fail "R $observer has no pending ping to R $primary"
        }
        set loglevel [lindex [R $observer CONFIG GET loglevel] 1]
        R $observer CONFIG SET loglevel verbose
        set loglines [count_log_lines [expr -1*$observer]]
        R $observer DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_PING
        assert_equal {OK} [R $observer CLUSTER MEET [srv [expr -1*$primary] host] [srv [expr -1*$primary] port]]
        wait_for_log_messages [expr -1*$observer] [list "*Handshake: we already know node $primary_id*"] $loglines 100 100
        R $observer CONFIG SET loglevel $loglevel
    }

    if {$first_seen eq "publish"} {
        # $observer dropped every packet long enough for $primary to reconnect
        # its link to $observer, so that link has not seen a PONG yet.
        set observer_id [R $observer CLUSTER MYID]
        wait_for_condition 300 100 {
            [link_unsettled $primary $observer_id]
        } else {
            fail "R $primary link to R $observer never became unsettled"
        }
        set sub [valkey_deferring_client [expr -1*$observer]]
        $sub subscribe first-epoch-ch
        $sub read
        R $observer DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_PING
        R $primary PUBLISH first-epoch-ch hello
        assert_equal {message first-epoch-ch hello} [$sub read]
        $sub close
        # The full-header PUBLISH recorded the newer config epoch.
        assert_equal $primary_epoch [dict get [cluster_get_node_by_id $observer $primary_id] config_epoch]
    }

    lassign [R $observer TIME] sec usec
    set now [expr {$sec * 1000 + $usec / 1000}]
    R $observer DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTER_PACKET_TYPE_NONE

    # The pings sent to and by $observer in the meantime are still pending.
    # Wait for a newer PONG in both directions, so that a lower
    # cluster-node-timeout in a later test does not turn them into a failure.
    set observer_id [R $observer CLUSTER MYID]
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {$j == $observer} continue
        set node_id [R $j CLUSTER MYID]
        wait_for_condition 100 100 {
            [dict get [cluster_get_node_by_id $j $observer_id] pong_recv] > $now &&
            [dict get [cluster_get_node_by_id $observer $node_id] pong_recv] > $now
        } else {
            fail "R $observer and R $j didn't exchange a PONG in time"
        }
    }
}

start_cluster 3 5 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no cluster-node-timeout 5000} } {

    set R0_id [R 0 CLUSTER MYID]
    set R1_id [R 1 CLUSTER MYID]
    set R2_id [R 2 CLUSTER MYID]
    set R3_id [R 3 CLUSTER MYID]
    set R4_id [R 4 CLUSTER MYID]
    set R5_id [R 5 CLUSTER MYID]

    create_empty_shard 6 7
    set R6_id [R 6 CLUSTER MYID]
    set R7_id [R 7 CLUSTER MYID]
    set shard_pairs {0 3 1 4 2 5 6 7}

    test "Empty-shard migration replicates slot importing states" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # Validate initial states
        assert_not_equal [get_open_slots 0] "\[609->-$R6_id\]"
        assert_not_equal [get_open_slots 6] "\[609-<-$R0_id\]"
        assert_not_equal [get_open_slots 3] "\[609->-$R6_id\]"
        assert_not_equal [get_open_slots 7] "\[609-<-$R0_id\]"
        # Kick off the migration of slot 609 from R0 to R6
        assert_equal {OK} [R 0 CLUSTER SETSLOT 609 MIGRATING $R6_id]
        assert_equal {OK} [R 6 CLUSTER SETSLOT 609 IMPORTING $R0_id]
        # Validate that R0 is migrating slot 609 to R6
        assert_equal [get_open_slots 0] "\[609->-$R6_id\]"
        # Validate that R6 is importing slot 609 from R0 
        assert_equal [get_open_slots 6] "\[609-<-$R0_id\]"
        # Validate final states
        wait_for_slot_state 0 "\[609->-$R6_id\]"
        wait_for_slot_state 6 "\[609-<-$R0_id\]"
        wait_for_slot_state 3 "\[609->-$R6_id\]"
        wait_for_slot_state 7 "\[609-<-$R0_id\]"
    }

    test "Empty-shard migration target is auto-updated after failover in target shard" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # Trigger an auto-failover from R6 to R7
        fail_server 6 7
        # Validate final states
        wait_for_slot_state 0 "\[609->-$R7_id\]"
        wait_for_slot_state 6 "\[609-<-$R0_id\]"
        wait_for_slot_state 3 "\[609->-$R7_id\]"
        wait_for_slot_state 7 "\[609-<-$R0_id\]"
        # Restore R6's primaryship
        failback 6
        # Validate final states
        wait_for_slot_state 0 "\[609->-$R6_id\]"
        wait_for_slot_state 6 "\[609-<-$R0_id\]"
        wait_for_slot_state 3 "\[609->-$R6_id\]"
        wait_for_slot_state 7 "\[609-<-$R0_id\]"
    }

    test "Empty-shard migration source is auto-updated after failover in source shard" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # Trigger an auto-failover from R0 to R3
        fail_server 0 3
        # Validate final states
        wait_for_slot_state 0 "\[609->-$R6_id\]"
        wait_for_slot_state 6 "\[609-<-$R3_id\]"
        wait_for_slot_state 3 "\[609->-$R6_id\]"
        wait_for_slot_state 7 "\[609-<-$R3_id\]"
        # Restore R0's primaryship
        failback 0
        # Validate final states
        wait_for_slot_state 0 "\[609->-$R6_id\]"
        wait_for_slot_state 6 "\[609-<-$R0_id\]"
        wait_for_slot_state 3 "\[609->-$R6_id\]"
        wait_for_slot_state 7 "\[609-<-$R0_id\]"
    }

    test "Empty-shard migration target is auto-updated after a failover seen only as a newer config epoch" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # R3 sees the failover from R6 to R7, but not the one back to R6
        failover_and_back_unseen_by 6 7 3
        wait_for_role 6 master
        wait_for_role 7 slave
        # Validate final states
        wait_for_slot_state 0 "\[609->-$R6_id\]"
        wait_for_slot_state 6 "\[609-<-$R0_id\]"
        wait_for_slot_state 3 "\[609->-$R6_id\]"
        wait_for_slot_state 7 "\[609-<-$R0_id\]"
    }

    test "Empty-shard migration target is auto-updated after a newer config epoch first seen on a handshake link" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # R3 misses both failovers and discards the first PONG of R6 after them
        failover_and_back_unseen_by 6 7 3 handshake
        wait_for_role 6 master
        wait_for_role 7 slave
        # Validate final states
        wait_for_slot_state 0 "\[609->-$R6_id\]"
        wait_for_slot_state 6 "\[609-<-$R0_id\]"
        wait_for_slot_state 3 "\[609->-$R6_id\]"
        wait_for_slot_state 7 "\[609-<-$R0_id\]"
    }

    test "Empty-shard migration source primary is auto-updated after a failover seen only as a newer config epoch" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # The source primary R0 sees the failover from R6 to R7, but not the one back to R6
        failover_and_back_unseen_by 6 7 0
        wait_for_role 6 master
        wait_for_role 7 slave
        # Validate final states
        wait_for_slot_state 0 "\[609->-$R6_id\]"
        wait_for_slot_state 6 "\[609-<-$R0_id\]"
        wait_for_slot_state 3 "\[609->-$R6_id\]"
        wait_for_slot_state 7 "\[609-<-$R0_id\]"
    }

    test "Empty-shard migration target is auto-updated after a newer config epoch first seen in a PUBLISH" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # R3 misses both failovers and records R6's newer epoch from a PUBLISH
        failover_and_back_unseen_by 6 7 3 publish
        wait_for_role 6 master
        wait_for_role 7 slave
        # Validate final states
        wait_for_slot_state 0 "\[609->-$R6_id\]"
        wait_for_slot_state 6 "\[609-<-$R0_id\]"
        wait_for_slot_state 7 "\[609-<-$R0_id\]"
        wait_for_slot_state 3 "\[609->-$R6_id\]"
    }

    test "Empty-shard migration target follows a takeover by a node last seen as the primary of another shard" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # An earlier failing test may have left R3 on another target.
        if {[get_open_slots 3] ne "\[609->-$R6_id\]"} {
            assert_equal {OK} [R 0 CLUSTER SETSLOT 609 MIGRATING $R6_id]
        }
        wait_for_slot_state 0 "\[609->-$R6_id\]"
        wait_for_slot_state 3 "\[609->-$R6_id\]"
        # R7 leaves for its own empty shard, and R3 sees it there as a primary.
        assert_equal {OK} [R 7 CLUSTER RESET]
        assert_equal {OK} [R 7 CLUSTER MEET [srv 0 host] [srv 0 port]]
        set R7_old_shard [R 7 CLUSTER MYSHARDID]
        wait_for_condition 200 100 {
            [catch {cluster_get_node_by_id 3 $R7_id} n] == 0 && $n ne {} &&
            [cluster_has_flag $n master] && [node_shard_id 3 $R7_id] eq $R7_old_shard
        } else {
            fail "R3 never saw R7 as a primary of its own shard"
        }
        wait_for_condition 200 100 {
            [catch {cluster_get_node_by_id 7 $R6_id} n] == 0 && $n ne {}
        } else {
            fail "R7 never learned R6"
        }
        assert_equal "\[609->-$R6_id\]" [get_open_slots 3]
        # R3 misses R7 joining R6's shard and taking it over, so the first
        # packet with R7's newer epoch also carries its new shard id.
        R 3 DEBUG DROP-CLUSTER-PACKET-FILTER -2
        assert_equal {OK} [R 7 CLUSTER REPLICATE $R6_id]
        wait_for_role 7 slave
        set shard [R 6 CLUSTER MYSHARDID]
        pause_process [srv -6 pid]
        assert_equal {OK} [R 7 CLUSTER FAILOVER TAKEOVER]
        wait_for_condition 100 100 {
            [get_cluster_role 7] eq "master"
        } else {
            fail "R7 did not take over"
        }
        set e7 [dict get [cluster_get_myself 7] config_epoch]
        wait_for_condition 100 100 {
            [dict get [cluster_get_node_by_id 0 $R7_id] config_epoch] == $e7
        } else {
            fail "R0 did not see R7 take over"
        }
        wait_for_slot_state 0 "\[609->-$R7_id\]"
        # R6 is paused, so R3 hears from R7 first.
        R 3 DEBUG DROP-CLUSTER-PACKET-FILTER -1
        wait_for_condition 300 100 {
            [dict get [cluster_get_node_by_id 3 $R7_id] config_epoch] == $e7 &&
            [node_shard_id 3 $R7_id] eq $shard
        } else {
            fail "R3 did not record R7's takeover"
        }
        resume_process [srv -6 pid]
        wait_for_slot_state 3 "\[609->-$R7_id\]"
    }

    test "Empty-shard migration target is kept when a primary of the target shard moves to another shard" {
        test_defer_fail { normalize_cluster $shard_pairs }
        # The previous test leaves R7 as the primary. Wait until R6 follows
        # it, and give the shard back to R6.
        wait_for_condition 100 100 {
            [get_cluster_role 6] eq "slave" || [get_cluster_role 7] eq "slave"
        } else {
            fail "The shard of R6 and R7 has no replica"
        }
        normalize_cluster $shard_pairs
        wait_for_slot_state 0 "\[609->-$R6_id\]"
        wait_for_slot_state 3 "\[609->-$R6_id\]"
        # R7 takes the shard over from R6, but does not see R6 take it back,
        # so it stays a primary of the shard with an older config epoch
        failover_and_back_unseen_by 6 7 7
        wait_for_slot_state 3 "\[609->-$R6_id\]"
        assert {[cluster_has_flag [cluster_get_node_by_id 3 $R7_id] master]}
        # R3 misses that R7 leaves for a shard of its own and gets the highest
        # config epoch, so one packet tells it both
        R 3 DEBUG DROP-CLUSTER-PACKET-FILTER -2
        test_defer { R 3 DEBUG DROP-CLUSTER-PACKET-FILTER -1 }
        assert_equal {OK} [R 7 CLUSTER RESET]
        assert_equal {OK} [R 7 CLUSTER MEET [srv 0 host] [srv 0 port]]
        wait_for_condition 200 100 {
            [catch {cluster_get_node_by_id 7 $R6_id} n] == 0 && $n ne {}
        } else {
            fail "R7 never learned R6"
        }
        set reply [R 7 CLUSTER BUMPEPOCH]
        R 3 DEBUG DROP-CLUSTER-PACKET-FILTER -1
        assert_match {BUMPED *} $reply
        set e7 [lindex $reply 1]
        wait_for_condition 300 100 {
            [dict get [cluster_get_node_by_id 3 $R7_id] config_epoch] == $e7
        } else {
            fail "R3 did not see R7 at config epoch $e7"
        }
        # R7 is not in the shard of R6 anymore
        assert_equal "\[609->-$R6_id\]" [get_open_slots 3]
    }
}

# Build the fixed clusterbus message header of a primary that owns no slots,
# with explicit type, currentEpoch and configEpoch. Layout follows
# create_cluster_meet_packet in packet.tcl. The caller appends the message
# body and then calls finish_cluster_packet.
proc create_primary_header {type sender_name sender_port sender_cport current_epoch config_epoch ext_count mflags0} {
    set CLUSTER_NAMELEN 40
    set CLUSTER_SLOTS 16384
    set NET_IP_STR_LEN 46

    set packet "RCmb"
    append packet [binary format I 0]
    append packet [binary format S 1]
    append packet [binary format S $sender_port]
    append packet [binary format S $type]
    append packet [binary format S 0]
    append packet [binary format W $current_epoch]
    append packet [binary format W $config_epoch]
    append packet [binary format W 0]
    set sender_padded [string range "${sender_name}[string repeat "\x00" $CLUSTER_NAMELEN]" 0 [expr {$CLUSTER_NAMELEN - 1}]]
    append packet $sender_padded
    append packet [string repeat "\x00" [expr {$CLUSTER_SLOTS / 8}]]
    append packet [string repeat "\x00" $CLUSTER_NAMELEN]
    append packet [string repeat "\x00" $NET_IP_STR_LEN]
    append packet [binary format S $ext_count]
    append packet [string repeat "\x00" 30]
    append packet [binary format S 0]
    append packet [binary format S $sender_cport]
    # Primary flag (CLUSTER_NODE_PRIMARY), state CLUSTER_OK, mflags.
    append packet [binary format S 1]
    append packet [binary format c 0]
    append packet [binary format ccc $mflags0 0 0]
    return $packet
}

# Write the total length into the header.
proc finish_cluster_packet {packet} {
    return [string replace $packet 4 7 [binary format I [string length $packet]]]
}

# A PING with a shard id extension.
proc create_primary_ping {sender_name sender_port sender_cport current_epoch config_epoch shard_id} {
    set CLUSTERMSG_TYPE_PING 0
    set CLUSTERMSG_EXT_TYPE_SHARDID 3
    # One extension, and the EXT_DATA message flag (1 << 2).
    set packet [create_primary_header $CLUSTERMSG_TYPE_PING $sender_name $sender_port $sender_cport \
        $current_epoch $config_epoch 1 4]

    # Shard id extension: length (uint32), type (uint16), unused (uint16),
    # then the NUL-terminated shard id padded to eight bytes.
    set datalen [expr {(([string length $shard_id] + 1 + 7) / 8) * 8}]
    append packet [binary format ISS [expr {8 + $datalen}] $CLUSTERMSG_EXT_TYPE_SHARDID 0]
    append packet $shard_id [string repeat "\x00" [expr {$datalen - [string length $shard_id]}]]
    return [finish_cluster_packet $packet]
}

# A FAIL message about a node the receiver does not know, so the receiver only
# processes its header.
proc create_primary_fail {sender_name sender_port sender_cport current_epoch config_epoch} {
    set CLUSTERMSG_TYPE_FAIL 3
    set packet [create_primary_header $CLUSTERMSG_TYPE_FAIL $sender_name $sender_port $sender_cport \
        $current_epoch $config_epoch 0 0]
    append packet [string repeat "f" 40]
    return [finish_cluster_packet $packet]
}

proc send_cluster_packet {port packet} {
    set sock [socket 127.0.0.1 [expr {$port + 10000}]]
    fconfigure $sock -translation binary -buffering none -blocking 1
    puts -nonewline $sock $packet
    flush $sock
    close $sock
}

# Regression test for a stale primary claim in an empty target shard. The
# observer R3 records R6 as a replica of R7. With R6 paused, R3 gets a stale
# primary claim from R6 (its pre-failover epoch), so R3 sees R6 as a primary
# again. R3 then gets R6's primary claim at a newer epoch, as after a failback.
# R6 owns no slots, so only the epoch change shows the failover, and R3 must
# move slot 609's migration target from R7 to R6.
start_cluster 3 5 {tags {tls:skip external:skip cluster} overrides {cluster-allow-replica-migration no cluster-node-timeout 5000} } {

    set R0_id [R 0 CLUSTER MYID]
    set R3_id [R 3 CLUSTER MYID]

    create_empty_shard 6 7
    set R6_id [R 6 CLUSTER MYID]
    set R7_id [R 7 CLUSTER MYID]

    test "Migration target follows a newer epoch after a stale primary claim" {
        # Start the migration of slot 609 from R0 to the empty shard R6.
        assert_equal {OK} [R 0 CLUSTER SETSLOT 609 MIGRATING $R6_id]
        assert_equal {OK} [R 6 CLUSTER SETSLOT 609 IMPORTING $R0_id]
        wait_for_slot_state 0 "\[609->-$R6_id\]"
        wait_for_slot_state 3 "\[609->-$R6_id\]"

        # R6's configEpoch and shard id before the failover. R7 gets a higher epoch.
        set E6 [dict get [cluster_get_myself 6] config_epoch]
        set shard_id [R 6 cluster myshardid]

        # Promote R7 by failing R6. R6 comes back as a replica of R7.
        fail_server 6 7
        set E7 [dict get [cluster_get_myself 7] config_epoch]
        assert {$E7 > $E6}
        wait_for_slot_state 0 "\[609->-$R7_id\]"
        wait_for_slot_state 3 "\[609->-$R7_id\]"

        # Observer state: R3 records R6 as a replica of R7 at epoch E7.
        wait_for_condition 100 100 {
            [cluster_node_is_replica_of 3 $R6_id $R7_id]
        } else {
            fail "R3 never recorded R6 as a replica of R7"
        }
        wait_for_condition 100 100 {
            [dict get [cluster_get_node_by_id 3 $R7_id] config_epoch] == $E7
        } else {
            fail "R3 never saw R7 at config epoch $E7"
        }

        # Pause R6 so that its own replica claims cannot correct R3 between
        # the two forged PINGs.
        pause_process [srv -6 pid]
        # Stale primary claim from R6 with its pre-failover configEpoch.
        # R3 accepts it and sees R6 as a primary again.
        send_cluster_packet [srv -3 port] [create_primary_ping $R6_id [srv -6 port] \
            [expr {[srv -6 port] + 10000}] $E6 $E6 $shard_id]
        wait_for_condition 1000 10 {
            [cluster_has_flag [cluster_get_node_by_id 3 $R6_id] master]
        } else {
            fail "R3 did not process the stale primary claim"
        }
        assert_equal "\[609->-$R7_id\]" [get_open_slots 3]

        # R6's primary claim at a newer epoch, as after a failback. Its
        # slots are unchanged, so only the epoch shows the failover.
        set E_new [expr {$E7 + 1}]
        set loglines [count_log_lines -3]
        send_cluster_packet [srv -3 port] [create_primary_ping $R6_id [srv -6 port] \
            [expr {[srv -6 port] + 10000}] $E_new $E_new $shard_id]
        wait_for_log_messages -3 [list "*Slot 609 is now being migrated to node $R6_id*"] $loglines 1000 10
        assert_equal "\[609->-$R6_id\]" [get_open_slots 3]
        resume_process [srv -6 pid]

        # Real failback, so the cluster agrees on R6's role and epoch again.
        # R6's election epoch is at least the forged one, so R3's record of
        # R6 is not ahead of the real one. Until then R3 holds the forged
        # epoch and ignores R6's replica packets as stale, so failback's wait
        # for every node to see R6 as a replica cannot pass. R3 is a replica
        # and does not vote, so fail back directly.
        wait_for_condition 100 100 {
            [node_seen_healthy_by_all $R6_id] == 1
        } else {
            fail "R6 is still flagged as failing by some node"
        }
        assert_equal {OK} [R 6 cluster failover]
        wait_for_role 6 master
        assert {[dict get [cluster_get_myself 6] config_epoch] >= $E_new}

        # Outcome only: R3 and the source R0 end with slot 609 migrating to R6.
        wait_for_slot_state 3 "\[609->-$R6_id\]"
        wait_for_slot_state 0 "\[609->-$R6_id\]"
    }
}

# Same stale and newer primary claims from a paused R6, sent to the migration
# source R0. R0 must move slot 609's migration target from R7 to R6. There is
# no real failback here: forged packets replace R0's link from R6, which could
# drop R6's vote request to R0.
start_cluster 3 5 {tags {tls:skip external:skip cluster} overrides {cluster-allow-replica-migration no cluster-node-timeout 5000} } {

    set R0_id [R 0 CLUSTER MYID]

    create_empty_shard 6 7
    set R6_id [R 6 CLUSTER MYID]
    set R7_id [R 7 CLUSTER MYID]

    test "Migration source follows a newer epoch after a stale primary claim" {
        assert_equal {OK} [R 0 CLUSTER SETSLOT 609 MIGRATING $R6_id]
        assert_equal {OK} [R 6 CLUSTER SETSLOT 609 IMPORTING $R0_id]
        wait_for_slot_state 0 "\[609->-$R6_id\]"

        set E6 [dict get [cluster_get_myself 6] config_epoch]
        set shard_id [R 6 cluster myshardid]

        fail_server 6 7
        set E7 [dict get [cluster_get_myself 7] config_epoch]
        assert {$E7 > $E6}
        wait_for_slot_state 0 "\[609->-$R7_id\]"

        wait_for_condition 100 100 {
            [cluster_node_is_replica_of 0 $R6_id $R7_id]
        } else {
            fail "R0 never recorded R6 as a replica of R7"
        }
        wait_for_condition 100 100 {
            [dict get [cluster_get_node_by_id 0 $R7_id] config_epoch] == $E7
        } else {
            fail "R0 never saw R7 at config epoch $E7"
        }

        pause_process [srv -6 pid]
        send_cluster_packet [srv 0 port] [create_primary_ping $R6_id [srv -6 port] \
            [expr {[srv -6 port] + 10000}] $E6 $E6 $shard_id]
        wait_for_condition 1000 10 {
            [cluster_has_flag [cluster_get_node_by_id 0 $R6_id] master]
        } else {
            fail "R0 did not process the stale primary claim"
        }
        assert_equal "\[609->-$R7_id\]" [get_open_slots 0]

        set E_new [expr {$E7 + 1}]
        set loglines [count_log_lines 0]
        send_cluster_packet [srv 0 port] [create_primary_ping $R6_id [srv -6 port] \
            [expr {[srv -6 port] + 10000}] $E_new $E_new $shard_id]
        wait_for_log_messages 0 [list "*Slot 609 is now being migrated to node $R6_id*"] $loglines 1000 10
        assert_equal "\[609->-$R6_id\]" [get_open_slots 0]
        resume_process [srv -6 pid]
    }
}

# Same stale primary claim from a paused R6 to the observer R3, but the newer
# epoch first arrives in a FAIL message instead of a PING. R3 records the
# epoch from any message type, so the PING that follows carries an epoch R3
# already has. R3 must still move slot 609's migration target from R7 to R6.
start_cluster 3 5 {tags {tls:skip external:skip cluster} overrides {cluster-allow-replica-migration no cluster-node-timeout 5000} } {

    create_empty_shard 6 7
    set R0_id [R 0 CLUSTER MYID]
    set R6_id [R 6 CLUSTER MYID]
    set R7_id [R 7 CLUSTER MYID]

    test "Migration target follows a newer epoch first seen in a non-PING message" {
        assert_equal {OK} [R 0 CLUSTER SETSLOT 609 MIGRATING $R6_id]
        assert_equal {OK} [R 6 CLUSTER SETSLOT 609 IMPORTING $R0_id]
        wait_for_slot_state 3 "\[609->-$R6_id\]"

        set E6 [dict get [cluster_get_myself 6] config_epoch]
        set shard_id [R 6 cluster myshardid]

        fail_server 6 7
        set E7 [dict get [cluster_get_myself 7] config_epoch]
        assert {$E7 > $E6}
        wait_for_slot_state 3 "\[609->-$R7_id\]"
        wait_for_condition 100 100 {
            [cluster_node_is_replica_of 3 $R6_id $R7_id]
        } else {
            fail "R3 never recorded R6 as a replica of R7"
        }

        pause_process [srv -6 pid]
        set R6_cport [expr {[srv -6 port] + 10000}]
        send_cluster_packet [srv -3 port] [create_primary_ping $R6_id [srv -6 port] $R6_cport $E6 $E6 $shard_id]
        wait_for_condition 1000 10 {
            [cluster_has_flag [cluster_get_node_by_id 3 $R6_id] master]
        } else {
            fail "R3 did not process the stale primary claim"
        }
        assert_equal "\[609->-$R7_id\]" [get_open_slots 3]

        set E_new [expr {$E7 + 1}]
        set loglines [count_log_lines -3]
        send_cluster_packet [srv -3 port] [create_primary_fail $R6_id [srv -6 port] $R6_cport $E_new $E_new]
        wait_for_condition 1000 10 {
            [dict get [cluster_get_node_by_id 3 $R6_id] config_epoch] == $E_new
        } else {
            fail "R3 did not record R6's epoch from the FAIL message"
        }
        send_cluster_packet [srv -3 port] [create_primary_ping $R6_id [srv -6 port] $R6_cport $E_new $E_new $shard_id]
        wait_for_log_messages -3 [list "*Slot 609 is now being migrated to node $R6_id*"] $loglines 1000 10
        assert_equal "\[609->-$R6_id\]" [get_open_slots 3]
        resume_process [srv -6 pid]
    }
}

# A stale primary claim from a paused R6 that carries a previous shard id. R3
# moves R6 to that shard, so the first claim at a newer epoch, which carries
# the real shard id, is checked against the old one. The claims that follow
# must still move slot 609's migration target from R7 to R6.
start_cluster 3 5 {tags {tls:skip external:skip cluster} overrides {cluster-allow-replica-migration no cluster-node-timeout 5000} } {

    create_empty_shard 6 7
    set R0_id [R 0 CLUSTER MYID]
    set R6_id [R 6 CLUSTER MYID]
    set R7_id [R 7 CLUSTER MYID]

    test "Migration target follows a newer epoch after a stale claim with a previous shard id" {
        assert_equal {OK} [R 0 CLUSTER SETSLOT 609 MIGRATING $R6_id]
        assert_equal {OK} [R 6 CLUSTER SETSLOT 609 IMPORTING $R0_id]
        wait_for_slot_state 3 "\[609->-$R6_id\]"

        set E6 [dict get [cluster_get_myself 6] config_epoch]
        set shard_id [R 6 cluster myshardid]
        set old_shard_id "0123456789abcdef0123456789abcdef01234567"

        fail_server 6 7
        set E7 [dict get [cluster_get_myself 7] config_epoch]
        assert {$E7 > $E6}
        wait_for_slot_state 3 "\[609->-$R7_id\]"
        wait_for_condition 100 100 {
            [cluster_node_is_replica_of 3 $R6_id $R7_id]
        } else {
            fail "R3 never recorded R6 as a replica of R7"
        }

        pause_process [srv -6 pid]
        set R6_cport [expr {[srv -6 port] + 10000}]
        send_cluster_packet [srv -3 port] [create_primary_ping $R6_id [srv -6 port] $R6_cport $E6 $E6 $old_shard_id]
        wait_for_condition 1000 10 {
            [node_shard_id 3 $R6_id] eq $old_shard_id
        } else {
            fail "R3 did not take the previous shard id from the stale claim"
        }
        assert_equal "\[609->-$R7_id\]" [get_open_slots 3]

        set E_new [expr {$E7 + 1}]
        set loglines [count_log_lines -3]
        foreach i {1 2} {
            send_cluster_packet [srv -3 port] [create_primary_ping $R6_id [srv -6 port] $R6_cport $E_new $E_new $shard_id]
        }
        wait_for_log_messages -3 [list "*Slot 609 is now being migrated to node $R6_id*"] $loglines 1000 10
        assert_equal "\[609->-$R6_id\]" [get_open_slots 3]
        resume_process [srv -6 pid]
    }
}

# Fail over from $primary to $replica and back, while $replica drops every
# cluster bus packet from the moment it is a primary. $replica never sees
# $primary take the shard back, so it stays a primary of the shard with an
# older config epoch. $replica still drops every packet when this returns.
proc stale_primary_after_failback {primary replica} {
    set primary_id [R $primary CLUSTER MYID]
    set replica_id [R $replica CLUSTER MYID]

    pause_process [srv [expr -1*$primary] pid]
    set wait_error [catch {
        assert_equal {OK} [R $replica CLUSTER FAILOVER TAKEOVER]
        wait_for_condition 100 100 {
            [get_cluster_role $replica] eq "master"
        } else {
            fail "R $replica didn't take over from R $primary in time"
        }
        R $replica DEBUG DROP-CLUSTER-PACKET-FILTER -2
    } wait_result wait_options]
    resume_process [srv [expr -1*$primary] pid]
    if {$wait_error} {
        return -options $wait_options $wait_result
    }

    wait_for_condition 100 100 {
        [get_cluster_role $primary] eq "slave"
    } else {
        fail "R $primary didn't become a replica of R $replica in time"
    }
    assert_equal {OK} [R $primary CLUSTER FAILOVER TAKEOVER]
    wait_for_condition 100 100 {
        [get_cluster_role $primary] eq "master"
    } else {
        fail "R $primary didn't take over from R $replica in time"
    }
    set primary_epoch [dict get [cluster_get_myself $primary] config_epoch]
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {$j == $replica} continue
        wait_for_condition 100 100 {
            [cluster_has_flag [cluster_get_node_by_id $j $primary_id] master] &&
            [dict get [cluster_get_node_by_id $j $primary_id] config_epoch] == $primary_epoch
        } else {
            fail "R $j didn't see R $primary take over from R $replica in time"
        }
    }
    # $primary never got a replica announcement from $replica.
    assert {[cluster_has_flag [cluster_get_node_by_id $primary $replica_id] master]}
    assert_equal master [get_cluster_role $replica]
}

# Returns a key that hashes to $slot.
proc key_in_slot {slot} {
    for {set i 0} {1} {incr i} {
        if {[R 0 CLUSTER KEYSLOT key$i] == $slot} { return key$i }
    }
}

# Write a key in the importing slot 609 of the empty shard primary R6.
proc write_imported_key {key} {
    wait_for_cluster_state ok
    R 6 ASKING
    assert_equal {OK} [R 6 SET $key imported]
    assert_equal 1 [R 6 DBSIZE]
}

# Wait until R<idx> has handled a PONG from the node received after $since.
proc wait_for_pong_after {idx node_id since} {
    wait_for_condition 300 100 {
        [dict get [cluster_get_node_by_id $idx $node_id] pong_recv] > $since
    } else {
        fail "R $idx got no PONG from $node_id"
    }
}

proc current_epoch {idx} {
    regexp {cluster_current_epoch:(\d+)} [R $idx CLUSTER INFO] -> epoch
    return $epoch
}

proc now_ms {idx} {
    lassign [R $idx TIME] sec usec
    return [expr {$sec * 1000 + $usec / 1000}]
}

# A stale primary R7 of the empty shard leaves for a shard of its own and gets
# the highest config epoch, while R6 and R3 hear nothing. The first packet from
# R7 that they handle carries both its new shard id and its newer epoch. R7 is
# not in R6's shard anymore, so R6 must stay its primary and keep its imported
# key, and R3 must keep slot 609 migrating to R6.
start_cluster 3 5 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no cluster-node-timeout 5000} } {

    set R0_id [R 0 CLUSTER MYID]
    create_empty_shard 6 7
    set R6_id [R 6 CLUSTER MYID]
    set R7_id [R 7 CLUSTER MYID]

    test "Empty-shard primary keeps its role and keys when a stale primary of its shard moves to another shard" {
        assert_equal {OK} [R 0 CLUSTER SETSLOT 609 MIGRATING $R6_id]
        assert_equal {OK} [R 6 CLUSTER SETSLOT 609 IMPORTING $R0_id]
        wait_for_slot_state 3 "\[609->-$R6_id\]"

        stale_primary_after_failback 6 7
        test_defer {
            foreach j {3 6 7} { R $j DEBUG DROP-CLUSTER-PACKET-FILTER -1 }
        }
        wait_for_slot_state 6 "\[609-<-$R0_id\]"
        wait_for_slot_state 3 "\[609->-$R6_id\]"
        set key [key_in_slot 609]
        write_imported_key $key

        # R6 and R3 miss that R7 leaves the shard and bumps its epoch.
        R 6 DEBUG DROP-CLUSTER-PACKET-FILTER -2
        R 3 DEBUG DROP-CLUSTER-PACKET-FILTER -2
        assert_equal {OK} [R 7 CLUSTER RESET]
        R 7 DEBUG DROP-CLUSTER-PACKET-FILTER -1
        set R7_shard [R 7 CLUSTER MYSHARDID]
        assert_equal {OK} [R 7 CLUSTER MEET [srv 0 host] [srv 0 port]]
        wait_for_condition 200 100 {
            [catch {cluster_get_node_by_id 7 $R0_id} n] == 0 && $n ne {}
        } else {
            fail "R7 never learned R0"
        }
        # The PONG of the handshake does not update the current epoch.
        set e6 [dict get [cluster_get_myself 6] config_epoch]
        wait_for_condition 100 100 {
            [current_epoch 7] >= $e6
        } else {
            fail "R7 never learned the current epoch $e6"
        }
        set reply [R 7 CLUSTER BUMPEPOCH]
        assert_match {BUMPED *} $reply
        set e7 [lindex $reply 1]
        assert {$e7 > [dict get [cluster_get_myself 6] config_epoch]}

        set since [now_ms 6]
        R 6 DEBUG DROP-CLUSTER-PACKET-FILTER -1
        R 3 DEBUG DROP-CLUSTER-PACKET-FILTER -1
        foreach j {6 3} {
            wait_for_condition 300 100 {
                [dict get [cluster_get_node_by_id $j $R7_id] config_epoch] == $e7 &&
                [node_shard_id $j $R7_id] eq $R7_shard
            } else {
                fail "R $j did not record R7 in its new shard at config epoch $e7"
            }
        }
        wait_for_pong_after 6 $R7_id $since
        assert_equal master [get_cluster_role 6]
        assert_equal 1 [R 6 DBSIZE]
        assert_equal "\[609->-$R6_id\]" [get_open_slots 3]
    }
}

# A stale primary R7 of the empty shard gets the highest config epoch without
# leaving the shard, here with CLUSTER BUMPEPOCH. An epoch without consensus
# says nothing about which primary has the newer data, so R6 must stay a
# primary and keep the key it imported after it took the shard back.
start_cluster 3 5 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no cluster-node-timeout 5000} } {

    set R0_id [R 0 CLUSTER MYID]
    create_empty_shard 6 7
    set R6_id [R 6 CLUSTER MYID]
    set R7_id [R 7 CLUSTER MYID]

    test "Empty-shard primary keeps its role and keys when a stale primary of its shard bumps its epoch" {
        assert_equal {OK} [R 0 CLUSTER SETSLOT 609 MIGRATING $R6_id]
        assert_equal {OK} [R 6 CLUSTER SETSLOT 609 IMPORTING $R0_id]

        stale_primary_after_failback 6 7
        test_defer { R 7 DEBUG DROP-CLUSTER-PACKET-FILTER -1 }
        wait_for_slot_state 6 "\[609-<-$R0_id\]"
        set key [key_in_slot 609]
        write_imported_key $key
        set e6 [dict get [cluster_get_myself 6] config_epoch]

        # R7 must not hear R6 before its epoch is above R6's.
        pause_process [srv -6 pid]
        set wait_error [catch {
            R 7 DEBUG DROP-CLUSTER-PACKET-FILTER -1
            wait_for_condition 100 100 {
                [current_epoch 7] >= $e6
            } else {
                fail "R7 never learned the current epoch $e6"
            }
            set reply [R 7 CLUSTER BUMPEPOCH]
            assert_match {BUMPED *} $reply
            set e7 [lindex $reply 1]
            assert {$e7 > $e6}
            assert_equal master [get_cluster_role 7]
        } wait_result wait_options]
        resume_process [srv -6 pid]
        if {$wait_error} {
            return -options $wait_options $wait_result
        }

        set since [now_ms 6]
        wait_for_condition 300 100 {
            [dict get [cluster_get_node_by_id 6 $R7_id] config_epoch] == $e7
        } else {
            fail "R6 did not record R7 at config epoch $e7"
        }
        wait_for_pong_after 6 $R7_id $since
        assert_equal master [get_cluster_role 6]
        assert_equal 1 [R 6 DBSIZE]
    }
}

# Raise R<idx>'s config epoch. CLUSTER BUMPEPOCH only bumps a node that is not
# already the unique max, so if R<idx> is, bump R0 first and wait until R<idx>
# has seen R0's new epoch.
proc bump_epoch_of {idx} {
    set reply [R $idx CLUSTER BUMPEPOCH]
    if {[string match {STILL *} $reply]} {
        set r0 [R 0 CLUSTER BUMPEPOCH]
        assert_match {BUMPED *} $r0
        set e0 [lindex $r0 1]
        set R0_id [R 0 CLUSTER MYID]
        wait_for_condition 100 100 {
            [dict get [cluster_get_node_by_id $idx $R0_id] config_epoch] == $e0
        } else {
            fail "R$idx never saw R0 at config epoch $e0"
        }
        set reply [R $idx CLUSTER BUMPEPOCH]
    }
    assert_match {BUMPED *} $reply
    return [lindex $reply 1]
}

# R1 imports slot 1000 from R<src>, which does not own it (R0 does). R<src>
# then bumps its epoch with its slots unchanged. R1 must keep the importing
# state, must not take slot 1000 from R0, and must not bump its own epoch.
proc assert_epoch_bump_keeps_stale_import {src} {
    set R0_id [R 0 CLUSTER MYID]
    set src_id [R $src CLUSTER MYID]
    assert_equal {OK} [R 1 CLUSTER SETSLOT 1000 IMPORTING $src_id]
    wait_for_slot_state 1 "\[1000-<-$src_id\]"

    set e1 [dict get [cluster_get_myself 1] config_epoch]
    # count_log_message takes a grep regex, not a glob.
    set cleared_pattern "Slot 1000 is no longer being imported from node $src_id"
    set cleared [count_log_message -1 $cleared_pattern]
    set e [bump_epoch_of $src]

    # R1 handles the slot update in the same packet that carries the new epoch.
    wait_for_condition 100 100 {
        [dict get [cluster_get_node_by_id 1 $src_id] config_epoch] == $e
    } else {
        fail "R1 never saw R$src at config epoch $e"
    }
    assert_equal "\[1000-<-$src_id\]" [get_open_slots 1]
    assert_equal $cleared [count_log_message -1 $cleared_pattern]
    # Taking the slot over would bump R1's own epoch without consensus.
    assert_equal $e1 [dict get [cluster_get_myself 1] config_epoch]
    set owner {}
    foreach range [R 1 CLUSTER SLOTS] {
        if {[lindex $range 0] <= 1000 && 1000 <= [lindex $range 1]} {
            set owner [lindex $range 2 2]
        }
    }
    assert_equal $R0_id $owner
    assert_equal {OK} [R 1 CLUSTER SETSLOT 1000 STABLE]
}

# An epoch rise from a primary, with its slots unchanged, must not finalize
# another node's importing state, whether the primary owns slots or not.
start_cluster 3 1 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no cluster-node-timeout 5000} } {
    test "Epoch bump of a slot-owning primary does not finalize a stale import" {
        assert_epoch_bump_keeps_stale_import 2
    }

    test "Epoch bump of a slot-less primary does not finalize a stale import" {
        # Turn R3 into a primary that owns no slots.
        assert_equal {OK} [R 3 CLUSTER RESET]
        assert_equal {OK} [R 3 CLUSTER MEET [srv 0 "host"] [srv 0 "port"]]
        set R3_id [R 3 CLUSTER MYID]
        wait_for_condition 100 100 {
            [catch {cluster_get_node_by_id 1 $R3_id} n] == 0 && [cluster_has_flag $n master]
        } else {
            fail "R1 never saw R3 as a primary"
        }
        assert_epoch_bump_keeps_stale_import 3
    }
}

start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no cluster-node-timeout 1000} } {

    set R0_id [R 0 CLUSTER MYID]
    set R1_id [R 1 CLUSTER MYID]
    set R2_id [R 2 CLUSTER MYID]
    set R3_id [R 3 CLUSTER MYID]
    set R4_id [R 4 CLUSTER MYID]
    set R5_id [R 5 CLUSTER MYID]

    test "Multiple slot migration states are replicated" {
        migrate_slot 0 1 13
        migrate_slot 0 1 7
        migrate_slot 0 1 17
        # Validate final states
        wait_for_slot_state 0 "\[7->-$R1_id\] \[13->-$R1_id\] \[17->-$R1_id\]"
        wait_for_slot_state 1 "\[7-<-$R0_id\] \[13-<-$R0_id\] \[17-<-$R0_id\]"
        wait_for_slot_state 3 "\[7->-$R1_id\] \[13->-$R1_id\] \[17->-$R1_id\]"
        wait_for_slot_state 4 "\[7-<-$R0_id\] \[13-<-$R0_id\] \[17-<-$R0_id\]"
    }

    test "New replica inherits multiple migrating slots" {
        # Reset R3 to turn it into an empty node
        assert_equal {OK} [R 3 CLUSTER RESET]
        # Add R3 back as a replica of R0
        assert_equal {OK} [R 3 CLUSTER MEET [srv 0 "host"] [srv 0 "port"]]
        wait_for_role 0 master
        assert_equal {OK} [R 3 CLUSTER REPLICATE $R0_id]
        wait_for_role 3 slave
        # Validate final states
        wait_for_slot_state 3 "\[7->-$R1_id\] \[13->-$R1_id\] \[17->-$R1_id\]"
    }

    test "Slot finalization succeeds on both primary and replicas" {
        assert_equal {OK} [R 1 CLUSTER SETSLOT 7 NODE $R1_id]
        wait_for_slot_state 1 "\[13-<-$R0_id\] \[17-<-$R0_id\]"
        wait_for_slot_state 4 "\[13-<-$R0_id\] \[17-<-$R0_id\]"
        assert_equal {OK} [R 1 CLUSTER SETSLOT 13 NODE $R1_id]
        wait_for_slot_state 1 "\[17-<-$R0_id\]"
        wait_for_slot_state 4 "\[17-<-$R0_id\]"
        assert_equal {OK} [R 1 CLUSTER SETSLOT 17 NODE $R1_id]
        wait_for_slot_state 1 ""
        wait_for_slot_state 4 ""
    }

}

start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no cluster-node-timeout 1000} } {

    set node_timeout [lindex [R 0 CONFIG GET cluster-node-timeout] 1]
    set R0_id [R 0 CLUSTER MYID]
    set R1_id [R 1 CLUSTER MYID]

    test "Slot is auto-claimed by target after source relinquishes ownership" {
        migrate_slot 0 1 609
        #Validate that R1 doesn't own slot 609
        catch {[R 1 get aga]} e
        assert_equal {MOVED} [lindex [split $e] 0]
        #Finalize the slot on the source first
        assert_equal {OK} [R 0 CLUSTER SETSLOT 609 NODE $R1_id]
        after $node_timeout
        #R1 should claim slot 609 since it is still importing slot 609
        #from R0 but R0 no longer owns this slot
        assert_equal {OK} [R 1 set aga foo]
    }
}

start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no cluster-node-timeout 1000} } {
    set R1_id [R 1 CLUSTER MYID]

    test "CLUSTER SETSLOT with invalid timeouts" {
        catch {R 0 CLUSTER SETSLOT 609 MIGRATING $R1_id TIMEOUT} e
        assert_equal $e "ERR Missing timeout value"

        catch {R 0 CLUSTER SETSLOT 609 MIGRATING $R1_id TIMEOUT -1} e
        assert_equal $e "ERR timeout is negative"

        catch {R 0 CLUSTER SETSLOT 609 MIGRATING $R1_id TIMEOUT 99999999999999999999} e
        assert_equal $e "ERR timeout is not an integer or out of range"

        catch {R 0 CLUSTER SETSLOT 609 MIGRATING $R1_id TIMEOUT abc} e
        assert_equal $e "ERR timeout is not an integer or out of range"

        catch {R 0 CLUSTER SETSLOT 609 TIMEOUT 100 MIGRATING $R1_id} e
        assert_equal $e "ERR Invalid CLUSTER SETSLOT action or number of arguments. Try CLUSTER HELP"
    }

    test "CLUSTER SETSLOT MIGRATING/IMPORTING should not be allowed to target self" {
        assert_error {ERR Target node is myself} {R 0 CLUSTER SETSLOT 609 MIGRATING [R 0 CLUSTER MYID]}
        assert_error {ERR Target node is myself} {R 1 CLUSTER SETSLOT 609 IMPORTING [R 1 CLUSTER MYID]}
    }

    test "CLUSTER SETSLOT inside MULTI does not block on replicas" {
        R 0 MULTI
        R 0 CLUSTER SETSLOT 609 MIGRATING $R1_id
        assert_equal {OK} [R 0 EXEC]
        wait_for_slot_state 3 "\[609->-$R1_id\]"
        R 0 CLUSTER SETSLOT 609 STABLE
    }
}

start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no cluster-node-timeout 1000} } {
    set R1_id [R 1 CLUSTER MYID]

    test "CLUSTER SETSLOT with an explicit timeout" {
        # Pause the replica to simulate a failure
        pause_process [srv -3 pid]

        # Setslot with an explicit 1ms timeout
        set start_time [clock milliseconds]
        catch {R 0 CLUSTER SETSLOT 609 MIGRATING $R1_id TIMEOUT 3000} e
        set end_time [clock milliseconds]
        set duration [expr {$end_time - $start_time}]

        # Assert that the execution time is greater than the default 2s timeout
        assert {$duration > 2000}

        # Setslot should fail with not enough good replicas to write after the timeout
        assert_equal {NOREPLICAS Not enough good replicas to write.} $e

        resume_process [srv -3 pid]
    }
}

start_cluster 2 0 {tags {tls:skip external:skip cluster regression} overrides {cluster-allow-replica-migration no cluster-node-timeout 1000} } {
    # Issue #563 regression test
    test "Client blocked on XREADGROUP while stream's slot is migrated" {
        set stream_name aga
        set slot 609

        # Start a deferring client to simulate a blocked client on XREADGROUP
        R 0 XGROUP CREATE $stream_name mygroup $ MKSTREAM
        set rd [valkey_deferring_client]
        $rd xreadgroup GROUP mygroup consumer BLOCK 0 streams $stream_name >
        wait_for_blocked_client

        # Migrate the slot to the target node
        R 0 CLUSTER SETSLOT $slot MIGRATING [dict get [cluster_get_myself 1] id]
        R 1 CLUSTER SETSLOT $slot IMPORTING [dict get [cluster_get_myself 0] id]

        # This line should cause the crash
        R 0 MIGRATE 127.0.0.1 [lindex [R 1 CONFIG GET port] 1] $stream_name 0 5000
    }
}

start_cluster 3 6 {tags {external:skip cluster} overrides {cluster-node-timeout 1000} } {
    test "Slot migration is ok when the replicas are down" {
        # Killing all replicas in primary 0.
        assert_equal 2 [s 0 connected_slaves]
        catch {R 3 shutdown nosave}
        catch {R 6 shutdown nosave}
        wait_for_condition 50 100 {
            [s 0 connected_slaves] == 0
        } else {
            fail "The replicas in primary 0 are still connecting"
        }

        # Killing one replica in primary 1.
        assert_equal 2 [s -1 connected_slaves]
        catch {R 4 shutdown nosave}
        wait_for_condition 50 100 {
            [s -1 connected_slaves] == 1
        } else {
            fail "The replica in primary 1 is still connecting"
        }

        # Check slot migration is ok when the replicas are down.
        migrate_slot 0 1 0
        migrate_slot 0 2 1
        assert_equal {OK} [R 0 CLUSTER SETSLOT 0 NODE [R 1 CLUSTER MYID]]
        assert_equal {OK} [R 0 CLUSTER SETSLOT 1 NODE [R 2 CLUSTER MYID]]
        wait_for_slot_state 0 ""
        wait_for_slot_state 1 ""
        wait_for_slot_state 2 ""
    }
}

start_cluster 3 3 {tags {external:skip cluster} } {
    test "Multi/Exec Validation During Slot Migration with Multiple Databases" {
        set primary_id_src 0
        set primary_id_src_nodeid [R $primary_id_src CLUSTER MYID]
        set primary_id_src_port [get_port $primary_id_src]
        set primary_id_target 1
        set primary_id_target_port [get_port $primary_id_target]
        set primary_id_target_nodeid [R $primary_id_target CLUSTER MYID]

        R $primary_id_src select 0
        R $primary_id_src set "{3560}key1" "value1_db0"
        assert_equal [R $primary_id_src get "{3560}key1"] "value1_db0"

        R $primary_id_src select 1
        R $primary_id_src set "{3560}key2" "value2_db1"
        assert_equal [R $primary_id_src get "{3560}key2"] "value2_db1"

        set slot0 [R $primary_id_src cluster keyslot "{3560}key1"]
        
        R $primary_id_src select 0
        
        R $primary_id_target cluster setslot $slot0 importing $primary_id_src_nodeid
        R $primary_id_src cluster setslot $slot0 migrating $primary_id_target_nodeid

        # Ensure correct database is selected before running MULTI/EXEC during migration
        R $primary_id_src select 0
        # Ensure key is still accessible before running MULTI/EXEC
        assert_equal [R $primary_id_src get "{3560}key1"] "value1_db0"
        R $primary_id_src multi
        R $primary_id_src exists "{3560}key1"
        set result [R $primary_id_src exec]
        assert_equal $result {1}

        # Multi/Exec on source before migration should pass
        R $primary_id_src multi
        R $primary_id_src exists "{3560}key1"
        R $primary_id_src select 1
        R $primary_id_src exists "{3560}key2"
        set result [R $primary_id_src exec]
        assert_equal $result {1 OK 1}

        # Multi/Exec on source before migration, bad select command would be allowed to fail
        R $primary_id_src select 0
        R $primary_id_src multi
        R $primary_id_src select 100
        R $primary_id_src exists "{3560}key1"
        R $primary_id_src select 1
        R $primary_id_src exists "{3560}key2"
        set result [catch {R $primary_id_src exec} err]
        assert_match "ERR DB index is out of range" $err

        # Multi/Exec on source before migration, mixed key existence
        R $primary_id_src select 0
        R $primary_id_src multi
        R $primary_id_src exists "{3560}key1"
        R $primary_id_src select 1
        R $primary_id_src exists "{3560}key2"
        R $primary_id_src select 2
        R $primary_id_src exists "{3560}no_key"
        assert_error "TRYAGAIN Multiple keys*" {R $primary_id_src exec}
        # Connection must still be on db 0 after aborted MULTI
        assert_equal [R $primary_id_src get "{3560}key1"] "value1_db0"

        # Multi/Exec on target before migration should fail at EXEC
        R $primary_id_target ASKING
        R $primary_id_target multi
        R $primary_id_target exists "{3560}key1"
        R $primary_id_src select 1
        R $primary_id_target exists "{3560}key2"
        set result [catch {R $primary_id_target exec} err]
        assert_match "TRYAGAIN Multiple keys request during rehashing of slot" $err

        # Multi/Exec on source - select invalid db num
        R $primary_id_src multi        
        R $primary_id_src select 100                
        set result [catch {R $primary_id_src exec} err]
        assert_match "ERR DB index is out of range" $err

        # Migrate keys        
        R $primary_id_src select 0
        R $primary_id_src MIGRATE 127.0.0.1 $primary_id_target_port "{3560}key1" 0 5000
        R $primary_id_src select 1
        R $primary_id_src MIGRATE 127.0.0.1 $primary_id_target_port "{3560}key2" 1 5000

        # Multi/exec accessing both keys should pass in the target
        R $primary_id_target select 0
        R $primary_id_target ASKING
        R $primary_id_target multi
        R $primary_id_target exists "{3560}key1"
        R $primary_id_target select 1
        R $primary_id_target exists "{3560}key2"        
        set result [R $primary_id_target exec]        
        assert_equal $result {1 OK 1}        
    }
}


start_cluster 3 3 {tags {external:skip cluster} } {
    test "MOVE should not be allowed while migrating slots with multi databases" {
        set primary_id_src 0
        set primary_id_src_nodeid [R $primary_id_src CLUSTER MYID]    
        set primary_id_target 1
        set primary_id_target_nodeid [R $primary_id_target CLUSTER MYID]

        R $primary_id_src select 0
        R $primary_id_src set "{3560}key1" "value1_db1"
        assert_equal [R $primary_id_src get "{3560}key1"] "value1_db1"
    
        set slot [R $primary_id_src cluster keyslot "{3560}key1"]

        R $primary_id_target cluster setslot $slot importing $primary_id_src_nodeid
        R $primary_id_src cluster setslot $slot migrating $primary_id_target_nodeid

        
        set result [catch {assert_error [R $primary_id_src MOVE "{3560}key1" 2]} err]
        assert_match "TRYAGAIN Multiple keys request during rehashing of slot" $err

        set result [catch {assert_error [R $primary_id_target MOVE "{3560}key1" 2]} err]
        assert_match "TRYAGAIN Multiple keys request during rehashing of slot" $err
    }

}


start_cluster 3 3 {tags {external:skip cluster} } {
    test "Cross-DB COPY command should not be allow during slot migration" {
        set primary_id_src 0
        set primary_id_src_nodeid [R $primary_id_src CLUSTER MYID]
        set primary_id_target 1
        set primary_id_target_nodeid [R $primary_id_target CLUSTER MYID]

        R $primary_id_src select 0
        R $primary_id_src set "{3560}key1" "value1_db1"
        R $primary_id_src set "{3560}key2" "value2_db1"


        set slot [R $primary_id_src cluster keyslot "{3560}key1"]

        R $primary_id_target cluster setslot $slot importing $primary_id_src_nodeid
        R $primary_id_src cluster setslot $slot migrating $primary_id_target_nodeid

        # Cross slot should still fail
        set result [catch {assert_error [R $primary_id_src COPY "{3560}key1" "{3561}key1"]} err]
        assert_match "CROSSSLOT Keys in request don't hash to the same slot" $err

        # Same DB, source key exist, dest key doesn't exist, should still fail
        set result [catch {assert_error [R $primary_id_src COPY "{3560}key1" "{3560}key3"]} err]
        assert_match "TRYAGAIN Multiple keys request during rehashing of slot" $err

        # Cross-DB COPY should NOT be allowed
        set result [catch {assert_error [R $primary_id_src COPY "{3560}key1" "{3560}key1" DB 7 REPLACE]} err]
        assert_match "TRYAGAIN Multiple keys request during rehashing of slot" $err

        # A DB token with no value is a syntax error, not an out-of-range read.
        assert_error "ERR syntax error" {R $primary_id_src COPY "{3560}key1" "{3560}key2" DB}
        assert_error "ERR syntax error" {R $primary_id_src COPY "{3560}key1" "{3560}key2" REPLACE DB}
        assert_equal {PONG} [R $primary_id_src PING]

        # DB and REPLACE are order-independent, so a cross-DB COPY must be blocked
        # no matter where the DB clause appears.
        assert_error "TRYAGAIN*" {R $primary_id_src COPY "{3560}key1" "{3560}key2" REPLACE DB 7}
        assert_error "TRYAGAIN*" {R $primary_id_src COPY "{3560}key1" "{3560}key2" DB 0 DB 7}

        # Both keys exist, should work, but both keys must exist.
        R $primary_id_src COPY "{3560}key1" "{3560}key2"
        # And it should work if DB param is provided, as long as it matches the selected DB
        R $primary_id_src COPY "{3560}key1" "{3560}key2" DB 0 REPLACE
        R $primary_id_src COPY "{3560}key1" "{3560}key2" REPLACE DB 0

    }

}



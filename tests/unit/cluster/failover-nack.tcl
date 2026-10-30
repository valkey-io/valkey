# FAILOVER_AUTH_NACK handling on a candidate replica, driven by crafted NACK
# packets injected on the cluster bus (see #4627 and #4626).
#
# The election is frozen while it waits for votes: the voters drop
# FAILOVER_AUTH_REQUEST, so the candidate keeps failover_auth_sent=1 for the
# whole auth_timeout window and every NACK it sees is one we injected. The
# candidate accepts a bus packet from any known node id, so the NACKs are sent
# from a plain TCP socket claiming to be one of the voters. Doing so replaces
# the candidate's inbound link from that voter, so the voter's own outbound link
# drops and is re-established by its cron; a voter with no link silently skips
# sending its ACK, so a test that expects real votes afterwards must wait for
# the links to come back first.

# Build the fixed clusterMsg header of a bus packet of the given type, sent by
# a primary. The layout matches packet.tcl. totlen is left 0 and patched by
# finish_cluster_bus_packet once the payload is appended.
proc create_cluster_bus_header {type sender_name sender_port sender_cport header_epoch} {
    set CLUSTER_NAMELEN 40
    set CLUSTER_SLOTS 16384
    set NET_IP_STR_LEN 46
    set CLUSTER_NODE_PRIMARY 1
    set CLUSTER_NODE_FAILOVER_AUTH_NACK_SUPPORTED [expr {1 << 14}]

    set packet ""
    append packet "RCmb"
    append packet [binary format I 0]                      ;# totlen, patched below
    append packet [binary format S 1]                      ;# ver
    append packet [binary format S $sender_port]           ;# port
    append packet [binary format S $type]
    append packet [binary format S 0]                      ;# count
    append packet [binary format W $header_epoch]          ;# currentEpoch (the voter's)
    append packet [binary format W 0]                      ;# configEpoch: 0 so the receiver never sees a newer claim
    append packet [binary format W 0]                      ;# offset
    append packet [string range "${sender_name}[string repeat "\x00" $CLUSTER_NAMELEN]" 0 [expr {$CLUSTER_NAMELEN - 1}]]
    append packet [string repeat "\x00" [expr {$CLUSTER_SLOTS / 8}]] ;# myslots
    append packet [string repeat "\x00" $CLUSTER_NAMELEN]  ;# replicaof: all zeros = sender is a primary
    append packet [string repeat "\x00" $NET_IP_STR_LEN]   ;# myip
    append packet [binary format S 0]                      ;# extensions
    append packet [string repeat "\x00" 30]                ;# notused1
    append packet [binary format S 0]                      ;# pport
    append packet [binary format S $sender_cport]          ;# cport
    append packet [binary format S [expr {$CLUSTER_NODE_PRIMARY | $CLUSTER_NODE_FAILOVER_AUTH_NACK_SUPPORTED}]]
    append packet [binary format c 0]                      ;# state
    append packet [binary format ccc 0 0 0]                ;# mflags
    return $packet
}

proc finish_cluster_bus_packet {packet} {
    set totlen [string length $packet]
    return [string replace $packet 4 7 [binary format I $totlen]]
}

# Build a FAILOVER_AUTH_NACK packet. The clusterMsgDataFailoverNack payload
# follows the header:
#   uint64_t epoch      - currentEpoch of the AUTH_REQUEST being rejected
#   uint8_t  reason     - CLUSTERMSG_FAILOVER_AUTH_NACK_REASON_*
#   uint8_t  reserved[7]
proc create_cluster_failover_nack_packet {sender_name sender_port sender_cport header_epoch nack_epoch reason} {
    set CLUSTERMSG_TYPE_FAILOVER_AUTH_NACK 11
    set packet [create_cluster_bus_header $CLUSTERMSG_TYPE_FAILOVER_AUTH_NACK $sender_name $sender_port $sender_cport $header_epoch]
    append packet [binary format W $nack_epoch]
    append packet [binary format c $reason]
    append packet [string repeat "\x00" 7]
    return [finish_cluster_bus_packet $packet]
}

# Build a FAIL packet announcing that failing_name is down. The receiver marks
# that node FAIL in its own view immediately; it does not re-broadcast it.
proc create_cluster_fail_packet {sender_name sender_port sender_cport header_epoch failing_name} {
    set CLUSTER_NAMELEN 40
    set CLUSTERMSG_TYPE_FAIL 3
    set packet [create_cluster_bus_header $CLUSTERMSG_TYPE_FAIL $sender_name $sender_port $sender_cport $header_epoch]
    append packet [string range "${failing_name}[string repeat "\x00" $CLUSTER_NAMELEN]" 0 [expr {$CLUSTER_NAMELEN - 1}]]
    return [finish_cluster_bus_packet $packet]
}

proc send_cluster_bus_packet {cport packet} {
    set sock [socket 127.0.0.1 $cport]
    fconfigure $sock -translation binary -buffering none -blocking 1
    puts -nonewline $sock $packet
    flush $sock
    close $sock
}

# 1 if instance id has an inbound cluster link from every node in peer_ids.
proc has_inbound_links_from {id peer_ids} {
    set from {}
    foreach l [R $id cluster links] {
        if {[dict get $l direction] eq "from"} {
            lappend from [dict get $l node]
        }
    }
    foreach peer $peer_ids {
        if {[lsearch -exact $from $peer] == -1} {return 0}
    }
    return 1
}

# Freeze the candidate's election so that the only NACKs it sees are the ones
# we inject: the two voters drop FAILOVER_AUTH_REQUEST, the primary is paused,
# and the candidate has broadcast a request that nobody answers. Returns
# {election_epoch election_line} describing the election left open.
#
# Must run in the body of
# start_cluster 3 1 {tags {external:skip cluster tls:skip}}
proc freeze_election {reason_name} {
    set CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST 5

    set primary_id [R 0 cluster myid]

    test "Freeze the election ($reason_name): voters drop FAILOVER_AUTH_REQUEST, primary is paused" {
        R 1 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST
        R 2 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST
        pause_process_persistent [srv 0 pid]

        wait_for_condition 1000 50 {
            [cluster_all_see_flag {1 2 3} [list $primary_id] fail]
        } else {
            fail "Voters and replica did not mark the paused primary as fail"
        }
    }

    set election_epoch 0
    set election_line 0
    test "Replica starts an election and waits for votes ($reason_name)" {
        set res [wait_for_log_messages -3 {"*Starting a failover election for epoch *"} 0 1000 50]
        set election_line [lindex $res 1]
        assert {[regexp {Starting a failover election for epoch (\d+)} [lindex $res 0] -> election_epoch]}
        assert_morethan $election_epoch 1
        # No voter answers, so the election is still open and un-NACKed.
        verify_no_log_message -3 "*Failover auth NACK*" $election_line
    }

    return [list $election_epoch $election_line]
}

# Drive one frozen election on the candidate replica, inject a single NACK
# carrying $reason_code for the current election epoch, and assert the candidate
# counted it and fast-failed the election instead of waiting out auth_timeout.
# Every reason accepted by clusterNackIsPerRoundReason() must behave this way,
# so this runs once per reason, each in its own cluster below.
#
# Must run in the body of
# start_cluster 3 1 {tags {external:skip cluster tls:skip}}
proc test_nack_fast_fail_reason {reason_name reason_code} {
    set voter_id [R 1 cluster myid]
    set voter_port [srv -1 port]
    set voter_cport [expr {[srv -1 port] + 10000}]
    set candidate_cport [expr {[srv -3 port] + 10000}]

    lassign [freeze_election $reason_name] election_epoch election_line

    test "NACK for an older election is ignored by the current election ($reason_name)" {
        # A voter at epoch E rejecting the candidate's stale epoch E-1 request:
        # its header claims E (which is what the old receive gate compared), but
        # the payload says the rejected request was E-1. The reason is irrelevant
        # here: the receive gate compares the echoed epoch first, so the packet
        # is dropped before any reason is looked at.
        set stale_nack [create_cluster_failover_nack_packet $voter_id $voter_port $voter_cport \
            $election_epoch [expr {$election_epoch - 1}] $reason_code]
        send_cluster_bus_packet $candidate_cport $stale_nack

        # Wait until the candidate has read the packet, then check it was not
        # counted: no NACK accounting line, no reset, still a replica.
        wait_for_condition 1000 10 {
            [CI 3 cluster_stats_messages_auth-nack_received] eq 1
        } else {
            fail "Stale NACK never reached the candidate"
        }
        verify_no_log_message -3 "*Failover auth NACK*" $election_line
        verify_no_log_message -3 "*cannot reach quorum*" $election_line
        assert_equal "slave" [s -3 role]
    }

    test "NACK ($reason_name) for the current election is counted and fast-fails it" {
        set nack [create_cluster_failover_nack_packet $voter_id $voter_port $voter_cport \
            $election_epoch $election_epoch $reason_code]
        send_cluster_bus_packet $candidate_cport $nack

        # 3 primaries, one FAIL, quorum 2: a single NACK leaves at most one
        # achievable ACK, so the election is reset immediately.
        wait_for_log_messages -3 [list "*Failover auth NACK *$reason_name* from $voter_id * for epoch $election_epoch*"] $election_line 1000 50
        wait_for_log_messages -3 [list "*Failover election for epoch $election_epoch cannot reach quorum*"] $election_line 1000 50
        # The reason must have been counted, not skipped as one a new epoch
        # cannot fix, and the election must not have waited out auth_timeout.
        verify_no_log_message -3 "*Ignoring failover auth NACK*" $election_line
        verify_no_log_message -3 "*Failover attempt expired*" $election_line
    }

    test "Replica wins once the voters answer again ($reason_name)" {
        R 1 DEBUG DROP-CLUSTER-PACKET-FILTER -1
        R 2 DEBUG DROP-CLUSTER-PACKET-FILTER -1
        wait_for_condition 1000 50 {
            [s -3 role] eq "master"
        } else {
            fail "Replica did not win the election"
        }
        resume_process [srv 0 pid]
        wait_for_cluster_state ok
    }
}

# Mirror of test_nack_fast_fail_reason() for the reasons a new epoch cannot fix
# (see clusterNackIsPerRoundReason()): the candidate must log the NACK and drop
# it instead of counting it, so the election is left to expire on the
# auth_retry_time cadence. A reason the voter would answer the same way next
# round needs no real voter state to produce, so all of them share one frozen
# election: an ignored NACK leaves the round untouched, so the next reason can
# be injected into it as well.
#
# Must run in the body of
# start_cluster 3 1 {tags {external:skip cluster tls:skip}}
proc test_nack_ignored_reasons {reason_list} {
    set voter_id [R 1 cluster myid]
    set voter_port [srv -1 port]
    set voter_cport [expr {[srv -1 port] + 10000}]
    set candidate_cport [expr {[srv -3 port] + 10000}]

    lassign [freeze_election "ignored reasons"] election_epoch election_line

    foreach reason $reason_list {
        lassign $reason reason_name reason_code
        test "NACK ($reason_name) for the current election is ignored, not counted" {
            set nack [create_cluster_failover_nack_packet $voter_id $voter_port $voter_cport \
                $election_epoch $election_epoch $reason_code]
            send_cluster_bus_packet $candidate_cport $nack

            # Not counted: no accounting line for it, no reset, still a replica waiting for votes.
            wait_for_log_messages -3 [list "*Ignoring failover auth NACK *$reason_name* from $voter_id * for epoch $election_epoch*"] $election_line 1000 10
            verify_no_log_message -3 "*Failover auth NACK *$reason_name* from $voter_id * for epoch*" $election_line
            verify_no_log_message -3 "*cannot reach quorum*" $election_line
            assert_equal "slave" [s -3 role]
        }
    }
}

start_cluster 3 1 {tags {external:skip cluster tls:skip}} {
    set NACK_REASON_REQ_EPOCH_OLD 2
    test_nack_fast_fail_reason "req-epoch-old" $NACK_REASON_REQ_EPOCH_OLD
}

start_cluster 3 1 {tags {external:skip cluster tls:skip}} {
    set NACK_REASON_ALREADY_VOTED 3
    test_nack_fast_fail_reason "already-voted" $NACK_REASON_ALREADY_VOTED
}

start_cluster 3 1 {tags {external:skip cluster tls:skip}} {
    # stale-config is also covered end to end, by test_replica_config_epoch_failover
    # failover2.tcl; this is the injected-packet variant of the same reason.
    set NACK_REASON_STALE_CONFIG 7
    test_nack_fast_fail_reason "stale-config" $NACK_REASON_STALE_CONFIG
}

# The same injected-packet check for the reasons a new epoch cannot fix, i.e.
# every reason clusterNackIsPerRoundReason() rejects. No real voter state is
# needed to produce them, so they share one frozen election. NO_PRIMARY among
# them is also covered end to end by the block below.
start_cluster 3 1 {tags {external:skip cluster tls:skip}} {
    test_nack_ignored_reasons {
        {"not-safe" 1}
        {"req-is-primary" 4}
        {"no-primary" 5}
        {"primary-up" 6}
    }
}

# NACK fast-fail must ignore rejections that a new epoch cannot fix.
#
# Deployment: 3 primaries + 1 replica, R3 is the sole replica of R0.
#
# The two voters CLUSTER FORGET the failed primary, so they answer every
# request with NO_PRIMARY ("I don't know its primary") while the candidate
# still considers itself that primary's replica. A new epoch cannot change
# that answer, so those NACKs must not be counted: the candidate stays on the
# auth_retry_time cadence instead of resetting the election once per NACK,
# which would retry at event-loop frequency.
start_cluster 3 1 {tags {external:skip cluster} overrides {cluster-node-timeout 2000 cluster-ping-interval 100 cluster-replica-validity-factor 0}} {
    test "NACK fast-fail ignores rejections that a new epoch cannot fix" {
        set primary0_id [R 0 CLUSTER MYID]
        set replica_id [R 3 CLUSTER MYID]
        set voter1_id [R 1 CLUSTER MYID]
        set voter2_id [R 2 CLUSTER MYID]

        # Hold the candidate back until the voters are set up to reject it.
        R 3 CONFIG SET cluster-replica-no-failover yes

        pause_process [srv 0 pid]
        wait_for_condition 1000 50 {
            [cluster_all_see_flag {1 2 3} [list $primary0_id] fail]
        } else {
            fail "not every surviving node marked primary 0 as FAIL"
        }

        # The voters forget the primary, the candidate keeps it, so every
        # request is answered with the same NO_PRIMARY rejection.
        R 1 CLUSTER FORGET $primary0_id
        R 2 CLUSTER FORGET $primary0_id
        wait_for_condition 1000 50 {
            [dict get [cluster_get_node_by_id 1 $replica_id] slaveof] eq "-" &&
            [dict get [cluster_get_node_by_id 2 $replica_id] slaveof] eq "-"
        } else {
            fail "voters still associate the candidate with a primary"
        }

        # cluster-node-timeout 2000 gives auth_timeout = 4000 ms and
        # auth_retry_time = 8000 ms, i.e. at most one attempt per 8s.
        # Ensure that we time out rather than fast fail.
        R 3 CONFIG SET cluster-replica-no-failover no

        # The voters really answer NO_PRIMARY and the candidate logs each
        # rejection as ignored rather than counting it.
        wait_for_log_messages -3 [list "*Ignoring failover auth NACK *no-primary* from $voter1_id *"] 0 2000 10
        wait_for_log_messages -3 [list "*Ignoring failover auth NACK *no-primary* from $voter2_id *"] 0 2000 10
        wait_for_log_messages -3 {"*Failover attempt expired*"} 0 2000 10

        # None of them was counted: no accounting line, hence no reset. That is
        # what keeps the election open until it expires instead of being reset.
        verify_no_log_message -3 "*Failover auth NACK *no-primary* for epoch*" 0

        resume_process [srv 0 pid]
    }
} ;# start_cluster

# A voter that NACKs and is then marked FAIL in the same election must be
# excluded from the achievable votes once, not twice (see #4626). Five voters,
# quorum 3: after V1 NACKs and fails and V2 NACKs, V3, V4 and V5 can still
# vote, so the election is still winnable and must not be reset. A bound that
# subtracts every FAIL voter and every NACK from size counts V1 twice and
# resets it. The FAIL is delivered as a crafted FAIL packet, which the
# candidate applies to its own view immediately without re-broadcasting it.
start_cluster 5 1 {tags {external:skip cluster tls:skip}} {
    set CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST 5
    set NACK_REASON_ALREADY_VOTED 3

    set primary_id [R 0 cluster myid]
    set candidate_cport [expr {[srv -5 port] + 10000}]
    set candidate_epoch 0

    # V1..V3 are primaries other than the candidate's own, so marking V1 FAIL
    # on the candidate does not touch its primary link.
    foreach v {1 2 3} {
        set v${v}_id [R $v cluster myid]
        set v${v}_port [srv -$v port]
        set v${v}_cport [expr {[srv -$v port] + 10000}]
    }

    test "Freeze the election: five voters drop FAILOVER_AUTH_REQUEST" {
        assert_equal $primary_id [dict get [cluster_get_myself 5] slaveof]
        assert_equal 5 [CI 5 cluster_size]
        foreach v {0 1 2 3 4} {
            R $v DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST
        }
    }

    set election_epoch 0
    set election_line 0
    test "Replica starts a manual election and waits for votes" {
        R 5 CLUSTER FAILOVER FORCE
        set res [wait_for_log_messages -5 {"*Starting a failover election for epoch *"} 0 1000 50]
        set election_line [lindex $res 1]
        assert {[regexp {Starting a failover election for epoch (\d+)} [lindex $res 0] -> election_epoch]}
        set candidate_epoch [CI 5 cluster_current_epoch]
        assert_equal $election_epoch $candidate_epoch
        verify_no_log_message -5 "*Failover auth NACK*" $election_line
    }

    test "First NACK is counted, election stays open" {
        send_cluster_bus_packet $candidate_cport [create_cluster_failover_nack_packet \
            $v1_id $v1_port $v1_cport $candidate_epoch $election_epoch $NACK_REASON_ALREADY_VOTED]
        wait_for_log_messages -5 [list "*Failover auth NACK * from $v1_id * for epoch $election_epoch*"] $election_line 1000 50
        # The bound is checked in clusterBeforeSleep, before replies are
        # written, so once INFO answers any reset is in the log.
        assert_equal "slave" [s -5 role]
        verify_no_log_message -5 "*cannot reach quorum*" $election_line
    }

    test "The NACKed voter is marked FAIL on the candidate" {
        send_cluster_bus_packet $candidate_cport [create_cluster_fail_packet \
            $v2_id $v2_port $v2_cport $candidate_epoch $v1_id]
        wait_for_condition 1000 10 {
            [cluster_all_see_flag {5} [list $v1_id] fail]
        } else {
            fail "Candidate did not mark the NACKed voter as fail"
        }
        assert_equal "slave" [s -5 role]
    }

    test "Second NACK does not reset an election that is still winnable" {
        send_cluster_bus_packet $candidate_cport [create_cluster_failover_nack_packet \
            $v2_id $v2_port $v2_cport $candidate_epoch $election_epoch $NACK_REASON_ALREADY_VOTED]
        # The bound is checked in clusterBeforeSleep, before replies are
        # written, so once INFO answers any reset is in the log.
        wait_for_log_messages -5 [list "*Failover auth NACK * from $v2_id * for epoch $election_epoch*"] $election_line 1000 50
        assert_equal "slave" [s -5 role]
        verify_no_log_message -5 "*cannot reach quorum*" $election_line
    }

    test "Third NACK leaves fewer possible votes than quorum and resets the election" {
        # Let the voters answer again first, so the retry started by the reset
        # is answered and the replica gets promoted. The injected packets cost
        # V1, V2 and V3 their link to the candidate; wait for those to be back
        # or their votes for the retry are dropped.
        foreach v {0 1 2 3 4} {
            R $v DEBUG DROP-CLUSTER-PACKET-FILTER -1
        }
        set voter_ids {}
        foreach v {0 1 2 3 4} {
            lappend voter_ids [R $v cluster myid]
        }
        wait_for_condition 1000 50 {
            [has_inbound_links_from 5 $voter_ids]
        } else {
            fail "Voters did not re-establish their links to the candidate"
        }
        send_cluster_bus_packet $candidate_cport [create_cluster_failover_nack_packet \
            $v3_id $v3_port $v3_cport $candidate_epoch $election_epoch $NACK_REASON_ALREADY_VOTED]
        wait_for_log_messages -5 [list "*Failover auth NACK * from $v3_id * for epoch $election_epoch*"] $election_line 1000 50
        wait_for_log_messages -5 [list "*Failover election for epoch $election_epoch cannot reach quorum*"] $election_line 1000 50
    }

    test "Replica wins the retried election once the voters answer" {
        wait_for_condition 1000 50 {
            [s -5 role] eq "master"
        } else {
            fail "Replica did not win the retried election"
        }
        wait_for_cluster_state ok
    }
}

# A voter that is already FAIL when the election starts will never respond to
# it, so it must count as a dead voter from the first NACK on (see #4626). Five
# voters, quorum 3, V1 FAIL before the election: after V2 and V3 NACK, only V4
# and V5 can still vote, so the election is reset at the second NACK with one
# dead voter. Resetting the per-voter state to CAN_RESPOND for every node at
# election start loses V1 and would need a third NACK. The retried election
# is then answered only by R0, V1 and R4, so it can reach quorum only if the
# ACK from V1, still FAIL in the candidate's view, is counted as a vote.
start_cluster 5 1 {tags {external:skip cluster tls:skip}} {
    set CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST 5
    set NACK_REASON_ALREADY_VOTED 3

    set primary_id [R 0 cluster myid]
    set candidate_cport [expr {[srv -5 port] + 10000}]
    set candidate_epoch 0

    foreach v {1 2 3} {
        set v${v}_id [R $v cluster myid]
        set v${v}_port [srv -$v port]
        set v${v}_cport [expr {[srv -$v port] + 10000}]
    }

    test "Freeze the election and mark one voter FAIL before it starts" {
        assert_equal $primary_id [dict get [cluster_get_myself 5] slaveof]
        assert_equal 5 [CI 5 cluster_size]
        foreach v {0 1 2 3 4} {
            R $v DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST
        }
        send_cluster_bus_packet $candidate_cport [create_cluster_fail_packet \
            $v2_id $v2_port $v2_cport [CI 5 cluster_current_epoch] $v1_id]
        wait_for_condition 1000 10 {
            [cluster_all_see_flag {5} [list $v1_id] fail]
        } else {
            fail "Candidate did not mark the voter as fail"
        }
    }

    set election_epoch 0
    set election_line 0
    test "Replica starts a manual election with one voter already FAIL" {
        R 5 CLUSTER FAILOVER FORCE
        set res [wait_for_log_messages -5 {"*Starting a failover election for epoch *"} 0 1000 50]
        set election_line [lindex $res 1]
        assert {[regexp {Starting a failover election for epoch (\d+)} [lindex $res 0] -> election_epoch]}
        set candidate_epoch [CI 5 cluster_current_epoch]
        assert_equal $election_epoch $candidate_epoch
        assert {[cluster_all_see_flag {5} [list $v1_id] fail]}
    }

    test "First NACK is counted, election stays open" {
        send_cluster_bus_packet $candidate_cport [create_cluster_failover_nack_packet \
            $v2_id $v2_port $v2_cport $candidate_epoch $election_epoch $NACK_REASON_ALREADY_VOTED]
        wait_for_log_messages -5 [list "*Failover auth NACK * from $v2_id * for epoch $election_epoch*"] $election_line 1000 50
        # The bound is checked in clusterBeforeSleep, before replies are
        # written, so once INFO answers any reset is in the log.
        assert_equal "slave" [s -5 role]
        verify_no_log_message -5 "*cannot reach quorum*" $election_line
    }

    test "Second NACK resets the election because the FAIL voter cannot respond" {
        # Let R0, V1 and R4 answer the retry started by the reset. V2 and V3
        # keep dropping the request, so the retry can only reach quorum 3 if
        # the ACK from V1, which the candidate still considers FAIL, is
        # counted. The injected packets cost V2 and V3 their link to the
        # candidate, which does not matter since they stay silent.
        foreach v {0 1 4} {
            R $v DEBUG DROP-CLUSTER-PACKET-FILTER -1
        }
        send_cluster_bus_packet $candidate_cport [create_cluster_failover_nack_packet \
            $v3_id $v3_port $v3_cport $candidate_epoch $election_epoch $NACK_REASON_ALREADY_VOTED]
        wait_for_log_messages -5 [list "*Failover auth NACK * from $v3_id * for epoch $election_epoch*"] $election_line 1000 50
        wait_for_log_messages -5 [list "*Failover election for epoch $election_epoch cannot reach quorum 3 (ACKs 0, NACKs 2, dead voters 1)*"] $election_line 1000 50
    }

    test "Replica wins the retried election with the FAIL voter's ACK" {
        wait_for_condition 1000 50 {
            [s -5 role] eq "master"
        } else {
            fail "Replica did not win the retried election"
        }
        foreach v {2 3} {
            R $v DEBUG DROP-CLUSTER-PACKET-FILTER -1
        }
        wait_for_cluster_state ok
    }
}

# Voters marked FAIL alone must never reset the election, only a NACK can. A
# retry starts with the same FAIL voters, so it would be reset again as soon
# as its request goes out, and with CLUSTER FAILOVER that repeats on every
# event loop iteration. Five voters, quorum 3: V1, V2 and V3 are marked FAIL
# on the candidate only, so with no NACK at most two ACKs look achievable.
# They are up and vote, so the election must stay open and be won in the
# epoch it started in.
start_cluster 5 1 {tags {external:skip cluster tls:skip}} {
    set primary_id [R 0 cluster myid]
    set candidate_cport [expr {[srv -5 port] + 10000}]
    set r4_id [R 4 cluster myid]
    set r4_port [srv -4 port]
    set r4_cport [expr {[srv -4 port] + 10000}]

    set voter_ids {}
    foreach v {0 1 2 3 4} {
        lappend voter_ids [R $v cluster myid]
    }
    set failed_ids [lrange $voter_ids 1 3]

    test "A majority of voters is marked FAIL on the candidate" {
        assert_equal $primary_id [dict get [cluster_get_myself 5] slaveof]
        assert_equal 5 [CI 5 cluster_size]
        foreach id $failed_ids {
            send_cluster_bus_packet $candidate_cport [create_cluster_fail_packet \
                $r4_id $r4_port $r4_cport [CI 5 cluster_current_epoch] $id]
        }
        wait_for_condition 1000 10 {
            [cluster_all_see_flag {5} $failed_ids fail]
        } else {
            fail "Candidate did not mark the voters as fail"
        }
        # The injected packets cost R4 its link to the candidate; wait for it
        # to be back so that every voter can answer.
        wait_for_condition 1000 50 {
            [has_inbound_links_from 5 $voter_ids]
        } else {
            fail "Voters did not re-establish their links to the candidate"
        }
    }

    test "Election with a majority of FAIL voters and no NACK is not reset" {
        set from_line [count_log_lines -5]
        R 5 CLUSTER FAILOVER FORCE
        set res [wait_for_log_messages -5 {"*Starting a failover election for epoch *"} $from_line 1000 50]
        set election_line [lindex $res 1]
        assert {[cluster_all_see_flag {5} $failed_ids fail]}
        wait_for_condition 1000 50 {
            [s -5 role] eq "master"
        } else {
            fail "Replica did not win the election"
        }
        verify_no_log_message -5 "*cannot reach quorum*" $election_line
        verify_no_log_message -5 "*Starting a failover election*" $election_line
        wait_for_cluster_state ok
    }
}

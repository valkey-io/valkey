# FAILOVER_AUTH_NACK handling on a candidate replica, driven by crafted NACK
# packets injected on the cluster bus (see #4627).
#
# The election is frozen while it waits for votes: the two voters drop
# FAILOVER_AUTH_REQUEST, so the candidate keeps failover_auth_sent=1 for the
# whole auth_timeout window and every NACK it sees is one we injected. The
# candidate accepts a bus packet from any known node id, so the NACKs are sent
# from a plain TCP socket claiming to be one of the voters.

# Build a FAILOVER_AUTH_NACK packet. The fixed clusterMsg header matches the
# layout in packet.tcl; the clusterMsgDataFailoverNack payload follows it:
#   uint64_t epoch      - currentEpoch of the AUTH_REQUEST being rejected
#   uint8_t  reason     - CLUSTERMSG_FAILOVER_AUTH_NACK_REASON_*
#   uint8_t  reserved[7]
proc create_cluster_failover_nack_packet {sender_name sender_port sender_cport header_epoch nack_epoch reason} {
    set CLUSTER_NAMELEN 40
    set CLUSTER_SLOTS 16384
    set NET_IP_STR_LEN 46
    set CLUSTERMSG_TYPE_FAILOVER_AUTH_NACK 11
    set CLUSTER_NODE_PRIMARY 1
    set CLUSTER_NODE_FAILOVER_AUTH_NACK_SUPPORTED [expr {1 << 14}]

    set packet ""
    append packet "RCmb"
    append packet [binary format I 0]                      ;# totlen, patched below
    append packet [binary format S 1]                      ;# ver
    append packet [binary format S $sender_port]           ;# port
    append packet [binary format S $CLUSTERMSG_TYPE_FAILOVER_AUTH_NACK]
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

    # clusterMsgDataFailoverNack
    append packet [binary format W $nack_epoch]
    append packet [binary format c $reason]
    append packet [string repeat "\x00" 7]

    set totlen [string length $packet]
    set packet [string replace $packet 4 7 [binary format I $totlen]]
    return $packet
}

proc send_cluster_bus_packet {cport packet} {
    set sock [socket 127.0.0.1 $cport]
    fconfigure $sock -translation binary -buffering none -blocking 1
    puts -nonewline $sock $packet
    flush $sock
    close $sock
}

start_cluster 3 1 {tags {external:skip cluster tls:skip}} {
    set CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST 5
    set NACK_REASON_REQ_EPOCH_OLD 2

    set primary_id [R 0 cluster myid]
    set voter_id [R 1 cluster myid]
    set voter_port [srv -1 port]
    set voter_cport [expr {[srv -1 port] + 10000}]
    set candidate_cport [expr {[srv -3 port] + 10000}]

    test "Cluster is up" {
        wait_for_cluster_state ok
    }

    test "Freeze the election: voters drop FAILOVER_AUTH_REQUEST, primary is paused" {
        R 1 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST
        R 2 DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST
        pause_process [srv 0 pid]

        wait_for_condition 1000 50 {
            [cluster_all_see_flag {1 2 3} [list $primary_id] fail]
        } else {
            fail "Voters and replica did not mark the paused primary as fail"
        }
    }

    set election_epoch 0
    set election_line 0
    test "Replica starts an election and waits for votes" {
        set res [wait_for_log_messages -3 {"*Starting a failover election for epoch *"} 0 1000 50]
        set election_line [lindex $res 1]
        assert {[regexp {Starting a failover election for epoch (\d+)} [lindex $res 0] -> election_epoch]}
        assert_morethan $election_epoch 1
        # No voter answers, so the election is still open and un-NACKed.
        verify_no_log_message -3 "*Failover auth NACK*" $election_line
    }

    test "NACK for an older election is ignored by the current election" {
        # A voter at epoch E rejecting the candidate's stale epoch E-1 request:
        # its header claims E (which is what the old receive gate compared), but
        # the payload says the rejected request was E-1.
        set stale_nack [create_cluster_failover_nack_packet $voter_id $voter_port $voter_cport \
            $election_epoch [expr {$election_epoch - 1}] $NACK_REASON_REQ_EPOCH_OLD]
        send_cluster_bus_packet $candidate_cport $stale_nack

        wait_for_log_messages -3 [list "*Ignoring failover auth NACK * for epoch [expr {$election_epoch - 1}]: current election is for epoch $election_epoch*"] $election_line 1000 50

        # Not counted, and the election was not reset.
        # (The counted-NACK line starts with a capital F; the verbose "Ignoring
        # failover auth NACK" line does not, and string match is case-sensitive.)
        verify_no_log_message -3 "*Failover auth NACK *" $election_line
        verify_no_log_message -3 "*cannot reach quorum*" $election_line
        assert_equal "slave" [s -3 role]
    }

    test "NACK for the current election is counted and fast-fails it" {
        set nack [create_cluster_failover_nack_packet $voter_id $voter_port $voter_cport \
            $election_epoch $election_epoch $NACK_REASON_REQ_EPOCH_OLD]
        send_cluster_bus_packet $candidate_cport $nack

        # 3 primaries, one FAIL, quorum 2: a single NACK leaves at most one
        # achievable ACK, so the election is reset immediately.
        wait_for_log_messages -3 [list "*Failover auth NACK * from $voter_id * for epoch $election_epoch (NACKs 1, quorum 2)*"] $election_line 1000 50
        wait_for_log_messages -3 [list "*Failover election for epoch $election_epoch cannot reach quorum*"] $election_line 1000 50
        verify_no_log_message -3 "*Failover attempt expired*" $election_line
    }

    test "Replica wins once the voters answer again" {
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

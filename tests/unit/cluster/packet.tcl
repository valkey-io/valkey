# Create a clusterbus message packet
proc create_cluster_meet_packet {sender_name sender_port sender_cport {count 0} {extensions 0} {mflags 0}} {
    # Constants
    set CLUSTER_NAMELEN 40
    set CLUSTER_SLOTS 16384
    set NET_IP_STR_LEN 46
    set CLUSTERMSG_TYPE_MEET 2
    set CLUSTERMSG_MIN_LEN 2256

    # Build the packet
    set packet ""

    # Signature "RCmb" (4 bytes)
    append packet "RCmb"

    # totlen (uint32_t) - will be updated at the end
    append packet [binary format I 0]

    # ver (uint16_t) - protocol version 1
    append packet [binary format S 1]

    # port (uint16_t)
    append packet [binary format S $sender_port]

    # type (uint16_t) - MEET
    append packet [binary format S $CLUSTERMSG_TYPE_MEET]

    # count (uint16_t)
    append packet [binary format S $count]

    # currentEpoch (uint64_t)
    append packet [binary format W 1]

    # configEpoch (uint64_t)
    append packet [binary format W 1]

    # offset (uint64_t)
    append packet [binary format W 0]

    # sender[40] - node name
    set sender_padded [string range "${sender_name}[string repeat "\x00" $CLUSTER_NAMELEN]" 0 [expr {$CLUSTER_NAMELEN - 1}]]
    append packet $sender_padded

    # myslots[2048] - all zeros
    append packet [string repeat "\x00" [expr {$CLUSTER_SLOTS / 8}]]

    # replicaof[40] - all zeros
    append packet [string repeat "\x00" $CLUSTER_NAMELEN]

    # myip[46] - all zeros
    append packet [string repeat "\x00" $NET_IP_STR_LEN]

    # extensions (uint16_t)
    append packet [binary format S $extensions]

    # notused1[30] - reserved
    append packet [string repeat "\x00" 30]

    # pport (uint16_t)
    append packet [binary format S 0]

    # cport (uint16_t) - cluster bus port
    append packet [binary format S $sender_cport]

    # flags (uint16_t) - CLUSTER_NODE_PRIMARY
    append packet [binary format S 1]

    # state (unsigned char) - CLUSTER_OK
    append packet [binary format c 0]

    # mflags[3] - message flags (WITH CLUSTERMSG_FLAG0_EXT_DATA flag set)
    # CLUSTERMSG_FLAG0_EXT_DATA = (1 << 2) = 4
    append packet [binary format ccc $mflags 0 0]

    # Update totlen
    set totlen [string length $packet]
    set packet [string replace $packet 4 7 [binary format I $totlen]]

    return $packet
}

# Create a clusterbus PUBLISH (type 4) packet with attacker-controlled
# channel_len / message_len fields. The fixed clusterMsg header is laid out
# identically to create_cluster_meet_packet; the clusterMsgDataPublish payload
# (channel_len uint32, message_len uint32, then bulk_data) follows it.
proc create_cluster_publish_packet {sender_name sender_port sender_cport channel_len message_len {bulk_data ""}} {
    # Constants
    set CLUSTER_NAMELEN 40
    set CLUSTER_SLOTS 16384
    set NET_IP_STR_LEN 46
    set CLUSTERMSG_TYPE_PUBLISH 4

    # Build the packet
    set packet ""

    # Signature "RCmb" (4 bytes)
    append packet "RCmb"

    # totlen (uint32_t) - will be overwritten with the (possibly wrapped) value
    append packet [binary format I 0]

    # ver (uint16_t) - protocol version 1
    append packet [binary format S 1]

    # port (uint16_t)
    append packet [binary format S $sender_port]

    # type (uint16_t) - PUBLISH
    append packet [binary format S $CLUSTERMSG_TYPE_PUBLISH]

    # count (uint16_t)
    append packet [binary format S 0]

    # currentEpoch (uint64_t)
    append packet [binary format W 0]

    # configEpoch (uint64_t)
    append packet [binary format W 0]

    # offset (uint64_t)
    append packet [binary format W 0]

    # sender[40] - node name
    set sender_padded [string range "${sender_name}[string repeat "\x00" $CLUSTER_NAMELEN]" 0 [expr {$CLUSTER_NAMELEN - 1}]]
    append packet $sender_padded

    # myslots[2048] - all zeros
    append packet [string repeat "\x00" [expr {$CLUSTER_SLOTS / 8}]]

    # replicaof[40] - all zeros
    append packet [string repeat "\x00" $CLUSTER_NAMELEN]

    # myip[46] - all zeros
    append packet [string repeat "\x00" $NET_IP_STR_LEN]

    # extensions (uint16_t)
    append packet [binary format S 0]

    # notused1[30] - reserved
    append packet [string repeat "\x00" 30]

    # pport (uint16_t)
    append packet [binary format S 0]

    # cport (uint16_t) - cluster bus port
    append packet [binary format S $sender_cport]

    # flags (uint16_t) - CLUSTER_NODE_PRIMARY
    append packet [binary format S 1]

    # state (unsigned char) - CLUSTER_OK
    append packet [binary format c 0]

    # mflags[3] - message flags
    append packet [binary format ccc 0 0 0]

    # --- clusterMsgDataPublish payload (union clusterMsgData) starts here ---
    # channel_len (uint32_t) - attacker controlled
    append packet [binary format I $channel_len]

    # message_len (uint32_t) - attacker controlled
    append packet [binary format I $message_len]

    # bulk_data (variable) - the channel + message bytes actually present
    append packet $bulk_data

    return $packet
}

start_cluster 1 0 {tags {external:skip cluster tls:skip}} {
    test "Forged PUBLISH packet with wrapped length does not crash the server" {
        set base_port [srv 0 port]
        set cluster_port [expr {$base_port + 10000}]
        # PUBLISH is only processed from a known sender; use our own id.
        set sender_node_id [R 0 cluster myid]

        # Sanity: server is responsive before the attack.
        assert_equal "PONG" [R 0 ping]

        # The vulnerable object creation only runs when a subscriber exists.
        set rd [valkey_deferring_client]
        subscribe $rd {attacker-channel}

        # channel_len 0xffffffff + message_len 1 wraps the 32-bit explen down to
        # the 2264-byte packet size. Payload carries only the two length fields.
        set packet [create_cluster_publish_packet $sender_node_id $base_port $cluster_port \
            0xffffffff 1 ""]

        # Set totlen to the wrapped value so it matches the computed explen.
        set packet [string replace $packet 4 7 [binary format I 2264]]
        assert_equal 2264 [string length $packet]

        # Send the forged packet to the cluster bus port.
        set sock [socket 127.0.0.1 $cluster_port]
        fconfigure $sock -translation binary -buffering none -blocking 1
        puts -nonewline $sock $packet
        flush $sock
        close $sock

        # Wait until the packet has reached the validator.
        wait_for_condition 1000 10 {
            [CI 0 cluster_stats_messages_publish_received] == 1
        } else {
            fail "Forged PUBLISH packet was never delivered to the validator"
        }

        # The packet must be rejected and the node must stay up.
        assert_equal "PONG" [R 0 ping]
        assert_match "*cluster_state:ok*" [R 0 cluster info]

        unsubscribe $rd {attacker-channel}
        $rd close
    }
}

start_cluster 1 0 {tags {external:skip cluster tls:skip}} {
    test "Packet with missing gossip messages don't cause invalid read" {
        set base_port [srv 0 port]
        set cluster_port [expr {$base_port + 10000}]
        set fake_node_id "abcdef1234567890abcdef1234567890abcdef12"

        # Get initial total messages received
        set info_before [R 0 cluster info]
        regexp {cluster_stats_messages_received:(\d+)} $info_before -> initial_received

        # Intentionally malformed packet: count=100 with no gossip data,
        # Create a packet with extensions=0 but CLUSTERMSG_FLAG0_EXT_DATA flag set
        # bogus extensions value, and EXT_DATA flag set.
        set packet [create_cluster_meet_packet $fake_node_id $base_port $cluster_port 100 2000000000 4]

        # Send the packet after configuring the socket to accept binary data
        set sock [socket 127.0.0.1 $cluster_port]
        fconfigure $sock -translation binary -buffering none -blocking 1
        puts -nonewline $sock $packet
        flush $sock
        close $sock

        wait_for_condition 1000 10 {
            [CI 0 cluster_stats_messages_received] == 1
        } else {
            fail "Packet was never received"
        }    
    }
}

start_cluster 10 0 {tags {external:skip cluster tls:skip}} {
    test "Gossip count scales with higher percentage of `cluster-message-gossip-perc`" {
        R 0 config set cluster-message-gossip-perc 80

        set base_port [srv 0 port]
        set cluster_port [expr {$base_port + 10000}]
        set packet [create_cluster_meet_packet \
            "fakenode2234567890fakenode2234567890fake12" \
            $base_port $cluster_port 0 0 0]

        set sock [socket 127.0.0.1 $cluster_port]
        fconfigure $sock -translation binary -buffering full -blocking 1
        puts -nonewline $sock $packet
        flush $sock

        set hdr [read $sock 16]
        binary scan $hdr @4I totlen
        set rest [read $sock [expr {$totlen - 16}]]
        set reply "${hdr}${rest}"
        close $sock

        binary scan $reply @12Su type
        binary scan $reply @14Su count

        assert_equal 1 $type
        # The exact count depends on the membership-table state at the moment
        # the reply is crafted (the fake MEET node may or may not be counted
        # yet), so allow some slack. The property under test is scaling: at 80%
        # gossip-perc the count must be far above the default-perc baseline
        # of 1-2 entries.
        assert_morethan_equal $count 5
        assert_lessthan_equal $count 10
    }
}

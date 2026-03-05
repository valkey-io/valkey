# Test that cluster bus messages with certain invalid packets are rejected
# and don't crash the system.

# Build a MEET packet for the cluster bus.
#   sender_name  - 40-char hex node ID
#   sender_port  - TCP port
#   sender_cport - cluster bus port
#   opts         - dict of overrides: count, extensions, mflags0, pad_to
#
# By default builds a valid packet (count=0, extensions=0, mflags=0,
# padded to CLUSTERMSG_MIN_LEN = 2256).  The original "malformed" test
# overrides count, extensions and mflags0 to craft an intentionally broken
# packet.
proc build_meet_packet {sender_name sender_port sender_cport {opts {}}} {
    set CLUSTER_NAMELEN 40
    set CLUSTER_SLOTS   16384
    set NET_IP_STR_LEN  46
    set CLUSTERMSG_TYPE_MEET 2
    set CLUSTERMSG_MIN_LEN   2256

    set count      [dict_get_or $opts count 0]
    set extensions [dict_get_or $opts extensions 0]
    set mflags0    [dict_get_or $opts mflags0 0]
    set pad_to     [dict_get_or $opts pad_to $CLUSTERMSG_MIN_LEN]

    set packet ""

    append packet "RCmb"                                ;# sig[4]
    append packet [binary format I 0]                   ;# totlen (patched below)
    append packet [binary format S 1]                   ;# ver
    append packet [binary format S $sender_port]        ;# port
    append packet [binary format S $CLUSTERMSG_TYPE_MEET] ;# type
    append packet [binary format S $count]              ;# count
    append packet [binary format W 1]                   ;# currentEpoch
    append packet [binary format W 1]                   ;# configEpoch
    append packet [binary format W 0]                   ;# offset

    # sender[40]
    set padded [string range "${sender_name}[string repeat "\x00" $CLUSTER_NAMELEN]" \
                0 [expr {$CLUSTER_NAMELEN - 1}]]
    append packet $padded

    append packet [string repeat "\x00" [expr {$CLUSTER_SLOTS / 8}]] ;# myslots
    append packet [string repeat "\x00" $CLUSTER_NAMELEN]            ;# replicaof
    append packet [string repeat "\x00" $NET_IP_STR_LEN]            ;# myip
    append packet [binary format S $extensions]                        ;# extensions
    append packet [string repeat "\x00" 30]                            ;# notused1
    append packet [binary format S 0]                                  ;# pport
    append packet [binary format S $sender_cport]                      ;# cport
    append packet [binary format S 1]                                  ;# flags (PRIMARY)
    append packet [binary format c 0]                                  ;# state
    append packet [binary format ccc $mflags0 0 0]                     ;# mflags[3]

    # Pad to the required length (server rejects totlen != explen).
    set cur_len [string length $packet]
    if {$cur_len < $pad_to} {
        append packet [string repeat "\x00" [expr {$pad_to - $cur_len}]]
    }

    # Patch totlen
    set totlen [string length $packet]
    set packet [string replace $packet 4 7 [binary format I $totlen]]
    return $packet
}

# dict get with a default value.
proc dict_get_or {d key default} {
    if {[dict exists $d $key]} { return [dict get $d $key] }
    return $default
}

# ---- Non-blocking cluster-bus reader helpers --------------------------------

# Accumulate up to $needed bytes into the upvar buffer from a non-blocking
# socket.  Returns 1 when the buffer has enough data.
proc read_into_buf {sock bufvar needed} {
    upvar $bufvar buf
    set have [string length $buf]
    if {$have < $needed} {
        append buf [read $sock [expr {$needed - $have}]]
    }
    return [expr {[string length $buf] >= $needed}]
}

# Read one full clusterMsg from a (possibly blocking) socket with a timeout.
# Returns a dict: type (uint16) and count (uint16).
proc read_cluster_msg {sock {timeout_ms 5000}} {
    fconfigure $sock -translation binary -blocking 0 -buffering full

    set buf ""
    set totlen 0
    set done 0

    fileevent $sock readable [list set ::_sock_readable 1]
    set after_id [after $timeout_ms [list set ::_sock_readable timeout]]

    while {!$done} {
        vwait ::_sock_readable
        if {$::_sock_readable eq "timeout"} {
            fileevent $sock readable {}
            error "Timed out waiting for cluster bus response"
        }
        if {$totlen == 0 && [read_into_buf $sock buf 16]} {
            binary scan $buf @4I totlen
            set totlen [expr {$totlen & 0xFFFFFFFF}]
        }
        if {$totlen > 0 && [read_into_buf $sock buf $totlen]} {
            set done 1
        }
    }

    after cancel $after_id
    fileevent $sock readable {}

    binary scan $buf @12Su type
    binary scan $buf @14Su count
    return [list type $type count $count]
}

# Send a MEET packet to a cluster bus port and return the parsed response.
# Caller gets back the dict from read_cluster_msg (type + count).
proc send_meet_and_read_reply {cluster_port sender_name sender_port sender_cport {packet_opts {}}} {
    set packet [build_meet_packet $sender_name $sender_port $sender_cport $packet_opts]

    set sock [socket 127.0.0.1 $cluster_port]
    fconfigure $sock -translation binary -buffering full -blocking 1
    puts -nonewline $sock $packet
    flush $sock

    set result [read_cluster_msg $sock]
    close $sock
    return $result
}

# ---- Tests ------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster tls:skip}} {
    test "Packet with missing gossip messages don't cause invalid read" {
        set base_port [srv 0 port]
        set cluster_port [expr {$base_port + 10000}]
        set fake_node_id "abcdef1234567890abcdef1234567890abcdef12"

        # Build an intentionally malformed packet: count=100 with no gossip
        # data, bogus extensions value, and EXT_DATA flag set.
        set packet [build_meet_packet $fake_node_id $base_port $cluster_port \
            {count 100 extensions 2000000000 mflags0 4}]

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

# Gossip-count bounding tests.
start_cluster 10 0 {tags {external:skip cluster tls:skip}} {
    test "Gossip count in PONG respects cluster-message-gossip-size" {
        R 0 config set cluster-message-gossip-size 20

        set base_port [srv 0 port]
        set cluster_port [expr {$base_port + 10000}]

        set result [send_meet_and_read_reply $cluster_port \
            "fakenode1234567890fakenode1234567890fake12" $base_port $cluster_port]

        assert_equal 1 [dict get $result type] ;# CLUSTERMSG_TYPE_PONG

        # 11 known nodes, 20% → floor(2.2)=2, clamped to min 3.
        assert_equal [dict get $result count] 3
    }

    test "Gossip count scales with higher percentage" {
        R 0 config set cluster-message-gossip-size 80

        set base_port [srv 0 port]
        set cluster_port [expr {$base_port + 10000}]

        set result [send_meet_and_read_reply $cluster_port \
            "fakenode2234567890fakenode2234567890fake12" $base_port $cluster_port]

        assert_equal 1 [dict get $result type] ;# CLUSTERMSG_TYPE_PONG

        # ~12 known nodes, 80% → overall=9, freshnodes=~10 → count ~9.
        assert_morethan [dict get $result count] 3
        assert_lessthan_equal [dict get $result count] 9
    }
}

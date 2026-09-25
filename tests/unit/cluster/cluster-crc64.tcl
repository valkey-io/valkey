# Offset of the crc field inside clusterMsg and the minimum length of a
# full-header message. Kept in sync with the static asserts in cluster code.
set ::crcmsg_crc_offset 2216
set ::crcmsg_min_len 2256

# Build a full-header cluster bus MEET packet carrying the given checksum.
proc create_cluster_meet_packet {sender_name sender_port sender_cport crc} {
    set CLUSTER_NAMELEN 40
    set CLUSTER_SLOTS 16384
    set NET_IP_STR_LEN 46
    set CLUSTERMSG_TYPE_MEET 2

    set packet ""
    append packet "RCmb"                                  ;# sig[4]
    append packet [binary format I 0]                     ;# totlen, patched below
    append packet [binary format S 1]                     ;# ver
    append packet [binary format S $sender_port]          ;# port
    append packet [binary format S $CLUSTERMSG_TYPE_MEET] ;# type
    append packet [binary format S 0]                     ;# count
    append packet [binary format W 1]                     ;# currentEpoch
    append packet [binary format W 1]                     ;# configEpoch
    append packet [binary format W 0]                     ;# offset
    append packet [string range "${sender_name}[string repeat "\x00" $CLUSTER_NAMELEN]" 0 [expr {$CLUSTER_NAMELEN - 1}]]
    append packet [string repeat "\x00" [expr {$CLUSTER_SLOTS / 8}]] ;# myslots
    append packet [string repeat "\x00" $CLUSTER_NAMELEN] ;# replicaof
    append packet [string repeat "\x00" $NET_IP_STR_LEN]  ;# myip
    append packet [binary format S 0]                     ;# extensions
    assert_equal $::crcmsg_crc_offset [string length $packet]
    append packet [binary format W $crc]                  ;# crc
    append packet [string repeat "\x00" 22]               ;# notused1
    append packet [binary format S 0]                     ;# pport
    append packet [binary format S $sender_cport]         ;# cport
    append packet [binary format S 1]                     ;# flags: primary
    append packet [binary format c 0]                     ;# state: ok
    append packet [binary format ccc 0 0 0]               ;# mflags[3]

    set totlen [string length $packet]
    assert_equal $::crcmsg_min_len $totlen
    return [string replace $packet 4 7 [binary format I $totlen]]
}

proc open_cluster_bus {id} {
    set sock [socket 127.0.0.1 [expr {[srv [expr {-1 * $id}] port] + 10000}]]
    fconfigure $sock -translation binary -buffering none -blocking 1
    return $sock
}

proc send_cluster_packet {id packet} {
    set sock [open_cluster_bus $id]
    puts -nonewline $sock $packet
    flush $sock
    close $sock
}

# Read one whole cluster bus message from an open cluster bus socket.
proc read_cluster_packet {sock} {
    set hdr [read $sock 8]
    binary scan $hdr @4I totlen
    return "${hdr}[read $sock [expr {$totlen - 8}]]"
}

start_cluster 1 0 {tags {external:skip cluster tls:skip}} {
    test "Cluster bus message with a wrong CRC64 is dropped" {
        set port [srv 0 port]
        set cport [expr {$port + 10000}]

        # A zeroed checksum means "no checksum", the backward-compatible path
        # for older senders, so this MEET is accepted and answered.
        set sock [open_cluster_bus 0]
        puts -nonewline $sock [create_cluster_meet_packet \
            "aaaaaa1234567890aaaaaa1234567890aaaaaa12" $port $cport 0]
        flush $sock
        set reply [read_cluster_packet $sock]
        close $sock

        binary scan $reply @12Su type
        assert_equal 1 $type ;# CLUSTERMSG_TYPE_PONG

        # The node computes a real checksum for what it sends. Without this the
        # whole feature could be a no-op and every other test would still pass.
        binary scan $reply "@$::crcmsg_crc_offset Wu" reply_crc
        assert {$reply_crc != 0}

        # A wrong checksum is detected before the message is processed, so the
        # sender never makes it into the cluster.
        set known [CI 0 cluster_known_nodes]
        set from_line [count_log_lines 0]
        send_cluster_packet 0 [create_cluster_meet_packet \
            "bbbbbb1234567890bbbbbb1234567890bbbbbb12" $port $cport 0x1234567812345678]

        wait_for_log_messages 0 {"*CRC mismatch*"} $from_line 1000 10
        assert_equal $known [CI 0 cluster_known_nodes]
    }
}
